// NeoDroid P1 syscall trampoline (AArch64)
// x8 = Linux syscall number, x0..x5 = args, x16 = 返回地址（由每站点 thunk 填入）
// 进入后保存 guest 所有可能被 C 调用破坏的寄存器，分发，再恢复并跳回。

    .section .text
    .global neodroid_trampoline
    .type   neodroid_trampoline, %function

neodroid_trampoline:
    sub     sp, sp, #160
    stp     x1,  x2,  [sp, #0]
    stp     x3,  x4,  [sp, #16]
    stp     x5,  x6,  [sp, #32]
    stp     x7,  x8,  [sp, #48]
    stp     x9,  x10, [sp, #64]
    stp     x11, x12, [sp, #80]
    stp     x13, x14, [sp, #96]
    stp     x15, x17, [sp, #112]
    stp     x18, x30, [sp, #128]
    str     x16,      [sp, #144]

    // 重排成 C 调用约定: neodroid_host_syscall(nr, a0, a1, a2, a3, a4, a5)
    mov     x6, x5
    mov     x5, x4
    mov     x4, x3
    mov     x3, x2
    mov     x2, x1
    mov     x1, x0
    mov     x0, x8
    bl      neodroid_host_syscall

    // x0 保持为返回值（guest 的 syscall 返回寄存器）
    ldp     x1,  x2,  [sp, #0]
    ldp     x3,  x4,  [sp, #16]
    ldp     x5,  x6,  [sp, #32]
    ldp     x7,  x8,  [sp, #48]
    ldp     x9,  x10, [sp, #64]
    ldp     x11, x12, [sp, #80]
    ldp     x13, x14, [sp, #96]
    ldp     x15, x17, [sp, #112]
    ldp     x18, x30, [sp, #128]
    ldr     x16,      [sp, #144]
    add     sp, sp, #160
    br      x16

    .size neodroid_trampoline, . - neodroid_trampoline