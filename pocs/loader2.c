// NeoDroid P2 PoC：加载 static AArch64 libc ELF，构造 auxv 启动栈，
// 实现 libc 级 syscall（brk/mmap/openat/read/write/fstat/...），用 svc 重写执行。

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <elf.h>

extern void neodroid_trampoline(void);

static jmp_buf g_exit_jmp;
static long g_exit_code;
static uint64_t g_brk = 0, g_brk_end = 0;
static uint64_t g_mmap_cursor = 0x2000000000ull;

static char *g_out = NULL;
static size_t g_out_len = 0, g_out_cap = 0;

static int g_fdmap[1024];

static void out_append(const char *p, size_t n) {
    if (g_out_len + n + 1 > g_out_cap) {
        g_out_cap = (g_out_len + n + 1) * 2 + 4096;
        g_out = realloc(g_out, g_out_cap);
    }
    memcpy(g_out + g_out_len, p, n);
    g_out_len += n;
    g_out[g_out_len] = 0;
}

static uint64_t guest_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
    uint64_t page = 4096;
    len = (len + page - 1) & ~(page - 1);
    if (addr == 0 && (flags & MAP_FIXED)) addr = g_mmap_cursor;
    if (addr == 0) {
        addr = g_mmap_cursor;
        g_mmap_cursor += len + 0x10000;
    } else {
        addr &= ~(page - 1);
    }
    void *r = mmap((void *)addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (r == MAP_FAILED) return (uint64_t)-12; // ENOMEM
    return (uint64_t)r;
}

static int g_trace = -1;
long neodroid_host_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    if (g_trace < 0) g_trace = getenv("ND_TRACE") ? 1 : 0;
    if (g_trace) fprintf(stderr, "[sc] %ld (0x%lx,0x%lx,0x%lx)\n", nr, a0, a1, a2);

    switch (nr) {
    case 64: { // write
        if (a0 == 1 || a0 == 2) { out_append((const char *)a1, (size_t)a2); return a2; }
        int fd = g_fdmap[(int)a0];
        if (fd < 0) return -9;
        ssize_t r = write(fd, (void *)a1, (size_t)a2);
        return r < 0 ? -errno : r;
    }
    case 66: { // writev
        if (a0 == 1 || a0 == 2) {
            const struct iovec *iov = (const struct iovec *)a1;
            long total = 0;
            for (long i = 0; i < a2; ++i) { out_append((const char *)iov[i].iov_base, iov[i].iov_len); total += iov[i].iov_len; }
            return total;
        }
        return -9;
    }
    case 63: { // read
        int fd = g_fdmap[(int)a0];
        if (fd < 0) return -9;
        ssize_t r = read(fd, (void *)a1, (size_t)a2);
        return r < 0 ? -errno : r;
    }
    case 56: { // openat
        const char *path = (const char *)a1;
        int fd = openat((int)a0, path, (int)a2, (mode_t)a3);
        if (fd < 0) return -errno;
        for (int i = 3; i < 1024; ++i) if (g_fdmap[i] == -1) { g_fdmap[i] = fd; return i; }
        close(fd); return -24;
    }
    case 57: { // close
        int gf = (int)a0;
        if (gf < 3) return 0;
        int fd = g_fdmap[gf];
        if (fd < 0) return -9;
        g_fdmap[gf] = -1;
        return close(fd) == 0 ? 0 : -errno;
    }
    case 62: { // lseek
        int fd = g_fdmap[(int)a0];
        if (fd < 0) return -9;
        off_t r = lseek(fd, (off_t)a1, (int)a2);
        return r < 0 ? -errno : r;
    }
    case 80: { // fstat
        int fd = g_fdmap[(int)a0];
        struct stat st;
        if (fd < 0) {
            memset(&st, 0, sizeof(st));
        } else if (fstat(fd, &st) != 0) {
            return -errno;
        }
        memcpy((void *)a1, &st, sizeof(st));
        return 0;
    }
    case 79: { // newfstatat
        struct stat st;
        if (fstatat((int)a0, (const char *)a1, &st, (int)a3) != 0) return -errno;
        memcpy((void *)a2, &st, sizeof(st));
        return 0;
    }
    case 214: { // brk
        uint64_t want = (uint64_t)a0;
        if (want == 0) return (long)g_brk;
        if (want > g_brk_end) {
            uint64_t page = 4096;
            uint64_t newend = (want + page - 1) & ~(page - 1);
            if (mmap((void *)g_brk_end, newend - g_brk_end, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
                return (long)g_brk;
            g_brk_end = newend;
        }
        g_brk = want;
        return (long)g_brk;
    }
    case 222: return (long)guest_mmap((uint64_t)a0, (uint64_t)a1, (int)a2, (int)a3);
    case 215: return munmap((void *)a0, (size_t)a1) == 0 ? 0 : -errno;
    case 226: return mprotect((void *)a0, (size_t)a1, (int)a2) == 0 ? 0 : -errno;
    case 233: return 0; // madvise
    case 96:  return 43; // set_tid_address
    case 99:  return 0;  // set_robust_list
    case 293: return -38; // rseq: ENOSYS
    case 261: { // prlimit64
        struct { uint64_t cur, max; } *old = (void *)a3;
        if (old) {
            if ((int)a1 == 3) { old->cur = 8 * 1024 * 1024; old->max = 8 * 1024 * 1024; } // RLIMIT_STACK
            else { old->cur = 1024; old->max = 4096; }
        }
        return 0;
    }
    case 160: { // uname
        struct { char a[65], b[65], c[65], d[65], e[65], f[65]; } *u = (void *)a0;
        memset(u, 0, sizeof(*u));
        strcpy(u->a, "Linux"); strcpy(u->b, "neodroid");
        strcpy(u->c, "6.0.0"); strcpy(u->d, "#1 NeoDroid");
        strcpy(u->e, "aarch64"); strcpy(u->f, "(none)");
        return 0;
    }
    case 17: { // getcwd
        const char *cwd = getcwd(NULL, 0);
        size_t n = strlen(cwd) + 1;
        if (n > (size_t)a1) { free((void *)cwd); return -34; }
        memcpy((void *)a0, cwd, n);
        free((void *)cwd);
        return (long)n;
    }
    case 78: return -2; // readlinkat: ENOENT
    case 134: return 0; // rt_sigaction
    case 135: return 0; // rt_sigprocmask
    case 132: return 0; // sigaltstack
    case 278: { // getrandom
        uint8_t *p = (uint8_t *)a0;
        for (size_t i = 0; i < (size_t)a1; ++i) p[i] = (uint8_t)(i * 131 + 17);
        return a1;
    }
    case 172: return 42;
    case 178: return 43;
    case 174: case 175: case 176: case 177: return 0;
    case 113: { // clock_gettime
        struct { long sec, nsec; } ts;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        ts.sec = t.tv_sec; ts.nsec = t.tv_nsec;
        memcpy((void *)a1, &ts, sizeof(ts));
        return 0;
    }
    case 169: { // gettimeofday
        struct { long sec, usec; } tv;
        struct timeval t; gettimeofday(&t, NULL);
        tv.sec = t.tv_sec; tv.usec = t.tv_usec;
        memcpy((void *)a0, &tv, sizeof(tv));
        return 0;
    }
    case 29:  return -25; // ioctl: ENOTTY
    case 48:  return -2;  // faccessat
    case 221: return -38; // execve
    case 93:
    case 94:
        g_exit_code = a0;
        longjmp(g_exit_jmp, 1);
        return 0;
    default:
        fprintf(stderr, "[neodroid] 未实现 syscall %ld\n", nr);
        return -38;
    }
}

// ---- 指令编码 ----
static uint32_t em_movz(int rd, uint16_t i) { return 0xD2800000u | ((uint32_t)i << 5) | rd; }
static uint32_t em_movk16(int rd, uint16_t i) { return 0xF2A00000u | ((uint32_t)i << 5) | rd; }
static uint32_t em_movk32(int rd, uint16_t i) { return 0xF2C00000u | ((uint32_t)i << 5) | rd; }
static uint32_t em_movk48(int rd, uint16_t i) { return 0xF2E00000u | ((uint32_t)i << 5) | rd; }
static uint32_t em_br(int rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }
static uint32_t em_b(uint32_t from, uint32_t to) {
    int32_t off = ((int32_t)(to - from)) >> 2;
    return 0x14000000u | ((uint32_t)off & 0x03FFFFFFu);
}
static void emit_mov_imm(uint32_t **p, int rd, uint64_t v) {
    *(*p)++ = em_movz(rd, (uint16_t)v);
    *(*p)++ = em_movk16(rd, (uint16_t)(v >> 16));
    *(*p)++ = em_movk32(rd, (uint16_t)(v >> 32));
    *(*p)++ = em_movk48(rd, (uint16_t)(v >> 48));
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <guest.elf>\n", argv[0]); return 2; }

    for (int i = 0; i < 1024; ++i) g_fdmap[i] = -1;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); rewind(f);
    uint8_t *img = malloc(fsize);
    if (fread(img, 1, fsize, f) != (size_t)fsize) return 1;
    fclose(f);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    printf("[neodroid] entry=0x%llx type=%u phnum=%u\n",
           (unsigned long long)eh->e_entry, eh->e_type, eh->e_phnum);
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);

    uint64_t lo = ~0ull, hi = 0, phdr_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_offset == 0) phdr_vaddr = ph[i].p_vaddr + eh->e_phoff;
    }
    uint64_t page = 4096;
    lo &= ~(page - 1); hi = (hi + page - 1) & ~(page - 1);

    // ET_EXEC 必须在链接地址；ET_DYN(static-pie) 选一个安全基址
    uint64_t base = (eh->e_type == ET_DYN) ? 0x10000000ull : lo;
    uint64_t bias = base - lo;
    uint64_t image_span = hi - lo;

    uint64_t arena_size = 1 << 20, stack_size = 1 << 20;
    uint64_t total = image_span + arena_size + stack_size;
    if (mmap((void *)base, total, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("mmap"); return 1;
    }

    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        void *dst = (void *)(uintptr_t)(ph[i].p_vaddr + bias);
        memcpy(dst, img + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            memset((void *)((uintptr_t)dst + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
    }
    printf("[neodroid] base=0x%llx bias=0x%llx span=0x%llx\n",
           (unsigned long long)base, (unsigned long long)bias, (unsigned long long)image_span);

    // ET_DYN (static-pie)：以 lo 为链接基址，实际载入到 lo（bias=0）；
    // 若需换址，调整 base 并在此前把各段拷到 base 偏移处。
    if (eh->e_type == ET_DYN) {
        // bias 已在上方计算
        uint64_t dyn_va = 0, dyn_sz = 0;
        for (int i = 0; i < eh->e_phnum; ++i)
            if (ph[i].p_type == PT_DYNAMIC) { dyn_va = ph[i].p_vaddr; dyn_sz = ph[i].p_memsz; }
        if (dyn_va) {
            Elf64_Dyn *dyn = (Elf64_Dyn *)(uintptr_t)(dyn_va + bias);
            long rela = 0, relasz = 0, relaent = sizeof(Elf64_Rela);
            long relr = 0, relrsz = 0, relrent = 8;
            for (long i = 0; i < (long)(dyn_sz / sizeof(Elf64_Dyn)); ++i) {
                uint64_t tag = dyn[i].d_tag, val = dyn[i].d_un.d_val;
                if (tag == DT_NULL) break;
                if (tag == DT_RELA) rela = (long)val;
                else if (tag == DT_RELASZ) relasz = (long)val;
                else if (tag == DT_RELAENT) relaent = (long)val;
                else if (tag == DT_RELR) relr = (long)val;
                else if (tag == DT_RELRSZ) relrsz = (long)val;
                else if (tag == DT_RELRENT) relrent = (long)val;
            }
            long n = 0;
            if (rela && relasz && relaent) {
                Elf64_Rela *r = (Elf64_Rela *)(uintptr_t)(bias + rela);
                n = relasz / relaent;
                for (long i = 0; i < n; ++i) {
                    if ((r[i].r_info & 0xffffffffu) == 1027 /* R_AARCH64_RELATIVE */)
                        *(uint64_t *)(uintptr_t)(bias + r[i].r_offset) = bias + (uint64_t)r[i].r_addend;
                }
            }
            if (relr && relrsz && relrent) {
                uint64_t *p = (uint64_t *)(uintptr_t)(bias + relr);
                uint64_t where = 0;
                long cnt = relrsz / relrent;
                for (long i = 0; i < cnt; ++i) {
                    uint64_t e = p[i];
                    if ((e & 1) == 0) { where = e; *(uint64_t *)(uintptr_t)(bias + where) += bias; where += 8; }
                    else { uint64_t bits = e >> 1; for (int k = 0; bits; ++k, bits >>= 1) if (bits & 1) *(uint64_t *)(uintptr_t)(bias + where + k * 8) += bias; where += 63 * 8; }
                }
            }
            printf("[neodroid] ET_DYN reloc: rela=%ld relr=%ld sites\n", n, (long)(relrsz / relrent));
        }
    }

    g_brk = g_brk_end = (base + image_span + 64 + arena_size + page - 1) & ~(page - 1);

    uint32_t *arena = (uint32_t *)(uintptr_t)(base + image_span + 64);
    int patched = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        uint64_t seg_lo = ph[i].p_vaddr + bias, seg_end = seg_lo + ph[i].p_filesz;
        for (uint64_t a = seg_lo; a + 4 <= seg_end; a += 4) {
            uint32_t *w = (uint32_t *)(uintptr_t)a;
            if (*w == 0xD4000001u) {
                uint32_t *t = arena;
                emit_mov_imm(&arena, 16, a + 4);
                emit_mov_imm(&arena, 17, (uint64_t)(uintptr_t)neodroid_trampoline);
                *arena++ = em_br(17);
                *w = em_b((uint32_t)a, (uint32_t)(uintptr_t)t);
                patched++;
            }
        }
    }
    printf("[neodroid] patched %d svc sites\n", patched);

    // 启动栈：argc/argv/envp/auxv
    uint64_t sp = (base + image_span + arena_size + stack_size) & ~0xFull;
    sp -= 4096; // 留出 AT_RANDOM 区
    uint64_t random_slot = sp;
    for (int i = 0; i < 16; ++i) ((uint8_t *)random_slot)[i] = (uint8_t)(i * 37 + 11);
    sp &= ~0xFull;

    uint64_t *st = (uint64_t *)(uintptr_t)sp;
    int idx = 0;
    st[idx++] = 0; // argc（测试不传参数）
    st[idx++] = 0; // argv NULL
    // envp
    st[idx++] = 0;
    // auxv
    #define AUX(t, v) do { st[idx++] = (t); st[idx++] = (uint64_t)(v); } while (0)
    AUX(AT_PHDR, phdr_vaddr + bias);
    AUX(AT_PHENT, sizeof(Elf64_Phdr));
    AUX(AT_PHNUM, eh->e_phnum);
    AUX(AT_PAGESZ, 4096);
    AUX(AT_BASE, 0);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, eh->e_entry + bias);
    AUX(AT_UID, 0); AUX(AT_EUID, 0); AUX(AT_GID, 0); AUX(AT_EGID, 0);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, random_slot);
    AUX(AT_HWCAP, 0);
    AUX(AT_CLKTCK, 100);
    AUX(AT_EXECFN, random_slot + 32);
    AUX(AT_NULL, 0);
    #undef AUX
    memcpy((void *)(uintptr_t)(random_slot + 32), argv[1], strlen(argv[1]) + 1);

    printf("----- guest 输出 -----\n"); fflush(stdout);
    if (setjmp(g_exit_jmp) == 0) {
        uint64_t entry = eh->e_entry + bias;
        __asm__ volatile("mov x29,#0\n mov x30,#0\n mov sp,%0\n br %1\n" ::"r"(sp), "r"(entry) : "memory");
        printf("[neodroid] guest returned unexpectedly\n");
    }
    printf("----- guest 退出 code=%ld -----\n", g_exit_code);
    if (g_out_len) printf("%s", g_out);
    return (int)g_exit_code;
}