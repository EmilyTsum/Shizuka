/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_HAL_TRANSPORT_H
#define SHIZUKA_HAL_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifndef SHIZUKA_RELAY_RAW_ISLAND
#define SHIZUKA_RELAY_RAW_ISLAND 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SHIZUKA_TRANSPORT_ABI 1u
#define SHIZUKA_TRANSPORT_CLIENT_PORT 0x535a6101u
#define SHIZUKA_TRANSPORT_RD_MIID 0x000043f7u
#define SHIZUKA_TRANSPORT_WR_MIID 0x000043fau
#define SHIZUKA_TRANSPORT_ALLOCATION_BYTES 16384u
#define SHIZUKA_TRANSPORT_DATA_CAPACITY 4096u
#define SHIZUKA_TRANSPORT_METADATA_OFFSET 4096u
#define SHIZUKA_TRANSPORT_METADATA_CAPACITY 4096u
#define SHIZUKA_TRANSPORT_MAX_PACKET_PAYLOAD 1024u

enum shizuka_transport_result {
    SHIZUKA_TRANSPORT_OK = 0,
    SHIZUKA_TRANSPORT_BAD_ARGUMENT = -1,
    SHIZUKA_TRANSPORT_BAD_STATE = -2,
    SHIZUKA_TRANSPORT_HOST_ERROR = -3,
    SHIZUKA_TRANSPORT_REJECTED = -4,
    SHIZUKA_TRANSPORT_CLEANUP_UNKNOWN = -5
};

enum shizuka_transport_phase {
    SHIZUKA_TRANSPORT_DETACHED = 0,
    SHIZUKA_TRANSPORT_REGISTERED,
    SHIZUKA_TRANSPORT_READY,
    SHIZUKA_TRANSPORT_CONFIGURING,
    SHIZUKA_TRANSPORT_CONFIGURED,
    SHIZUKA_TRANSPORT_ACTIVE,
    SHIZUKA_TRANSPORT_DRAINING,
    SHIZUKA_TRANSPORT_UNCERTAIN,
    SHIZUKA_TRANSPORT_CLOSED,
    SHIZUKA_TRANSPORT_PAUSING,
    SHIZUKA_TRANSPORT_PAUSED
};

enum shizuka_transport_graph_stage {
    SHIZUKA_GRAPH_PREPARED = 1,
    SHIZUKA_GRAPH_STARTED,
    SHIZUKA_GRAPH_STOPPING,
    SHIZUKA_GRAPH_CLOSED,
    SHIZUKA_GRAPH_DSP_GENERATION_RESET,
    SHIZUKA_GRAPH_PAUSING, /* Before ordinary STOP; retain graph/mappings. */
    SHIZUKA_GRAPH_STOPPED  /* Successful ordinary STOP acknowledgement. */
};

/* A normalized packet, not a vendor gpr_packet_t. Receive callback wrapper
 * validates the actual GPR header/size, calls receive(), then frees its own GPR
 * packet exactly once. send() must copy payload before returning. */
struct shizuka_transport_packet {
    uint32_t src_port;
    uint32_t dst_port;
    uint32_t token;
    uint32_t opcode;
    uint32_t payload_bytes;
    uint8_t src_domain;
    uint8_t dst_domain;
    uint8_t client_data;
    uint8_t reserved;
    const uint8_t *payload;
};

/* Root bootstrap's allocator adapter fills this stable, context-resident POD.
 * cookie holds the reviewed 80-byte OSAL object without exposing that ABI here.
 * The adapter must allocate exactly 16 KiB with default flags / ADSP sys-id=2,
 * return offset mode and CPU START ownership, and keep all cookie pointers
 * valid until free(). No init/deinit is requested by this component. */
struct shizuka_transport_memory {
    uint64_t allocator_cookie[10];
    uint8_t *cpu_address;
    uint32_t capacity;
    int32_t fd;
    uint32_t offset_mode;
    uint8_t subsystem_id;
    uint8_t reserved[3];
};

struct shizuka_transport_transform_result {
    uint32_t data_bytes;
    uint32_t metadata_bytes;
    uint64_t timestamp_bits;
    uint32_t wr_flags;
    uint32_t wrappers_changed;
};

/* Translate an already validated parser result to OLC WR wire flags. A
 * validated RD buffer/frame-info record does not assert codec end-of-frame
 * or timestamp continuation. Do not use this helper instead of validation. */
static inline uint32_t shizuka_transport_wr_wire_flags(uint32_t validated_parser_flags)
{
    return validated_parser_flags & UINT32_C(0x80000000);
}

typedef int32_t (*shizuka_transport_receive_fn)(void *,
    const struct shizuka_transport_packet *);

struct shizuka_transport_host {
    uint32_t abi;
    uint32_t bytes;
    void *user;
    /* Existing initialized HAL registry only. Must fail if port is already
     * owned. unregister_port must also synchronize in-progress callbacks. */
    int32_t (*register_port)(void *, uint32_t, shizuka_transport_receive_fn, void *);
    int32_t (*unregister_port)(void *, uint32_t);
    /* 0 = accepted; 1 = definitely not sent; other = outcome unknown. No retry.
     * Adapter owns/frees a failed GPR packet. Never use gpr_init/deinit here. */
    int32_t (*send)(void *, const struct shizuka_transport_packet *);
    int32_t (*allocate)(void *, struct shizuka_transport_memory *);
    int32_t (*free)(void *, struct shizuka_transport_memory *);
    /* begin=1 CPU START|RW; begin=0 CPU END|RW, whole dma-buf. */
    int32_t (*cpu_sync)(void *, struct shizuka_transport_memory *, uint32_t begin);
    /* Must validate the entire frame before mutation. Returns nonzero without
     * publishing partial output. Never changes graph-epoch delta/profile. */
    int32_t (*transform)(void *, uint32_t profile, uint64_t delta_us,
        uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
        uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity,
        struct shizuka_transport_transform_result *);
    uint64_t (*clock_ms)(void *);
    /* Called synchronously by the serialized transport executor, including a
     * GSL control caller for bind/stage/failed attestation. Never the GPR receive
     * callback. Must not reenter the executor or wait for GSL while its owner
     * lock is held. need_graph_close requests the owner's ordinary owned-graph
     * STOP/CLOSE lifecycle; it does not perform that lifecycle or close-all. */
    void (*notify)(void *, uint32_t phase, int32_t error, uint32_t need_graph_close);
    /* Optional reviewed validator for nonempty RD buffers returned by STOP.
     * It may compact private MD, but must not add TTP, publish telemetry or
     * forward data. Unknown/tracked metadata must fail and require CLOSE. */
    int32_t (*validate_discard)(void *, uint32_t profile,
        uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
        uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity);
};

struct shizuka_transport_options {
    uint32_t abi;
    uint32_t bytes;
    uint32_t token_seed;
    uint32_t control_timeout_ms; /* 100..30000; each command is sent once. */
    uint32_t buffer_timeout_ms;  /* 100..60000; expiry requires owned close. */
    uint32_t reserved;
    uint64_t hal_generation;
    uint64_t dsp_generation;
};

/* bind_graph is allowed only after the ordinary GSL OPEN acknowledgement and
 * positive owner checks. No caller-selected profile is an attestation. */
struct shizuka_transport_graph {
    uint32_t abi;
    uint32_t bytes;
    uint64_t graph_epoch;
    uint64_t hal_generation;
    uint64_t dsp_generation;
    uint64_t delta_us; /* Fixed for this epoch; UI changes wait for next epoch. */
    uint32_t profile;  /* 1 = reviewed stock R3 / HQ / 44100; others rejected. */
    uint32_t subgraph_id;
    uint32_t container_id;
    uint32_t parent_client_id;
    uint32_t rd_miid;
    uint32_t wr_miid;
    uint32_t fresh_owned_graph;
    uint32_t endpoint_namespace_owned;
    uint32_t original_imcl_preserved;
    uint32_t olc_parent_protocol_reviewed;
    uint32_t actual_init_profile_attested;
    uint32_t open_acknowledged;
};

struct shizuka_transport_status {
    uint32_t abi;
    uint32_t bytes;
    uint32_t phase;
    int32_t error;
    uint64_t graph_epoch;
    uint64_t delta_us;
    uint64_t rd_frames;
    uint64_t wr_frames;
    uint64_t rewritten_wrappers; /* Counted only after successful WR_DONE. */
    uint32_t registration_ready; /* Existing callback registered + SPF_READY. */
    uint32_t configuration_ready; /* Own mappings + event registration acked. */
    uint32_t media_format_ready;
    uint32_t need_graph_close;
    uint32_t unresolved_mappings;
    uint32_t unresolved_buffers;
    uint32_t unresolved_allocations;
    uint32_t pending_control;
    uint32_t received_overflow;
    uint32_t pause_ready;
    uint32_t media_format_bytes; /* Accepted current-epoch format; WR ACK is separate. */
    /* Internal diagnostic observations only; never alternate authorization.
     * startup bits0..11: configured,INIT,PREPARED,ownerSTART,peerACK3,OPFS,
     * WR-formatACK,observedRD-STARTED,pause,needCLOSE,error,graphCLOSED. */
    uint32_t startup_flags,rd_frame_bytes,initial_peer_acks,last_rd_state,rd_state_events;
    /* Exact first property of the last validated peer payload queued to WR,
     * not a default or an independent ACK/authorization claim. bytes0 means
     * unknown; count/type/value are then0. The observation may remain after
     * revocation/CLOSE and is cleared when a new graph epoch is bound. */
    uint32_t wr_peer_bytes,wr_peer_count,wr_peer_type,wr_peer_value;
    /* Last fully validated WR03001005 packet containing type3. Diagnostic
     * only, including a state that revokes the epoch. bytes/events0 means
     * unobserved and last_wr_state is UINT32_MAX. Duplicate valid receipts
     * increment events; no default, ACK or downstream-ready inference. */
    uint32_t wr_state_bytes,wr_state_events,last_wr_state;
};

/* Context is caller-owned, initially zero-filled, 8-byte aligned and immovable
 * until detach succeeds. Reinitializing an attached context is rejected.
 * No heap, STL, exception, dlopen or device-open dependency. */
size_t shizuka_transport_context_bytes(void);
int32_t shizuka_transport_init(void *, size_t,
    const struct shizuka_transport_host *, const struct shizuka_transport_options *);
int32_t shizuka_transport_attach(void *);
int32_t shizuka_transport_bind_graph(void *, const struct shizuka_transport_graph *);
/* OPEN may precede codec INIT. Mapping/event registration can start without
 * this evidence; data cannot. Call only after the actual INIT was accepted. */
int32_t shizuka_transport_attest_profile(void *, uint64_t epoch, uint32_t profile,
    uint32_t accepted_init_profile_attested);
int32_t shizuka_transport_graph_stage(void *, uint64_t epoch, uint32_t stage,
    uint64_t independently_confirmed_dsp_generation);
/* Default-disabled RAW-island experiment only. After the real owner's accepted
 * INIT and PREPARE, opt in to queue two bounded READ leases before owner START.
 * Worker sends only the positively observed PREPARED downstream state1 first;
 * actual START acknowledgement/state2 and normal MF/R3 gates remain separate.
 * No allocation/send/cache operation occurs in either caller-facing API.
 * ready:1 means both current leases are queued at RD,0 means pending,negative
 * means invalid scope/error. Caller must release its executor lock while
 * waiting, and must not send START until ready1. This is not a data/WR gate.
 */
int32_t shizuka_transport_prepare_reads(void *, uint64_t epoch);
int32_t shizuka_transport_prepared_reads_ready(void *, uint64_t epoch);
/* Sole producer: the HAL's existing GPR receiver callback thread. Copies only;
 * no transform, allocation, send, cache sync or wait occurs in this callback. */
int32_t shizuka_transport_receive(void *, const struct shizuka_transport_packet *);
/* All APIs except receive() require caller-provided serialization for the whole
 * context. There is no TLS or native-thread affinity: bind_graph(),
 * attest_profile(), graph_stage(), and get_status() may run on a GSL control
 * thread under the same executor lock as the worker. That lock must be released
 * before waiting for the worker, a GPR ACK, or the original GSL basic-response
 * wait. Host notify() must obey the synchronous, non-reentrant contract above.
 * Keep attach()/step()/detach() on the designated worker: they perform host
 * registration, allocation/free, cache synchronization, sends, and transforms.
 * receive() is the sole nonblocking SPSC producer and must never take this lock.
 */
int32_t shizuka_transport_step(void *);
int32_t shizuka_transport_get_status(void *, struct shizuka_transport_status *);
int32_t shizuka_transport_detach(void *);

#ifdef __cplusplus
}
#endif
#endif
