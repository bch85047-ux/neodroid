#include "neodroid_guest.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/types.h>
#endif

// ELF 常量（避免依赖 <elf.h> 在 iOS SDK 中的可用性差异）
#define ND_ELF_MAGIC0 0x7f
#define ND_ET_EXEC 2
#define ND_EM_AARCH64 183
#define ND_PT_LOAD 1
#define ND_PF_X 1
#define ND_PF_W 2
#define ND_PF_R 4

extern "C" void neodroid_trampoline(void);
extern "C" long neodroid_host_syscall(long nr, long a0, long a1, long a2,
                                      long a3, long a4, long a5);

namespace neodroid {
namespace {

std::jmp_buf g_exit_jmp;
long g_exit_code = 0;
std::string g_output;
pthread_once_t g_out_once = PTHREAD_ONCE_INIT;
pthread_mutex_t g_out_mutex;

void init_out_mutex() { pthread_mutex_init(&g_out_mutex, nullptr); }

struct Ehdr64 {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
};

struct Phdr64 {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

uint64_t page_size() { return static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }
uint64_t align_down(uint64_t v) { return v & ~(page_size() - 1); }
uint64_t align_up(uint64_t v) { return (v + page_size() - 1) & ~(page_size() - 1); }

// --- 指令编码 ---
uint32_t em_movz(int rd, uint16_t imm) { return 0xD2800000u | (uint32_t(imm) << 5) | uint32_t(rd); }
uint32_t em_movk16(int rd, uint16_t imm) { return 0xF2A00000u | (uint32_t(imm) << 5) | uint32_t(rd); }
uint32_t em_movk32(int rd, uint16_t imm) { return 0xF2C00000u | (uint32_t(imm) << 5) | uint32_t(rd); }
uint32_t em_movk48(int rd, uint16_t imm) { return 0xF2E00000u | (uint32_t(imm) << 5) | uint32_t(rd); }
uint32_t em_br(int rn) { return 0xD61F0000u | (uint32_t(rn) << 5); }
uint32_t em_b(uint32_t from, uint32_t to) {
    int32_t off = (int32_t)(to - from) >> 2;
    return 0x14000000u | (uint32_t(off)&0x03FFFFFFu);
}

void emit_mov_imm(uint32_t **p, int rd, uint64_t v) {
    *(*p)++ = em_movz(rd, uint16_t(v & 0xFFFF));
    *(*p)++ = em_movk16(rd, uint16_t((v >> 16) & 0xFFFF));
    *(*p)++ = em_movk32(rd, uint16_t((v >> 32) & 0xFFFF));
    *(*p)++ = em_movk48(rd, uint16_t((v >> 48) & 0xFFFF));
}

uint32_t *emit_thunk(uint32_t *arena, uint32_t site_addr) {
    emit_mov_imm(&arena, 16, uint64_t(site_addr) + 4);
    emit_mov_imm(&arena, 17, uint64_t(uintptr_t(neodroid_trampoline)));
    *arena++ = em_br(17);
    return arena;
}

}  // namespace

// 宿主 syscall 分发（被 trampoline 调用）
extern "C" long neodroid_host_syscall(long nr, long a0, long a1, long a2,
                                      long a3, long a4, long a5) {
    (void)a3; (void)a4; (void)a5;
    switch (nr) {
    case 64: {  // write
        const char *buf = reinterpret_cast<const char *>(a1);
        size_t cnt = size_t(a2);
        if (a0 == 1 || a0 == 2) {
            pthread_once(&g_out_once, init_out_mutex);
            pthread_mutex_lock(&g_out_mutex);
            g_output.append(buf, cnt);
            pthread_mutex_unlock(&g_out_mutex);
            return long(cnt);
        }
        return -9;  // EBADF
    }
    case 93:  // exit
    case 94:  // exit_group
        g_exit_code = a0;
        std::longjmp(g_exit_jmp, 1);
        return 0;
    case 172: return 42;   // getpid
    case 178: return 43;   // gettid
    case 113: {            // clock_gettime
        struct TS { long sec; long nsec; } ts{};
        ts.sec = long(time(nullptr));
        if (a1) *reinterpret_cast<TS *>(a1) = ts;
        return 0;
    }
    default:
        return -38;  // ENOSYS
    }
}

GuestRunResult run_guest_elf(const std::vector<std::byte> &elf_image) {
    GuestRunResult result;

    if (elf_image.size() < sizeof(Ehdr64)) {
        result.error = "ELF 镜像过小";
        return result;
    }
    const auto *eh = reinterpret_cast<const Ehdr64 *>(elf_image.data());
    if (eh->ident[0] != ND_ELF_MAGIC0 || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        result.error = "不是 ELF 文件";
        return result;
    }
    if (eh->machine != ND_EM_AARCH64) {
        result.error = "不是 AArch64 ELF";
        return result;
    }
    if (eh->phnum == 0 || eh->phoff + uint64_t(eh->phnum) * sizeof(Phdr64) > elf_image.size()) {
        result.error = "程序头表越界";
        return result;
    }

    const auto *ph = reinterpret_cast<const Phdr64 *>(elf_image.data() + eh->phoff);

    uint64_t lo = ~0ull, hi = 0;
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != ND_PT_LOAD) continue;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo) {
        result.error = "没有可加载段";
        return result;
    }
    lo = align_down(lo);
    hi = align_up(hi);

    const uint64_t arena_size = 256 * 1024;
    const uint64_t stack_size = 512 * 1024;
    const uint64_t total = (hi - lo) + arena_size + stack_size;

    // 1) 先以 RW 映射（iOS 上避免匿名 RWX）
    void *base = mmap(reinterpret_cast<void *>(lo), total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (base == MAP_FAILED || reinterpret_cast<uintptr_t>(base) != lo) {
        result.error = "mmap 客户机内存失败（目标地址不可用）";
        return result;
    }

    // 2) 拷贝各段
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != ND_PT_LOAD) continue;
        if (ph[i].offset + ph[i].filesz > elf_image.size()) {
            result.error = "段文件数据越界";
            return result;
        }
        std::memcpy(reinterpret_cast<void *>(uintptr_t(ph[i].vaddr)),
                    elf_image.data() + ph[i].offset, size_t(ph[i].filesz));
        if (ph[i].memsz > ph[i].filesz) {
            std::memset(reinterpret_cast<void *>(uintptr_t(ph[i].vaddr + ph[i].filesz)),
                        0, size_t(ph[i].memsz - ph[i].filesz));
        }
    }

    // 3) 重写 svc 站点
    auto *arena = reinterpret_cast<uint32_t *>(uintptr_t(hi + 64));
    int patched = 0;
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != ND_PT_LOAD || !(ph[i].flags & ND_PF_X)) continue;
        uint64_t start = ph[i].vaddr;
        uint64_t end = ph[i].vaddr + ph[i].filesz;
        for (uint64_t a = start; a + 4 <= end; a += 4) {
            auto *word = reinterpret_cast<uint32_t *>(uintptr_t(a));
            if (*word == 0xD4000001u) {  // svc #0
                uint32_t *thunk = arena;
                arena = emit_thunk(arena, uint32_t(a));
                *word = em_b(uint32_t(a), uint32_t(uintptr_t(thunk)));
                patched++;
            }
        }
    }
    if (patched == 0) {
        result.error = "没有找到 svc 站点（可能该 ELF 不使用 syscall）";
        return result;
    }

    // 4) 把可执行区间与 thunk 区设为 RX
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != ND_PT_LOAD || !(ph[i].flags & ND_PF_X)) continue;
        uint64_t s = align_down(ph[i].vaddr);
        uint64_t e = align_up(ph[i].vaddr + ph[i].memsz);
        if (mprotect(reinterpret_cast<void *>(uintptr_t(s)), size_t(e - s),
                     PROT_READ | PROT_EXEC) != 0) {
            result.error = "mprotect(RX) 失败——iOS 上需要 JIT/可执行内存 entitlement";
            return result;
        }
    }
    uint64_t arena_lo = align_down(hi + 64);
    uint64_t arena_hi = align_up(hi + 64 + arena_size);
    if (mprotect(reinterpret_cast<void *>(uintptr_t(arena_lo)), size_t(arena_hi - arena_lo),
                 PROT_READ | PROT_EXEC) != 0) {
        result.error = "mprotect(thunk 区) 失败";
        return result;
    }

    // 5) 设栈并执行
    uint64_t sp = hi + arena_size + stack_size - 64;
    sp &= ~0xFull;
    *reinterpret_cast<uint64_t *>(uintptr_t(sp)) = 0;  // argc = 0

    g_output.clear();
    g_exit_code = 0;

    if (setjmp(g_exit_jmp) == 0) {
        uint64_t entry = eh->entry;
        __asm__ volatile(
            "mov x29, #0\n"
            "mov x30, #0\n"
            "mov sp, %0\n"
            "br %1\n" ::"r"(sp), "r"(entry)
            : "memory");
        // 不应返回
        result.error = "guest 异常返回到宿主";
        return result;
    }

    result.ok = true;
    result.exit_code = int(g_exit_code);
    result.output = g_output;
    return result;
}

}  // namespace neodroid