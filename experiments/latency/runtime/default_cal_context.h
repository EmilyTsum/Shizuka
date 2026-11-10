/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_DEFAULT_CAL_CONTEXT_H
#define SHIZUKA_DEFAULT_CAL_CONTEXT_H
#include "graph_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
struct shizuka_default_cal_view {
    const struct shizuka_gsl_key *keys;
    uint32_t num_keys;
};

/* A narrowly scoped view of the still-live stock 4d7219..c6bc OPEN stack.
 * Caller holds its GSL/state ownership lock, has checked that the original ACDB
 * call succeeded, and supplies its exact active CAL frame, node and owned ACDB
 * arrays. They and the saved original native GKV remain readable for this call.
 * This is not an arbitrary-memory probe: integer/alignment checks cannot prove
 * a pointer is mapped. Call only at the two recognized stock CAL return PCs.
 *
 * The helper verifies the frame relationships and exact saved return sites
 * BEFORE reading the saved node/vector; it never follows a DSP/physical address.
 * strip_ip strips metadata only, without authenticating/changing the real LR.
 * Accepted view points to the ACTUAL saved original GKV, never cached keys; use
 * it immediately before the original OPEN stack unwinds, do not retain it.
 * Return1 on full ownership/key/SG equality, else0 with output unchanged.
 * No graph/packet/bridge/environment/input mutation, allocation, lock or send.
 */
int shizuka_default_cal_context_view(const struct shizuka_graph_bridge *,
    const struct shizuka_graph_bridge_env *,uint32_t cal_pc,
    const void *command,uint32_t command_size,const void *response,uint32_t response_size,
    uintptr_t node,uintptr_t cal_fp,uintptr_t (*strip_ip)(uintptr_t),
    struct shizuka_default_cal_view *output);
#ifdef __cplusplus
}
#endif
#endif
