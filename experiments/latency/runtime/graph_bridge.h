/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_RUNTIME_GRAPH_BRIDGE_H
#define SHIZUKA_RUNTIME_GRAPH_BRIDGE_H
#include "../gsl_hook/relay_calibration.h"
#include "../../../src/latency_context.h"

#ifdef __cplusplus
extern "C" {
#endif
#define SHIZUKA_GRAPH_ACDB_DATA UINT32_C(0xacdb0002)
#define SHIZUKA_GRAPH_ACDB_CAL UINT32_C(0xacdb0004)
#define SHIZUKA_GRAPH_OPEN_OPCODE UINT32_C(0x01001000)
#define SHIZUKA_GRAPH_AR_EFAILED INT32_C(1)
#define SHIZUKA_GRAPH_AR_ENOTEXIST INT32_C(19)

struct shizuka_graph_bridge {
    struct shizuka_relay_transaction transaction;
    struct shizuka_relay_cal_transaction calibration;
    S25LatencyContext frozen_context;
    uintptr_t library_base;
    uint32_t frozen_context_seq;
    uint32_t open_accepted;
};
struct shizuka_graph_bridge_env {
    struct shizuka_relay_scope scope;
    struct shizuka_relay_provider provider;
    S25LatencyContext context; /* Owner supplies a coherent same-process snapshot. */
    uintptr_t library_base; /* Checked exact GSL mapping, not an arbitrary LR base. */
    uint32_t transport_ready;
    /* Actual provided buffer capacities captured BEFORE original ACDB overwrites
     * its response lengths. A post-call actual-size field is not a capacity.
     */
    size_t spf_capacity,cal_capacity;
    /* Calibration command carries CKVs, not GKV. Owner verifies graph/sourceport
     * and supplies that epoch's actual current GKV, even after provider expiry.
     */
    const struct shizuka_gsl_key *current_gkv;
    uint32_t num_gkv;
};
struct shizuka_graph_bridge_result {
    int32_t action; /* relay BYPASS/ADVERTISE/COMMITTED/ABORT or CAL_EMPTY */
    uint32_t actual_size,removed;
};

/* Caller holds its state lock and calls original ACDB exactly once BEFORE this.
 * Only recognized packed ABIs/call sites are decoded. Original success proves
 * array storage came from the exact GSL/ACDB owner; this is not a general memory
 * probing API. Arrays are bounded/aligned, then copied using byte-safe loads.
 * graph_from_gateway is x28 for graph ACDB, x21 for calibration ACDB.
 * Return is the ACDB AR status; SPF/Blob size fields and bodies are updated here.
 * Unknown/unowned calls and original nonzero statuses are preserved unchanged.
 */
int32_t shizuka_graph_bridge_after_acdb(struct shizuka_graph_bridge *,
    const struct shizuka_graph_bridge_env *,uint32_t opcode,
    const void *command,uint32_t command_size,void *response,uint32_t response_size,
    int32_t original_status,uintptr_t graph_from_gateway,uint32_t caller_rva,
    uint8_t *workspace,size_t workspace_capacity,struct shizuka_graph_bridge_result *);

/* Call after the original synchronous basic-response wait returns, BEFORE its
 * wrapper returns to graph_open and that same thread can request calibration.
 * Original wait has validated the ACK/opcode/APM status; status is its AR rc.
 * Root caches command/source identity before that original can free its packet.
 */
int shizuka_graph_bridge_open_ack(struct shizuka_graph_bridge *,
    const struct shizuka_graph_bridge_env *,uint32_t command_opcode,
    uint32_t owner_source_port,int32_t original_wait_status);

/* strict_r3_success means the first real buffer passed the complete reviewed R3
 * parser BEFORE any positive rewrite/send. It is not inferred from INIT or a
 * fabricated framing probe. INIT-only calls must pass zero here.
 */
int shizuka_graph_bridge_confirm_profile(struct shizuka_graph_bridge *,
    const struct shizuka_graph_bridge_env *,uint32_t accepted_init,uint32_t codec_mid,
    uint32_t negotiated_mode,uint32_t sample_rate_hz,uint32_t strict_r3_success);
uint32_t shizuka_graph_bridge_active_delta(const struct shizuka_graph_bridge *,
    const struct shizuka_graph_bridge_env *);
/* Close/reset only this exact graph/epoch/source/client ownership. */
void shizuka_graph_bridge_close(struct shizuka_graph_bridge *,
    const struct shizuka_relay_scope *);
void shizuka_graph_bridge_reset(struct shizuka_graph_bridge *);
#ifdef __cplusplus
}
#endif
#endif
