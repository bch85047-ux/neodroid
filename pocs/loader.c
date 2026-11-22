// NeoDroid P1 PoC: 加载静态 AArch64 ELF，重写 svc -> thunk -> trampoline，
// 用宿主 syscall 分发执行。Linux/aarch64 上验证；同一套逻辑移植进 runtime。

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>
#include <elf.h>

extern void neodroid_trampoline(void);

static jmp_buf g_exit_jmp;
static long g_exit_code = 0;
static uint64_t g_image_lo = 0, g_image_hi = 0;

// ---------- syscall 分发（宿主侧实现） ----------
long neodroid_host_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    (void)a3; (void)a4; (void)a5;
    switch (nr) {
    case 64: { // write
        int fd = (int)a0;
        const void *buf = (const void *)a1;
        size_t cnt = (size_t)a2;
        if (fd == 1 || fd == 2) {
            fwrite(buf, 1, cnt, fd == 1 ? stdout : stderr);
            fflush(fd == 1 ? stdout : stderr);
        }
        return (long)cnt;
    }
    case 93:  // exit
    case 94:  // exit_group
        g_exit_code = a0;
        longjmp(g_exit_jmp, 1);
        return 0;
    case 172: // getpid
        return 42;
    case 178: // gettid
        return 43;
    default:
        fprintf(stderr, "[neodroid] 未实现 syscall %ld (a0=%ld a1=%ld a2=%ld)\n", nr, a0, a1, a2);
        return -38; // ENOSYS
    }
}

// ---------- 指令编码 ----------
static uint32_t em_movz(int rd, uint16_t imm) { return 0xD2800000u | ((uint32_t)imm << 5) | (uint32_t)rd; }
static uint32_t em_movk16(int rd, uint16_t imm) { return 0xF2A00000u | ((uint32_t)imm << 5) | (uint32_t)rd; }
static uint32_t em_movk32(int rd, uint16_t imm) { return 0xF2C00000u | ((uint32_t)imm << 5) | (uint32_t)rd; }
static uint32_t em_movk48(int rd, uint16_t imm) { return 0xF2E00000u | ((uint32_t)imm << 5) | (uint32_t)rd; }
static uint32_t em_br(int rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }
static uint32_t em_b(uint32_t from, uint32_t to) {
    int32_t off = ((int32_t)(to - from)) >> 2;
    return 0x14000000u | ((uint32_t)off & 0x03FFFFFFu);
}

static void emit_mov_imm(uint32_t **p, int rd, uint64_t value) {
    *(*p)++ = em_movz(rd, (uint16_t)(value & 0xFFFF));
    *(*p)++ = em_movk16(rd, (uint16_t)((value >> 16) & 0xFFFF));
    *(*p)++ = em_movk32(rd, (uint16_t)((value >> 32) & 0xFFFF));
    *(*p)++ = em_movk48(rd, (uint16_t)((value >> 48) & 0xFFFF));
}

// ---------- 构造每站点 thunk ----------
// thunk: mov x16, <site+4>; mov x17, <trampoline>; br x17
static uint32_t *emit_thunk(uint32_t *arena, uint32_t site_addr) {
    emit_mov_imm(&arena, 16, (uint64_t)(site_addr + 4));
    emit_mov_imm(&arena, 17, (uint64_t)(uintptr_t)neodroid_trampoline);
    *arena++ = em_br(17);
    return arena;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <guest.elf>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    uint8_t *image = malloc((size_t)fsize);
    if (fread(image, 1, (size_t)fsize, f) != (size_t)fsize) { perror("fread"); return 1; }
    fclose(f);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;
    if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_machine != EM_AARCH64) {
        fprintf(stderr, "不是 AArch64 ELF\n"); return 1;
    }
    printf("[neodroid] ELF: entry=0x%llx, phnum=%u type=%u\n",
           (unsigned long long)eh->e_entry, eh->e_phnum, eh->e_type);

    Elf64_Phdr *ph = (Elf64_Phdr *)(image + eh->e_phoff);

    // 计算加载区间
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    lo &= ~(page - 1);
    hi = (hi + page - 1) & ~(page - 1);
    uint64_t image_size = hi - lo;

    // thunk 区 + 栈
    uint64_t arena_size = 64 * 1024;
    uint64_t stack_size = 64 * 1024;
    uint64_t total = image_size + arena_size + stack_size + page;
    uint64_t base = lo;

    void *m = mmap((void *)base, total, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) { perror("mmap guest"); return 1; }
    if ((uint64_t)m != base) { fprintf(stderr, "mmap 未落在期望地址\n"); return 1; }

    // 拷贝各段
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        memcpy((void *)(uintptr_t)ph[i].p_vaddr, image + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset((void *)(uintptr_t)(ph[i].p_vaddr + ph[i].p_filesz), 0,
                   ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    g_image_lo = lo; g_image_hi = hi;

    // 扫描 svc #0 并重写
    uint32_t *arena = (uint32_t *)(uintptr_t)(hi + 64);
    int patched = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        uint64_t start = ph[i].p_vaddr;
        uint64_t end = ph[i].p_vaddr + ph[i].p_filesz;
        for (uint64_t a = start; a + 4 <= end; a += 4) {
            uint32_t insn = *(uint32_t *)(uintptr_t)a;
            if (insn == 0xD4000001u) { // svc #0
                uint32_t *thunk = arena;
                arena = emit_thunk(arena, (uint32_t)a);
                *(uint32_t *)(uintptr_t)a = em_b((uint32_t)a, (uint32_t)(uintptr_t)thunk);
                patched++;
            }
        }
    }
    printf("[neodroid] 已重写 %d 个 svc 站点\n", patched);

    // 栈
    uint64_t sp = hi + arena_size + stack_size + page - 64;
    sp &= ~0xFULL;
    *(uint64_t *)(uintptr_t)sp = 0; // argc=0

    printf("\n----- guest 输出 -----\n");
    fflush(stdout);

    if (setjmp(g_exit_jmp) == 0) {
        register uint64_t rsp __asm__("sp") = sp;
        __asm__ volatile(
            "mov x29, #0\n"
            "mov x30, #0\n"
            "mov sp, %0\n"
            "br %1\n"
            :: "r"(rsp), "r"((uint64_t)eh->e_entry) : "memory");
    }

    printf("\n----- guest 退出 code=%ld -----\n", g_exit_code);
    return (int)g_exit_code;
}