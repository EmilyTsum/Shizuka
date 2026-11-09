/* SPDX-License-Identifier: GPL-2.0-only
 * Test-only PLT simulation. This file is never appended to libar-gsl.
 */
.text
.balign 16
.global shizuka_test_gateway
.hidden shizuka_test_gateway
.type shizuka_test_gateway,@function
shizuka_test_gateway:
    bti c
    paciasp
    stp x28, x30, [sp, #-16]!
    mov x28, x5
    mov x17, x6
    bl shizuka_gsl_gateway
    ldp x28, x30, [sp], #16
    autiasp
    ret
.size shizuka_test_gateway, .-shizuka_test_gateway
.section .note.gnu.property,"a",@note
.p2align 3
.long 4, 16, 5
.asciz "GNU"
.long 0xc0000000, 4, 3, 0
.section .note.GNU-stack,"",@progbits
