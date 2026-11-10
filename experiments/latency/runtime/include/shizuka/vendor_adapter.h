/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_VENDOR_ADAPTER_H
#define SHIZUKA_VENDOR_ADAPTER_H
#include <stddef.h>
#include <stdint.h>
#include "shizuka/hal_transport.h"
#include "shizuka/encoded_transform.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SHIZUKA_VENDOR_ADAPTER_ABI 1u
#define SHIZUKA_VENDOR_DZIF_LAYOUT_V1 0x445a0001u

/* Exact reviewed ARM64 vendor structures, not serialized host pointers. */
struct shizuka_vendor_gpr_packet {
    uint32_t header;
    uint8_t dst_domain, src_domain, client_data, reserved;
    uint32_t src_port, dst_port, token, opcode;
};
struct shizuka_vendor_gpr_alloc {
    uint8_t src_domain;
    uint32_t src_port;
    uint8_t dst_domain;
    uint32_t dst_port;
    uint8_t client_data;
    uint32_t token, opcode, payload_size;
    struct shizuka_vendor_gpr_packet **ret_packet;
};
struct shizuka_vendor_shmem {
    uint32_t cache_type;
    size_t buf_size;
    uint32_t mem_type, index_type, ipa_lsw, ipa_msw, pa_lsw, pa_msw;
    void *vaddr;
    uint64_t metadata;
    uint8_t num_sys_id;
    uint8_t *sys_id;
    uint32_t platform_info, flags;
};
typedef uint32_t (*shizuka_vendor_gpr_callback)(struct shizuka_vendor_gpr_packet *, void *);

enum shizuka_vendor_receive_error {
    SHIZUKA_VENDOR_INVALID_PACKET = 1,
    SHIZUKA_VENDOR_RECEIVER_REJECTED,
    SHIZUKA_VENDOR_PACKET_FREE_FAILED,
    SHIZUKA_VENDOR_CONCURRENT_CALLBACK
};

struct shizuka_vendor_functions {
    uint32_t abi, bytes, abi_profile, identity_verified;
    void *user;
    uint32_t (*gpr_register)(uint32_t, shizuka_vendor_gpr_callback, void *);
    uint32_t (*gpr_deregister)(uint32_t);
    uint32_t (*gpr_alloc)(struct shizuka_vendor_gpr_alloc *);
    uint32_t (*gpr_send)(struct shizuka_vendor_gpr_packet *);
    uint32_t (*gpr_free)(struct shizuka_vendor_gpr_packet *);
    int32_t (*shmem_alloc)(struct shizuka_vendor_shmem *);
    int32_t (*shmem_free)(struct shizuka_vendor_shmem *);
    /* Must be root's exact CFI-approved shizuka_runtime_callback address.
     * Adapter registration never substitutes its own callback entry point. */
    shizuka_vendor_gpr_callback callback_entry;
    long (*raw_ioctl)(void *, int32_t fd, uint32_t request, void *argument);
    uint64_t (*clock_ms)(void *);
    void (*notify)(void *, uint32_t phase, int32_t error, uint32_t need_graph_close);
    /* Callback/worker notification: atomically flag a worker-owned stop. It
     * must not send, block, free transport state or call worker APIs. */
    void (*receive_error)(void *, uint32_t reason);
    /* Worker-thread telemetry; result is valid only for this synchronous call.
     * This records a transform, not receiver latency or successful WR_DONE. */
    void (*transform_result)(void *, int32_t status, uint32_t profile,
        uint64_t delta_us, const struct shz_encoded_result *result);
};

struct shizuka_vendor_status {
    uint32_t abi, bytes, accepting, registered, inflight, retired;
    uint32_t packets_received, packets_delivered, packets_rejected;
    uint32_t packets_free_attempted, packets_free_failed;
    uint32_t sends_accepted, sends_failed, allocations_owned, allocations_quarantined;
};

/* One resident adapter per HAL lifetime. Storage starts zero-filled, is
 * 8-byte aligned and never moves or unloads. init cannot overwrite an existing
 * adapter. After deregistration it cannot register again: late callbacks keep
 * this old context and are freed without delivery. Ordinary graph epochs keep
 * the same port attached. A new HAL generation gets fresh resident storage. */
size_t shizuka_vendor_adapter_bytes(void);
int32_t shizuka_vendor_adapter_init(void *, size_t, const struct shizuka_vendor_functions *);
int32_t shizuka_vendor_make_host(void *, struct shizuka_transport_host *);
int32_t shizuka_vendor_get_status(void *, struct shizuka_vendor_status *);
/* Root's CFI-approved GPR callback calls this and returns GPR AR_EOK. This
 * function always consumes/frees a nonnull packet once for a valid adapter,
 * including malformed packets and callbacks arriving after detach. */
uint32_t shizuka_vendor_receive(void *adapter, struct shizuka_vendor_gpr_packet *);

#ifdef __cplusplus
}
#endif
#endif
