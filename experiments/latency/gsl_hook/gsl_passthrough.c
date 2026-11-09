/* SPDX-License-Identifier: GPL-2.0-only */
#include "gsl_passthrough.h"

__attribute__((visibility("hidden"), noinline))
int32_t shizuka_gsl_acdb_wrapper(uint32_t opcode, const void *command,
    uint32_t command_size, void *response, uint32_t response_size,
    void *graph, uintptr_t caller_pc, shizuka_acdb_fn original)
{
    /* Deliberately unconditional passthrough. These inputs are preserved for
     * the later reviewed transformer, but no runtime enable/namespace bypass
     * or data mutation can be selected in this prototype.
     */
    __asm__ __volatile__("" : : "r"(graph), "r"(caller_pc) : "memory");
    return original(opcode, command, command_size, response, response_size);
}

__attribute__((visibility("hidden")))
uint32_t shizuka_gsl_hook_policy(void)
{
    /* version1 in the upper half; enabled=0, namespace_verified=0 below. */
    return UINT32_C(0x00010000);
}

__attribute__((visibility("hidden")))
uint32_t shizuka_gsl_hook_classify(uintptr_t return_rva)
{
    switch (return_rva) {
    case 0x246e4: return 1; /* first size query */
    case 0x247d0: return 2; /* driver fill / repeated SPF size query */
    case 0x24e20: return 3; /* SPF fill */
    default: return 0;
    }
}
