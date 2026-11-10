/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_RELAY_TRANSACTION_H
#define SHIZUKA_RELAY_TRANSACTION_H
#include "relay_graph.h"

#define SHIZUKA_GSL_EXACT_LIBRARY 1U
#define SHIZUKA_GSL_NAMESPACE_OWNED 2U
#define SHIZUKA_GSL_TRANSPORT_READY 4U
#define SHIZUKA_GSL_SOURCE_REGISTERED 8U
#define SHIZUKA_GSL_REQUIRED_FLAGS 15U
#define SHIZUKA_GSL_MAX_SGIDS 128U
#define SHIZUKA_GSL_MAX_KEYS 64U

struct shizuka_gsl_key { uint32_t key,value; };
struct shizuka_relay_scope {
    uintptr_t graph,thread;
    uint64_t epoch;
    uint32_t source_port,client_port,flags;
    uint32_t open_refs,start_refs,stop_refs;
};
/* NAMESPACE_OWNED now means exact replacement of the two existing logger nodes
 * in a positively fresh SG00ab owned by this GSL OPEN. It does not mean an
 * unknown-port timeout or a saved ACDB absence check reserved arbitrary IDs.
 */
struct shizuka_relay_provider {
    uint64_t revision;
    uint32_t enabled,hq_extra_ttp_us;
};
struct shizuka_gsl_after_call {
    uint32_t caller_rva,opcode,command_size,response_size;
    int32_t original_status;
    uint32_t original_spf_size;
    const uint32_t *sgids;
    uint32_t num_sgids;
    const struct shizuka_gsl_key *keys;
    uint32_t num_keys;
    uint8_t *spf;
    size_t spf_capacity;
};
enum shizuka_relay_phase { SHIZUKA_RELAY_IDLE=0,SHIZUKA_RELAY_SIZE=1,
    SHIZUKA_RELAY_DRIVER=2,SHIZUKA_RELAY_FILLED=3,SHIZUKA_RELAY_ACTIVE=4,
    SHIZUKA_RELAY_FAILED=5,SHIZUKA_RELAY_CLOSED=6 };
enum shizuka_relay_after_result { SHIZUKA_RELAY_ABORT=-1,
    SHIZUKA_RELAY_BYPASS=0,SHIZUKA_RELAY_ADVERTISE=1,SHIZUKA_RELAY_COMMITTED=2 };
struct shizuka_relay_transaction {
    struct shizuka_relay_scope scope;
    struct shizuka_relay_provider provider;
    uint32_t phase,original_size,num_sgids,num_keys,profile_confirmed;
    uint32_t sgids[SHIZUKA_GSL_MAX_SGIDS];
    struct shizuka_gsl_key keys[SHIZUKA_GSL_MAX_KEYS];
};
struct shizuka_relay_after_result_data {
    uint32_t advertised_size;
    uint64_t frozen_revision;
};

/* The eight-argument PLT wrapper calls original exactly once first. After a
 * recognized successful ACDB call it passes a checked view to this function.
 * This function never invokes original, inspects arbitrary graph pointers,
 * allocates, locks, registers callbacks, or sends GPR. The embedding owner must
 * serialize this transaction with the original GSL open/close lock and provide
 * validated arrays, scope/ownership facts and an external workspace.
 * Before size growth a failed gate returns BYPASS with no changed byte.
 * After growth any changed ownership/stage/size/topology returns ABORT; the
 * wrapper must fail this fresh OPEN, never return a shorter successful blob.
 */
int shizuka_relay_after_acdb(struct shizuka_relay_transaction *transaction,
    const struct shizuka_relay_scope *scope,const struct shizuka_relay_provider *provider,
    const struct shizuka_gsl_after_call *call,uint8_t *workspace,size_t workspace_capacity,
    struct shizuka_relay_after_result_data *result);

/* Owner calls these only for an exact graph/session epoch and its matched APM
 * lifecycle completion, not from a setting save or command-send success.
 */
int shizuka_relay_open_complete(struct shizuka_relay_transaction *,
                               const struct shizuka_relay_scope *,int accepted);
void shizuka_relay_close(struct shizuka_relay_transaction *,const struct shizuka_relay_scope *);
void shizuka_relay_reset(struct shizuka_relay_transaction *);

/* A positive delta additionally needs actual accepted encoder configuration
 * and negotiated non-Lossless HQ/R3/44.1kHz in the same epoch. INIT's HQ bit
 * alone cannot identify Lossless. No LL/Lossless/legacy support is invented.
 */
int shizuka_relay_confirm_profile(struct shizuka_relay_transaction *,
    const struct shizuka_relay_scope *,uint32_t accepted_init,uint32_t codec_mid,
    uint32_t negotiated_mode,uint32_t sample_rate_hz,uint32_t wrapper_version,
    uint32_t lossless);
uint32_t shizuka_relay_active_delta(const struct shizuka_relay_transaction *);
#endif
