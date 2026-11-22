// NeoDroid P1 guest test program (ARM64 Linux ABI, freestanding)
// 只用 write + exit 两个 syscall，验证 svc 重写 + 分发机制。

    .section .text
    .global _start
_start:
    mov     x8, #64          // __NR_write
    mov     x0, #1           // stdout
    adr     x1, msg
    mov     x2, #34          // length
    svc     #0

    mov     x8, #64          // __NR_write
    mov     x0, #1
    adr     x1, msg2
    mov     x2, #24
    svc     #0

    mov     x8, #93          // __NR_exit
    mov     x0, #7           // exit code 7
    svc     #0

msg:
    .ascii  "hello from NeoDroid guest ELF!\n"
msg2:
    .ascii  "syscall dispatch worked!\n"

    .section .data
    .quad   0x1122334455667788