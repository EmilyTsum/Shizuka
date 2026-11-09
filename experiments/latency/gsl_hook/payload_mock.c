/* SPDX-License-Identifier: GPL-2.0-only
 * Pure mock program: no dlopen, GPR, ACDB, GSL, device, or audio operations.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "gsl_passthrough.h"

static struct {
    uint32_t opcode, command_size, response_size;
    const void *command;
    void *response;
    int32_t result;
    unsigned calls, errors, writes;
} expected;

static int32_t mock_original(uint32_t opcode, const void *command,
    uint32_t command_size, void *response, uint32_t response_size)
{
    ++expected.calls;
    if (opcode!=expected.opcode || command!=expected.command || command_size!=expected.command_size
        || response!=expected.response || response_size!=expected.response_size) ++expected.errors;
    if (expected.writes && response_size && response) ((unsigned char *)response)[0]^=0x5a;
    return expected.result;
}

#if defined(__aarch64__) && defined(__ANDROID__)
int32_t shizuka_test_gateway(uint32_t, const void *, uint32_t, void *, uint32_t,
                           void *, shizuka_acdb_fn);
#endif

static uint32_t next_random(uint32_t *state)
{
    uint32_t value=*state;
    value^=value<<13;value^=value>>17;value^=value<<5;
    return *state=value;
}

int main(void)
{
    uint32_t random=UINT32_C(0x535a2026);
    unsigned char command[96], response[96], original_command[96], wanted_response[96];
    unsigned cases=0, failures=0;
    const uintptr_t caller[] = {0x246e4,0x247d0,0x24e20,0x38000};
    for (unsigned i=0;i<512;i++) {
        for (unsigned j=0;j<sizeof(command);j++) {
            command[j]=(unsigned char)next_random(&random);
            response[j]=(unsigned char)next_random(&random);
        }
        memcpy(original_command,command,sizeof(command));
        memcpy(wanted_response,response,sizeof(response));
        expected.opcode=next_random(&random);
        expected.command_size=i%5 ? next_random(&random)%97 : UINT32_MAX;
        expected.response_size=i%7 ? next_random(&random)%97 : UINT32_MAX;
        expected.command=command;expected.response=response;
        expected.result=(int32_t)next_random(&random);
        expected.calls=0;expected.errors=0;expected.writes=i&1;
        if (expected.writes && expected.response_size) wanted_response[0]^=0x5a;
        int32_t actual=shizuka_gsl_acdb_wrapper(expected.opcode,command,expected.command_size,
            response,expected.response_size,(void *)(uintptr_t)0x12345000,caller[i%4],mock_original);
        if (actual!=expected.result || expected.calls!=1 || expected.errors
            || memcmp(command,original_command,sizeof(command))
            || memcmp(response,wanted_response,sizeof(response))) ++failures;
        ++cases;
#if defined(__aarch64__) && defined(__ANDROID__)
        /* Real ARM64 gateway entry, with x28/x17 supplied by a mock PLT caller. */
        memcpy(response,wanted_response,sizeof(response));
        if (expected.writes && expected.response_size) wanted_response[0]^=0x5a;
        expected.calls=0;
        actual=shizuka_test_gateway(expected.opcode,command,expected.command_size,response,
            expected.response_size,(void *)(uintptr_t)0x12345000,mock_original);
        if (actual!=expected.result || expected.calls!=1 || expected.errors
            || memcmp(command,original_command,sizeof(command))
            || memcmp(response,wanted_response,sizeof(response))) ++failures;
        ++cases;
#endif
    }
    if (shizuka_gsl_hook_policy()!=0x10000) ++failures;
    for(unsigned i=0;i<4;i++) if(shizuka_gsl_hook_classify(caller[i])!=(i<3?i+1:0)) ++failures;
    printf("{\"operation\":\"pure_mock_gsl_passthrough\",\"cases\":%u,\"failures\":%u,"
           "\"provider_enabled\":false,\"namespace_verified\":false,\"gpr_opened\":false,"
           "\"gsl_loaded\":false,\"graph_commands_sent\":false}\n",cases,failures);
    return failures?1:0;
}
