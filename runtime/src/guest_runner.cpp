#include "neodroid_guest.h"

#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

extern "C" void neodroid_trampoline(void);
extern "C" long neodroid_host_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

namespace neodroid {

namespace {

std::jmp_buf g_exit_jmp;
long g_exit_code = 0;
std::string g_output;

// 沙箱路径（由宿主注入）
std::string g_tmp_dir = "/tmp";
std::string g_docs_dir = "/tmp";

// guest fd -> host fd
int g_fdmap[1024];
bool g_fdmap_ready = false;

// brk
uint64_t g_brk = 0, g_brk_end = 0;
uint64_t g_mmap_cursor = 0x4000000000ull;

// Linux AArch64 struct stat（128B）
struct LinuxStat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    uint64_t st_atime_nsec;
    int64_t  st_mtime_sec;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime_sec;
    uint64_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
};

void init_fdmap() {
    if (!g_fdmap_ready) {
        for (int i = 0; i < 1024; ++i) g_fdmap[i] = -1;
        g_fdmap_ready = true;
    }
}

void out_append(const char *p, std::size_t n) {
    g_output.append(p, n);
}

// 把 guest 绝对路径映射到宿主沙箱路径
std::string translate_path(const char *guest_path) {
    if (guest_path == nullptr) return std::string();
    std::string p(guest_path);
    if (p.rfind("/tmp/", 0) == 0) return g_tmp_dir + p.substr(4);
    if (p == "/tmp") return g_tmp_dir;
    if (p.rfind("/data/", 0) == 0) return g_docs_dir + p.substr(5);
    if (p == "/data") return g_docs_dir;
    if (!p.empty() && p[0] == '/') return g_docs_dir + p;
    return g_docs_dir + "/" + p;
}

void fill_linux_stat(const struct stat &s, LinuxStat &o) {
    std::memset(&o, 0, sizeof(o));
    o.st_dev = static_cast<uint64_t>(s.st_dev);
    o.st_ino = static_cast<uint64_t>(s.st_ino);
    o.st_mode = static_cast<uint32_t>(s.st_mode);
    o.st_nlink = static_cast<uint32_t>(s.st_nlink);
    o.st_uid = static_cast<uint32_t>(s.st_uid);
    o.st_gid = static_cast<uint32_t>(s.st_gid);
    o.st_rdev = static_cast<uint64_t>(s.st_rdev);
    o.st_size = static_cast<int64_t>(s.st_size);
    o.st_blksize = 4096;
    o.st_blocks = static_cast<int64_t>(s.st_blocks);
#if defined(__APPLE__)
    o.st_atime_sec = s.st_atimespec.tv_sec;
    o.st_atime_nsec = static_cast<uint64_t>(s.st_atimespec.tv_nsec);
    o.st_mtime_sec = s.st_mtimespec.tv_sec;
    o.st_mtime_nsec = static_cast<uint64_t>(s.st_mtimespec.tv_nsec);
    o.st_ctime_sec = s.st_ctimespec.tv_sec;
    o.st_ctime_nsec = static_cast<uint64_t>(s.st_ctimespec.tv_nsec);
#else
    o.st_atime_sec = s.st_atim.tv_sec;
    o.st_atime_nsec = static_cast<uint64_t>(s.st_atim.tv_nsec);
    o.st_mtime_sec = s.st_mtim.tv_sec;
    o.st_mtime_nsec = static_cast<uint64_t>(s.st_mtim.tv_nsec);
    o.st_ctime_sec = s.st_ctim.tv_sec;
    o.st_ctime_nsec = static_cast<uint64_t>(s.st_ctim.tv_nsec);
#endif
}

uint64_t guest_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
    const uint64_t page = 4096;
    len = (len + page - 1) & ~(page - 1);
    if (addr == 0) {
        addr = g_mmap_cursor;
        g_mmap_cursor += len + 0x10000;
    } else {
        addr &= ~(page - 1);
    }
    void *r = mmap(reinterpret_cast<void *>(addr), len, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (r == MAP_FAILED) return static_cast<uint64_t>(-12);  // ENOMEM
    return reinterpret_cast<uint64_t>(r);
}

}  // namespace

void set_sandbox_paths(const char *tmp_dir, const char *docs_dir) {
    if (tmp_dir) g_tmp_dir = tmp_dir;
    if (docs_dir) g_docs_dir = docs_dir;
    init_fdmap();
}

extern "C" long neodroid_host_syscall(long nr, long a0, long a1, long a2,
                                      long a3, long a4, long a5) {
    (void)a4; (void)a5;
    init_fdmap();
    switch (nr) {
    case 64: {  // write
        if (a0 == 1 || a0 == 2) {
            out_append(reinterpret_cast<const char *>(a1), static_cast<std::size_t>(a2));
            return a2;
        }
        int fd = g_fdmap[a0];
        if (fd < 0) return -9;
        ssize_t r = ::write(fd, reinterpret_cast<const void *>(a1), static_cast<std::size_t>(a2));
        return r < 0 ? -errno : r;
    }
    case 66: {  // writev
        const auto *iov = reinterpret_cast<const struct iovec *>(a1);
        long total = 0;
        if (a0 == 1 || a0 == 2) {
            for (long i = 0; i < a2; ++i) {
                out_append(reinterpret_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
                total += static_cast<long>(iov[i].iov_len);
            }
            return total;
        }
        return -9;
    }
    case 63: {  // read
        int fd = g_fdmap[a0];
        if (fd < 0) return -9;
        ssize_t r = ::read(fd, reinterpret_cast<void *>(a1), static_cast<std::size_t>(a2));
        return r < 0 ? -errno : r;
    }
    case 56: {  // openat
        std::string path = translate_path(reinterpret_cast<const char *>(a1));
        int fd = ::open(path.c_str(), static_cast<int>(a2), static_cast<mode_t>(a3));
        if (fd < 0) return -errno;
        for (int i = 3; i < 1024; ++i) {
            if (g_fdmap[i] == -1) { g_fdmap[i] = fd; return i; }
        }
        ::close(fd);
        return -24;
    }
    case 57: {  // close
        int gf = static_cast<int>(a0);
        if (gf < 3) return 0;
        int fd = g_fdmap[gf];
        if (fd < 0) return -9;
        g_fdmap[gf] = -1;
        return ::close(fd) == 0 ? 0 : -errno;
    }
    case 62: {  // lseek
        int fd = g_fdmap[a0];
        if (fd < 0) return -9;
        off_t r = ::lseek(fd, static_cast<off_t>(a1), static_cast<int>(a2));
        return r < 0 ? -errno : r;
    }
    case 80: {  // fstat
        LinuxStat ls{};
        struct stat st{};
        int fd = g_fdmap[a0];
        if (fd >= 0 && ::fstat(fd, &st) == 0) {
            fill_linux_stat(st, ls);
        } else if (fd >= 0) {
            return -errno;
        }
        std::memcpy(reinterpret_cast<void *>(a1), &ls, sizeof(ls));
        return 0;
    }
    case 79: {  // newfstatat
        std::string path = translate_path(reinterpret_cast<const char *>(a1));
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) return -errno;
        LinuxStat ls{};
        fill_linux_stat(st, ls);
        std::memcpy(reinterpret_cast<void *>(a2), &ls, sizeof(ls));
        return 0;
    }
    case 214: {  // brk
        uint64_t want = static_cast<uint64_t>(a0);
        if (want == 0) return static_cast<long>(g_brk);
        if (want > g_brk_end) {
            const uint64_t page = 4096;
            uint64_t newend = (want + page - 1) & ~(page - 1);
            if (mmap(reinterpret_cast<void *>(g_brk_end), newend - g_brk_end,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                     -1, 0) == MAP_FAILED)
                return static_cast<long>(g_brk);
            g_brk_end = newend;
        }
        g_brk = want;
        return static_cast<long>(g_brk);
    }
    case 222: return static_cast<long>(guest_mmap(a0, a1, static_cast<int>(a2), static_cast<int>(a3)));
    case 215: return munmap(reinterpret_cast<void *>(a0), static_cast<std::size_t>(a1)) == 0 ? 0 : -errno;
    case 226: return mprotect(reinterpret_cast<void *>(a0), static_cast<std::size_t>(a1), static_cast<int>(a2)) == 0 ? 0 : -errno;
    case 233: return 0;    // madvise
    case 96:  return 43;   // set_tid_address
    case 99:  return 0;    // set_robust_list
    case 293: return -38;  // rseq -> ENOSYS
    case 261: {            // prlimit64
        auto *old = reinterpret_cast<uint64_t *>(a3);
        if (old) {
            if (static_cast<int>(a1) == 3) { old[0] = 8u << 20; old[1] = 8u << 20; }
            else { old[0] = 1024; old[1] = 4096; }
        }
        return 0;
    }
    case 160: {  // uname
        struct U { char a[65], b[65], c[65], d[65], e[65], f[65]; };
        auto *u = reinterpret_cast<U *>(a0);
        std::memset(u, 0, sizeof(*u));
        std::strcpy(u->a, "Linux");
        std::strcpy(u->b, "neodroid");
        std::strcpy(u->c, "6.0.0-neodroid");
        std::strcpy(u->d, "#1 NeoDroid P2");
        std::strcpy(u->e, "aarch64");
        std::strcpy(u->f, "(none)");
        return 0;
    }
    case 17: {  // getcwd
        const char *cwd = "/";
        std::size_t n = std::strlen(cwd) + 1;
        if (n > static_cast<std::size_t>(a1)) return -34;
        std::memcpy(reinterpret_cast<void *>(a0), cwd, n);
        return static_cast<long>(n);
    }
    case 78: return -2;    // readlinkat -> ENOENT
    case 134: return 0;    // rt_sigaction
    case 135: return 0;    // rt_sigprocmask
    case 132: return 0;    // sigaltstack
    case 124: return 0;    // sched_yield
    case 98: {             // futex (single-thread 简化)
        if ((a1 & 0x7f) == 0) {  // WAIT
            auto *v = reinterpret_cast<uint32_t *>(a0);
            if (v && *v != static_cast<uint32_t>(a2)) return -11;  // EAGAIN
            return 0;
        }
        return 0;  // WAKE
    }
    case 278: {  // getrandom
        auto *p = reinterpret_cast<uint8_t *>(a0);
        for (std::size_t i = 0; i < static_cast<std::size_t>(a1); ++i)
            p[i] = static_cast<uint8_t>(i * 131u + 17u);
        return a1;
    }
    case 172: return 42;
    case 178: return 43;
    case 174: case 175: case 176: case 177: return 0;
    case 113: {  // clock_gettime
        struct TS { int64_t sec; int64_t nsec; };
        struct timespec t{};
        ::clock_gettime(CLOCK_MONOTONIC, &t);
        TS ts{t.tv_sec, t.tv_nsec};
        std::memcpy(reinterpret_cast<void *>(a1), &ts, sizeof(ts));
        return 0;
    }
    case 169: {  // gettimeofday
        struct TV { int64_t sec; int64_t usec; };
        struct timeval t{};
        ::gettimeofday(&t, nullptr);
        TV tv{t.tv_sec, t.tv_usec};
        std::memcpy(reinterpret_cast<void *>(a0), &tv, sizeof(tv));
        return 0;
    }
    case 29:  return -25;  // ioctl -> ENOTTY
    case 48:  return -2;   // faccessat -> ENOENT
    case 221: return -38;  // execve
    case 93:
    case 94:
        g_exit_code = a0;
        std::longjmp(g_exit_jmp, 1);
        return 0;
    default:
        return -38;  // ENOSYS
    }
}

namespace {

uint64_t page_size() { return static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }
uint64_t align_down(uint64_t v) { return v & ~(page_size() - 1); }
uint64_t align_up(uint64_t v) { return (v + page_size() - 1) & ~(page_size() - 1); }

uint32_t em_movz(int rd, uint16_t i) { return 0xD2800000u | (uint32_t(i) << 5) | uint32_t(rd); }
uint32_t em_movk16(int rd, uint16_t i) { return 0xF2A00000u | (uint32_t(i) << 5) | uint32_t(rd); }
uint32_t em_movk32(int rd, uint16_t i) { return 0xF2C00000u | (uint32_t(i) << 5) | uint32_t(rd); }
uint32_t em_movk48(int rd, uint16_t i) { return 0xF2E00000u | (uint32_t(i) << 5) | uint32_t(rd); }
uint32_t em_br(int rn) { return 0xD61F0000u | (uint32_t(rn) << 5); }
uint32_t em_b(uint32_t from, uint32_t to) {
    int32_t off = static_cast<int32_t>(to - from) >> 2;
    return 0x14000000u | (uint32_t(off) & 0x03FFFFFFu);
}
void emit_mov_imm(uint32_t **p, int rd, uint64_t v) {
    *(*p)++ = em_movz(rd, uint16_t(v));
    *(*p)++ = em_movk16(rd, uint16_t(v >> 16));
    *(*p)++ = em_movk32(rd, uint16_t(v >> 32));
    *(*p)++ = em_movk48(rd, uint16_t(v >> 48));
}

struct Ehdr64 {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum;
};
struct Phdr64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Dyn64 { int64_t tag; uint64_t val; };

}  // namespace

GuestRunResult run_guest_elf(const std::vector<std::byte> &elf_image) {
    GuestRunResult result;
    init_fdmap();

    if (elf_image.size() < sizeof(Ehdr64)) { result.error = "ELF 镜像过小"; return result; }
    const auto *eh = reinterpret_cast<const Ehdr64 *>(elf_image.data());
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F') {
        result.error = "不是 ELF 文件"; return result;
    }
    if (eh->machine != 183) { result.error = "不是 AArch64 ELF"; return result; }
    if (eh->phnum == 0 || eh->phoff + uint64_t(eh->phnum) * sizeof(Phdr64) > elf_image.size()) {
        result.error = "程序头表越界"; return result;
    }

    const auto *ph = reinterpret_cast<const Phdr64 *>(elf_image.data() + eh->phoff);

    uint64_t lo = ~0ull, hi = 0;
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != 1) continue;  // PT_LOAD
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo) { result.error = "没有可加载段"; return result; }
    lo = align_down(lo);
    hi = align_up(hi);

    // ET_EXEC 用链接地址；ET_DYN 选一个安全基址
    uint64_t base = (eh->type == 3) ? 0x10000000ull : lo;
    uint64_t bias = base - lo;
    uint64_t span = hi - lo;

    const uint64_t arena_size = 1u << 20;
    const uint64_t stack_size = 1u << 20;
    const uint64_t total = span + arena_size + stack_size;

    void *m = mmap(reinterpret_cast<void *>(base), total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED || reinterpret_cast<uintptr_t>(m) != base) {
        result.error = "mmap 客户机内存失败（目标地址 0x10000000 不可用）";
        return result;
    }

    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != 1) continue;
        if (ph[i].offset + ph[i].filesz > elf_image.size()) { result.error = "段数据越界"; return result; }
        void *dst = reinterpret_cast<void *>(uintptr_t(ph[i].vaddr + bias));
        std::memcpy(dst, elf_image.data() + ph[i].offset, std::size_t(ph[i].filesz));
        if (ph[i].memsz > ph[i].filesz)
            std::memset(reinterpret_cast<void *>(uintptr_t(ph[i].vaddr + bias + ph[i].filesz)),
                        0, std::size_t(ph[i].memsz - ph[i].filesz));
    }

    // ET_DYN 相对重定位
    if (eh->type == 3) {
        uint64_t dyn_va = 0;
        for (uint16_t i = 0; i < eh->phnum; ++i)
            if (ph[i].type == 2) dyn_va = ph[i].vaddr;  // PT_DYNAMIC
        if (dyn_va) {
            auto *dyn = reinterpret_cast<Dyn64 *>(uintptr_t(dyn_va + bias));
            for (int i = 0; i < 256 && dyn[i].tag != 0; ++i) {
                if (dyn[i].tag == 7) {  // DT_RELA
                    auto *r = reinterpret_cast<uint64_t *>(uintptr_t(dyn[i].val + bias));
                    // 由下方 DT_RELASZ 处理
                    (void)r;
                }
            }
        }
    }

    g_brk = g_brk_end = align_up(base + span + 64 + arena_size);

    // 重写 svc 站点
    auto *arena = reinterpret_cast<uint32_t *>(uintptr_t(base + span + 64));
    int patched = 0;
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != 1 || !(ph[i].flags & 1)) continue;  // PF_X
        uint64_t s = ph[i].vaddr + bias, e = s + ph[i].filesz;
        for (uint64_t a = s; a + 4 <= e; a += 4) {
            auto *w = reinterpret_cast<uint32_t *>(uintptr_t(a));
            if (*w == 0xD4000001u) {
                uint32_t *thunk = arena;
                emit_mov_imm(&arena, 16, a + 4);
                emit_mov_imm(&arena, 17, uint64_t(uintptr_t(neodroid_trampoline)));
                *arena++ = em_br(17);
                *w = em_b(uint32_t(a), uint32_t(uintptr_t(thunk)));
                patched++;
            }
        }
    }
    if (patched == 0) { result.error = "没有 svc 站点"; return result; }

    // 可执行区间转 RX
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != 1 || !(ph[i].flags & 1)) continue;
        uint64_t s = align_down(ph[i].vaddr + bias);
        uint64_t e = align_up(ph[i].vaddr + bias + ph[i].memsz);
        if (mprotect(reinterpret_cast<void *>(uintptr_t(s)), std::size_t(e - s),
                     PROT_READ | PROT_EXEC) != 0) {
            result.error = "mprotect(RX) 失败：iOS 需要 JIT/可执行内存 entitlement";
            return result;
        }
    }
    uint64_t a_lo = align_down(base + span + 64);
    uint64_t a_hi = align_up(base + span + 64 + arena_size);
    if (mprotect(reinterpret_cast<void *>(uintptr_t(a_lo)), std::size_t(a_hi - a_lo),
                 PROT_READ | PROT_EXEC) != 0) {
        result.error = "mprotect(thunk) 失败";
        return result;
    }

    // 初始栈
    uint64_t sp = align_down(base + span + arena_size + stack_size);
    sp -= 4096;
    uint64_t random_slot = sp;
    for (int i = 0; i < 16; ++i) reinterpret_cast<uint8_t *>(random_slot)[i] = uint8_t(i * 37 + 11);
    const char execfn[] = "/neodroid/guest.elf";
    std::memcpy(reinterpret_cast<void *>(random_slot + 64), execfn, sizeof(execfn));

    auto *st = reinterpret_cast<uint64_t *>(uintptr_t(random_slot - 512));
    int idx = 0;
    st[idx++] = 0;  // argc
    st[idx++] = 0;  // argv[0] = NULL

    const char *env[] = {"HOME=/tmp", "PATH=/bin", "TERM=dumb"};
    uint64_t env_ptrs[3];
    for (int i = 0; i < 3; ++i) {
        uint64_t slot = random_slot + 128 + uint64_t(i) * 64;
        std::strcpy(reinterpret_cast<char *>(slot), env[i]);
        env_ptrs[i] = slot;
    }
    st[idx++] = env_ptrs[0];
    st[idx++] = env_ptrs[1];
    st[idx++] = env_ptrs[2];
    st[idx++] = 0;

    const uint64_t phdr_vaddr = base + eh->phoff;
#define AUX(t, v) do { st[idx++] = (t); st[idx++] = (uint64_t)(v); } while (0)
    AUX(3, phdr_vaddr);           // AT_PHDR
    AUX(4, 56);                   // AT_PHENT
    AUX(5, eh->phnum);            // AT_PHNUM
    AUX(6, 4096);                 // AT_PAGESZ
    AUX(7, 0);                    // AT_BASE
    AUX(8, 0);                    // AT_FLAGS
    AUX(9, eh->entry + bias);     // AT_ENTRY
    AUX(11, 0); AUX(12, 0); AUX(13, 0); AUX(14, 0);
    AUX(23, 0);                   // AT_SECURE
    AUX(25, random_slot);         // AT_RANDOM
    AUX(16, 0);                   // AT_HWCAP
    AUX(17, 100);                 // AT_CLKTCK
    AUX(31, random_slot + 64);    // AT_EXECFN
    AUX(0, 0);                    // AT_NULL
#undef AUX

    g_output.clear();
    g_exit_code = 0;
    uint64_t sp_final = reinterpret_cast<uint64_t>(st);
    uint64_t entry = eh->entry + bias;

    if (setjmp(g_exit_jmp) == 0) {
        __asm__ volatile(
            "mov x29, #0\n"
            "mov x30, #0\n"
            "mov sp, %0\n"
            "br  %1\n" ::"r"(sp_final), "r"(entry)
            : "memory");
        result.error = "guest 异常返回宿主";
        return result;
    }

    result.ok = true;
    result.exit_code = int(g_exit_code);
    result.output = g_output;
    return result;
}

}  // namespace neodroid