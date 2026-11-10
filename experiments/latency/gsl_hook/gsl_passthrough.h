#ifndef SHIZUKA_GSL_PASSTHROUGH_H
#define SHIZUKA_GSL_PASSTHROUGH_H
#include <stddef.h>
#include <stdint.h>

typedef int32_t (*shizuka_acdb_fn)(uint32_t, const void *, uint32_t, void *, uint32_t);

/* The first five arguments are the public acdb_ioctl ABI. The gateway adds
 * graph=x28, original caller LR, and the original GOT-resolved function=x17.
 */
int32_t shizuka_gsl_acdb_wrapper(uint32_t opcode, const void *command,
    uint32_t command_size, void *response, uint32_t response_size,
    void *graph, uintptr_t caller_pc, shizuka_acdb_fn original);

/* Pure metadata helpers; no provider setter or graph operation exists here. */
uint32_t shizuka_gsl_hook_policy(void);
uint32_t shizuka_gsl_hook_classify(uintptr_t return_rva);
#endif
