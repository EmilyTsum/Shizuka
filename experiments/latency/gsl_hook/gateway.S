/* SPDX-License-Identifier: GPL-2.0-only */
.section .text.shizuka_gateway,"ax",@progbits
.balign 16
.global shizuka_gsl_gateway
.hidden shizuka_gsl_gateway
.type shizuka_gsl_gateway,@function
shizuka_gsl_gateway:
    bti c
    mov x5, x28
    mov x6, x30
    mov x7, x17
    b shizuka_gsl_acdb_wrapper
.size shizuka_gsl_gateway, .-shizuka_gsl_gateway
.section .note.gnu.property,"a",@note
.p2align 3
.long 4, 16, 5
.asciz "GNU"
.long 0xc0000000, 4, 3, 0
.section .note.GNU-stack,"",@progbits
