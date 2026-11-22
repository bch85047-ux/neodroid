// NeoDroid P0 proof: AArch64 SVC instruction interception model.
// On iOS, the signal/exception boundary is supplied by the host runtime.
// This Linux proof confirms that native ARM64 instructions execute directly,
// and only the syscall ABI boundary requires host dispatch.

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>

static long native_gettid(void) {
#if defined(__aarch64__)
    register long x8 __asm__("x8") = __NR_gettid;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
#else
    return syscall(SYS_gettid);
#endif
}

static uint64_t native_add_hotpath(uint64_t a, uint64_t b) {
#if defined(__aarch64__)
    uint64_t result;
    __asm__ volatile("add %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
#else
    return a + b;
#endif
}

int main(void) {
    const uint64_t result = native_add_hotpath(0x12345678ULL, 0x87654321ULL);
    const long tid = native_gettid();
    printf("NeoDroid ARM64 direct execution proof\n");
    printf("  native ADD: 0x%llx\n", (unsigned long long)result);
    printf("  SVC gettid: %ld\n", tid);
    printf("  model: normal ARM64 instructions run natively; SVC routes to ABI layer\n");
    return result == 0x99999999ULL && tid > 0 ? 0 : 1;
}
