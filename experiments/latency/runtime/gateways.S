/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_DIAG_ZERO_TTP
#define SHIZUKA_DIAG_ZERO_TTP 0
#endif
#ifndef SHIZUKA_RELAY_RAW_ISLAND
#define SHIZUKA_RELAY_RAW_ISLAND 0
#endif
.global shizuka_runtime_raw_island_mode
.hidden shizuka_runtime_raw_island_mode
.equ shizuka_runtime_raw_island_mode, SHIZUKA_RELAY_RAW_ISLAND
.global shizuka_runtime_diagnostic_zero_ttp_mode
.hidden shizuka_runtime_diagnostic_zero_ttp_mode
.equ shizuka_runtime_diagnostic_zero_ttp_mode, SHIZUKA_DIAG_ZERO_TTP
.global shizuka_runtime_relay_mode
.hidden shizuka_runtime_relay_mode
.equ shizuka_runtime_relay_mode, SHIZUKA_RELAY
.section .text.shizuka_runtime_acdb_gateway,"ax",@progbits
.balign 16
.global shizuka_runtime_acdb_gateway
.hidden shizuka_runtime_acdb_gateway
.type shizuka_runtime_acdb_gateway,@function
shizuka_runtime_acdb_gateway:
    bti c
    mov x6, x30
    paciasp
    sub sp, sp, #32
    stp x29, x30, [sp, #16]
    stp x20, x29, [sp]
    mov x29, sp
    mov w9, #4
    movk w9, #0xacdb, lsl #16
    cmp w0, w9
    b.ne 1f
    mov x5, x21
    b 2f
1:  mov x5, x28
2:
    mov x7, x17
    bl shizuka_runtime_acdb
    ldp x29, x30, [sp, #16]
    add sp, sp, #32
    autiasp
    ret
.size shizuka_runtime_acdb_gateway, .-shizuka_runtime_acdb_gateway

.macro GATEWAY name, extra, target
.section .text.\name,"ax",@progbits
.balign 16
.global \name
.hidden \name
.type \name,@function
\name:
    bti c
    mov \extra, x17
    b \target
.size \name, .-\name
.endm
GATEWAY shizuka_runtime_register_gateway, x3, shizuka_runtime_register
GATEWAY shizuka_runtime_deregister_gateway, x1, shizuka_runtime_deregister
GATEWAY shizuka_runtime_send_gateway, x1, shizuka_runtime_send
GATEWAY shizuka_runtime_basic_gateway, x2, shizuka_runtime_basic
GATEWAY shizuka_runtime_deinit_gateway, x0, shizuka_runtime_deinit
GATEWAY shizuka_runtime_signal_gateway, x4, shizuka_runtime_signal
GATEWAY shizuka_runtime_message_free_gateway, x1, shizuka_runtime_message_free

/* The vendor allocator has four existing stack arguments. Preserve those at
 * their ABI offsets and append the resolved original as argument thirteen. */
.section .text.shizuka_runtime_message_alloc_gateway,"ax",@progbits
.balign 16
.global shizuka_runtime_message_alloc_gateway
.hidden shizuka_runtime_message_alloc_gateway
.type shizuka_runtime_message_alloc_gateway,@function
shizuka_runtime_message_alloc_gateway:
    bti c
    paciasp
    sub sp, sp, #64
    stp x29, x30, [sp, #48]
    add x29, sp, #48
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    stp x8, x9, [sp]
    stp x10, x11, [sp, #16]
    str x17, [sp, #32]
    bl shizuka_runtime_message_alloc
    ldp x29, x30, [sp, #48]
    add sp, sp, #64
    autiasp
    ret
.size shizuka_runtime_message_alloc_gateway, .-shizuka_runtime_message_alloc_gateway

/* Inspect metadata only. Never branch to the stripped value or change the
 * caller's saved return address; preserve this leaf's real LR exactly. */
.section .text.shizuka_runtime_strip_code_pointer,"ax",@progbits
.balign 16
.global shizuka_runtime_strip_code_pointer
.hidden shizuka_runtime_strip_code_pointer
.type shizuka_runtime_strip_code_pointer,@function
shizuka_runtime_strip_code_pointer:
    bti c
    mov x8, x30
    mov x30, x0
    hint #7 // XPACLRI
    mov x0, x30
    mov x30, x8
    ret
.size shizuka_runtime_strip_code_pointer, .-shizuka_runtime_strip_code_pointer

/* Keep Android's original page-aligned __cfi_check address. Extend only two
 * exact callback/type pairs; all other checks return to the original body. */
.section .text.shizuka_runtime_cfi_gateway,"ax",@progbits
.balign 16
.global shizuka_runtime_cfi_gateway
.hidden shizuka_runtime_cfi_gateway
.type shizuka_runtime_cfi_gateway,@function
shizuka_runtime_cfi_gateway:
    mov x8, #0xaa4f
    movk x8, #0x1f24, lsl #16
    movk x8, #0x91eb, lsl #32
    movk x8, #0xa999, lsl #48
    cmp x0, x8
    b.ne 1f
    adrp x9, shizuka_runtime_callback
    add x9, x9, :lo12:shizuka_runtime_callback
    cmp x1, x9
    b.eq 3f
1:  mov x8, #0xf4c1
    movk x8, #0xdf1c, lsl #16
    movk x8, #0x0309, lsl #32
    movk x8, #0x4c55, lsl #48
    cmp x0, x8
    b.ne 2f
    adrp x9, shizuka_runtime_worker
    add x9, x9, :lo12:shizuka_runtime_worker
    cmp x1, x9
    b.eq 3f
2:  mov x8, #0xf1c1
    b shizuka_original_cfi_continue
3:  ret
.size shizuka_runtime_cfi_gateway, .-shizuka_runtime_cfi_gateway
.section .note.GNU-stack,"",@progbits
