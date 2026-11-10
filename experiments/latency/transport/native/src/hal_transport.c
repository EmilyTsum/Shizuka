/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/hal_transport.h"

#define MAGIC 0x535a5452u
#define QUEUE_COUNT 64u
#define SLOT_COUNT 2u
#define EVENT_COUNT 8u
#define PEER_QUEUE_COUNT 16u
#define CONTROL_PAYLOAD_BYTES 1056u
#define VALID_TS 0x80000000u
#define BASIC_RESPONSE 0x02001005u
#define READY_QUERY 0x01001021u
#define READY_RESPONSE 0x02001007u
#define MAP_COMMAND 0x0100100cu
#define MAP_RESPONSE 0x02001001u
#define UNMAP_COMMAND 0x0100100du
#define REGISTER_EVENT 0x0100100eu
#define SET_CONFIGURATION 0x01001006u
#define WR_FORMAT 0x04001001u
#define RD_BUFFER 0x0400100bu
#define WR_BUFFER 0x0400100au
#define RD_DONE 0x05001005u
#define WR_DONE 0x05001004u
#define PEER_PROPERTY 0x080011c3u

enum control_kind { CONTROL_NONE, CONTROL_READY, CONTROL_MAP, CONTROL_EVENT,
    CONTROL_FORMAT, CONTROL_PEER, CONTROL_UNMAP };
enum slot_state { SLOT_EMPTY, SLOT_LOCAL, SLOT_RD, SLOT_RETURNED, SLOT_WR, SLOT_QUARANTINED };

struct queued_packet {
    uint32_t src_port, token, opcode, bytes,receipt;
    uint8_t payload[SHIZUKA_TRANSPORT_MAX_PACKET_PAYLOAD];
};
struct slot {
    struct shizuka_transport_memory memory;
    uint32_t allocated, cpu_owned, cpu_uncertain, handle, state;
    uint32_t token, wr_metadata_bytes, unmap_attempted, rewritten_wrappers;
    uint64_t deadline, read_order;
    uint8_t returned_read[56];
};
struct control {
    uint32_t kind, opcode, peer, token, index, timed_out;
    uint64_t deadline;
};
struct event_registration { uint32_t miid, event, token; };
struct peer_update {
    uint32_t peer, bytes, is_initial;
    uint8_t payload[68]; /* num_properties + at most eight property pairs. */
};
struct context {
    uint32_t magic, attached, phase;
    int32_t error;
    struct shizuka_transport_host host;
    struct shizuka_transport_options options;
    struct shizuka_transport_graph graph;
    uint32_t next_token, spf_ready, bound, configured, graph_prepared;
    uint32_t graph_started, graph_closed, need_close, reset_confirmed;
    uint32_t pause_requested, stop_acknowledged, resume_requested, ever_active;
    uint32_t allocations_done, maps_done, events_done, media_ready;
    uint32_t media_bytes, media_pending, initial_peers_queued;
    uint32_t initial_peer_acks, rd_started, rd_frame_bytes;
    uint32_t prestart_reads_requested,prepared_peer_queued,prepared_peer_acked;
    uint32_t prepared_barrier_completed,startup_empty_returns;
    uint32_t start_receive_cutoff,last_rd_state,rd_state_events;
    uint32_t wr_state_bytes,wr_state_events,last_wr_state;
    uint8_t media_format[SHIZUKA_TRANSPORT_MAX_PACKET_PAYLOAD];
    struct slot slots[SLOT_COUNT];
    struct event_registration events[EVENT_COUNT];
    struct control control;
    struct peer_update peer_queue[PEER_QUEUE_COUNT];
    uint32_t peer_head, peer_tail;
    uint8_t last_peer_payload[2][68];
    uint32_t last_peer_bytes[2];
    uint64_t last_clock, rd_frames, wr_frames, rewritten_wrappers, reads_submitted, next_read_order;
    uint64_t activation_deadline, pause_deadline;
    /* Existing HAL GPR receiver is the sole producer. All other APIs are on
     * one worker. Release/acquire publishes each complete bounded record. */
    uint32_t receive_head, receive_tail, receive_overflow;
    struct queued_packet received[QUEUE_COUNT];
};

static void copy_bytes(void *destination, const void *source, size_t bytes)
{
    uint8_t *d = (uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    size_t i;
    for (i = 0; i < bytes; ++i) d[i] = s[i];
}
static void zero_bytes(void *destination, size_t bytes)
{
    uint8_t *d = (uint8_t *)destination;
    size_t i;
    for (i = 0; i < bytes; ++i) d[i] = 0;
}
static int equal_bytes(const void *a, const void *b, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    size_t i;
    for (i = 0; i < bytes; ++i) if (p[i] != q[i]) return 0;
    return 1;
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static struct context *checked(void *storage)
{
    struct context *c = (struct context *)storage;
    return c && ((uintptr_t)c & 7u) == 0 && c->magic == MAGIC ? c : (struct context *)0;
}
static void notify(struct context *c)
{
    if (c->host.notify) c->host.notify(c->host.user, c->phase, c->error, c->need_close);
}
static void change_phase(struct context *c, uint32_t phase)
{
    if (c->phase != phase) { c->phase = phase; notify(c); }
}
static void fail(struct context *c, int32_t error, uint32_t uncertain)
{
    if (!c->error) c->error = error;
    if (c->bound && !c->graph_closed) c->need_close = 1;
    c->graph_started = 0;
    c->phase = uncertain ? SHIZUKA_TRANSPORT_UNCERTAIN : SHIZUKA_TRANSPORT_DRAINING;
    notify(c);
}
static uint32_t new_token(struct context *c)
{
    if (c->next_token == 0 || c->next_token == 0xffffffffu) {
        fail(c, SHIZUKA_TRANSPORT_BAD_STATE, 1); return 0;
    }
    return c->next_token++;
}
static int32_t send_packet(struct context *c, uint32_t peer, uint32_t token,
    uint32_t opcode, uint32_t client_data, const uint8_t *payload, uint32_t bytes)
{
    struct shizuka_transport_packet p;
    int32_t rc;
    zero_bytes(&p, sizeof(p));
    p.src_port = SHIZUKA_TRANSPORT_CLIENT_PORT; p.dst_port = peer;
    p.src_domain = 3; p.dst_domain = 2; p.token = token;
    p.opcode = opcode; p.client_data = (uint8_t)client_data;
    p.payload = payload; p.payload_bytes = bytes;
    rc = c->host.send(c->host.user, &p);
    if (rc) fail(c, SHIZUKA_TRANSPORT_HOST_ERROR, rc != 1);
    return rc;
}
static int32_t control_send(struct context *c, uint32_t kind, uint32_t index,
    uint32_t peer, uint32_t opcode, uint32_t client_data,
    const uint8_t *payload, uint32_t bytes)
{
    uint32_t token;
    int32_t rc;
    if (c->control.kind) return SHIZUKA_TRANSPORT_BAD_STATE;
    token = new_token(c); if (!token) return SHIZUKA_TRANSPORT_BAD_STATE;
    c->control.kind = kind; c->control.index = index;
    c->control.peer = peer; c->control.opcode = opcode;
    c->control.token = token; c->control.timed_out = 0;
    c->control.deadline = c->last_clock + c->options.control_timeout_ms;
    /* Publish expectation before send: a host adapter may immediately enqueue
     * a response, but receive() never executes this state machine recursively. */
    rc = send_packet(c, peer, token, opcode, client_data, payload, bytes);
    if (rc == 1 || (rc && kind != CONTROL_MAP && kind != CONTROL_UNMAP))
        c->control.kind = CONTROL_NONE;
    return rc;
}
static void initialize_events(struct context *c)
{
    static const uint32_t events[EVENT_COUNT] = {
        0x06001000u, 0x06001001u, 0x03001004u, 0x03001006u,
        0x03001007u, 0x06001003u, 0x03001009u, 0x03001005u
    };
    uint32_t i;
    for (i = 0; i < EVENT_COUNT; ++i) {
        c->events[i].miid = i == EVENT_COUNT - 1 ? SHIZUKA_TRANSPORT_WR_MIID : SHIZUKA_TRANSPORT_RD_MIID;
        c->events[i].event = events[i]; c->events[i].token = 0;
    }
}
static int32_t sync_cpu(struct context *c, struct slot *s, uint32_t begin)
{
    if (s->cpu_uncertain) return SHIZUKA_TRANSPORT_CLEANUP_UNKNOWN;
    if (s->cpu_owned == begin) return 0;
    if (c->host.cpu_sync(c->host.user, &s->memory, begin)) {
        s->cpu_uncertain = 1;
        fail(c, SHIZUKA_TRANSPORT_HOST_ERROR, 1); return SHIZUKA_TRANSPORT_HOST_ERROR;
    }
    s->cpu_owned = begin; return 0;
}
static void map_slot(struct context *c, uint32_t index)
{
    uint8_t payload[20];
    zero_bytes(payload, sizeof(payload));
    payload[0] = 3; payload[2] = 1; put32(payload + 4, 4);
    put32(payload + 8, (uint32_t)c->slots[index].memory.fd);
    put32(payload + 16, SHIZUKA_TRANSPORT_ALLOCATION_BYTES);
    (void)control_send(c, CONTROL_MAP, index, 1, MAP_COMMAND, 1, payload, sizeof(payload));
}
static void register_event(struct context *c, uint32_t index)
{
    uint8_t payload[40];
    zero_bytes(payload, sizeof(payload));
    put32(payload + 12, 24); /* in-band apm_cmd_header */
    put32(payload + 16, c->events[index].miid);
    put32(payload + 20, c->events[index].event); put32(payload + 24, 1);
    if (!control_send(c, CONTROL_EVENT, index, c->events[index].miid,
        REGISTER_EVENT, 0, payload, sizeof(payload)))
        c->events[index].token = c->control.token;
}
static void send_peer(struct context *c, const struct peer_update *update)
{
    uint8_t payload[104];
    zero_bytes(payload, sizeof(payload));
    uint32_t padded_bytes = (update->bytes + 7u) & ~7u;
    put32(payload + 12, 16 + padded_bytes);
    put32(payload + 16, update->peer); put32(payload + 20, PEER_PROPERTY);
    put32(payload + 24, update->bytes);
    copy_bytes(payload + 32, update->payload, update->bytes);
    (void)control_send(c, CONTROL_PEER, update->is_initial, update->peer,
        SET_CONFIGURATION, 0, payload, 32 + padded_bytes);
}
static void queue_peer(struct context *c, uint32_t peer, const uint8_t *payload,
    uint32_t bytes, uint32_t initial)
{
    uint32_t side = peer == SHIZUKA_TRANSPORT_RD_MIID ? 0 : 1;
    struct peer_update *u;
    if (bytes > 68 || bytes < 4) { fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return; }
    if (!initial && c->last_peer_bytes[side] == bytes &&
        equal_bytes(c->last_peer_payload[side], payload, bytes)) return;
    if ((uint32_t)(c->peer_head - c->peer_tail) == PEER_QUEUE_COUNT) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 1); return;
    }
    u = &c->peer_queue[c->peer_head % PEER_QUEUE_COUNT];
    u->peer = peer; u->bytes = bytes; u->is_initial = initial;
    copy_bytes(u->payload, payload, bytes);
    copy_bytes(c->last_peer_payload[side], payload, bytes); c->last_peer_bytes[side] = bytes;
    ++c->peer_head;
}
static void rd_peer_state(struct context *c,uint32_t state,uint32_t acknowledgement)
{
    uint8_t payload[12];
    put32(payload, 1); put32(payload + 4, 3); put32(payload + 8, state);
    /* TOPO_STATE propagates downstream-to-upstream to an external OUTPUT.
     * WR is an external INPUT: sending type3 there aliases its queue mask. */
    queue_peer(c, SHIZUKA_TRANSPORT_RD_MIID, payload, sizeof(payload), acknowledgement);
}
static void initial_peers(struct context *c)
{
    rd_peer_state(c,2,1);
    c->initial_peers_queued = 1;
}
static void clear_nonmemory_control_after_close(struct context *c)
{
    if (c->control.kind != CONTROL_MAP && c->control.kind != CONTROL_UNMAP)
        c->control.kind = CONTROL_NONE;
    c->peer_tail = c->peer_head;
}
static void handle_control(struct context *c, const struct queued_packet *p)
{
    struct control operation = c->control;
    uint32_t status = 0;
    if (!operation.kind || p->token != operation.token || p->src_port != operation.peer) return;
    if (p->opcode == BASIC_RESPONSE) {
        if (p->bytes != 8 || get32(p->payload) != operation.opcode) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED,
                operation.kind == CONTROL_MAP || operation.kind == CONTROL_UNMAP); return;
        }
        status = get32(p->payload + 4);
        /* MAP and READY need their specialized response on success. */
        if (!status && (operation.kind == CONTROL_MAP || operation.kind == CONTROL_READY)) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, operation.kind == CONTROL_MAP); return;
        }
    } else if (operation.kind == CONTROL_READY && p->opcode == READY_RESPONSE) {
        if (p->bytes != 4 || get32(p->payload) != 1) {
            c->control.kind = CONTROL_NONE; fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
        }
    } else if (operation.kind == CONTROL_MAP && p->opcode == MAP_RESPONSE) {
        uint32_t handle;
        if (p->bytes != 4 || !(handle = get32(p->payload))) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 1); return;
        }
        if (operation.index >= SLOT_COUNT || c->slots[operation.index].handle ||
            c->slots[1u - operation.index].handle == handle) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 1); return;
        }
        c->slots[operation.index].handle = handle;
    } else {
        fail(c, SHIZUKA_TRANSPORT_REJECTED,
            operation.kind == CONTROL_MAP || operation.kind == CONTROL_UNMAP); return;
    }
    c->control.kind = CONTROL_NONE;
    if (status) { fail(c, SHIZUKA_TRANSPORT_REJECTED, operation.kind == CONTROL_UNMAP); return; }
    switch (operation.kind) {
    case CONTROL_READY:
        c->spf_ready = 1;
        if (!c->bound && !c->error) change_phase(c, SHIZUKA_TRANSPORT_READY);
        break;
    case CONTROL_MAP: ++c->maps_done; break;
    case CONTROL_EVENT: ++c->events_done; break;
    case CONTROL_FORMAT: c->media_ready = 1; c->media_pending = 0; break;
    case CONTROL_PEER:
        if(operation.index==2)c->prepared_peer_acked=1;
        else c->initial_peer_acks |= operation.index;
        break;
    case CONTROL_UNMAP: c->slots[operation.index].handle = 0; break;
    default: break;
    }
}
static int read_ready(const struct context *c)
{
    /* Only RD receives the downstream STARTED seed. WR receives no invented
     * upstream state/RT value. Finish every actually observed peer update
     * before posting RD or forwarding any returned buffer. */
    return c->configured && c->graph.actual_init_profile_attested &&
        c->graph_prepared && c->graph_started && c->initial_peer_acks == 1 &&
        c->peer_head == c->peer_tail && c->control.kind != CONTROL_PEER &&
        c->rd_frame_bytes && !c->pause_requested &&
        !c->graph_closed && !c->error && !c->need_close;
}
static int prepared_read_ready(const struct context *c)
{
#if SHIZUKA_RELAY_RAW_ISLAND
    return c->prestart_reads_requested && c->prepared_peer_acked &&
        c->configured && c->graph.actual_init_profile_attested && c->graph_prepared &&
        !c->graph_started && !c->ever_active && c->rd_frame_bytes &&
        c->peer_head==c->peer_tail && c->control.kind!=CONTROL_PEER &&
        !c->pause_requested && !c->graph_closed && !c->error && !c->need_close;
#else
    (void)c;return 0;
#endif
}
static int prepared_leases_ready(const struct context *c)
{
    if(!prepared_read_ready(c) ||
        __atomic_load_n(&c->receive_head,__ATOMIC_ACQUIRE)!=
            __atomic_load_n(&c->receive_tail,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&c->receive_overflow,__ATOMIC_ACQUIRE))return 0;
    for(uint32_t i=0;i<SLOT_COUNT;i++)if(c->slots[i].state!=SLOT_RD)return 0;
    return 1;
}
static int startup_empty_candidate(const struct context *c,const struct slot *s)
{
#if SHIZUKA_RELAY_RAW_ISLAND
    return c->prestart_reads_requested && !c->rd_frames &&
        !get32(s->returned_read+16) && !get32(s->returned_read+52);
#else
    (void)c;(void)s;return 0;
#endif
}
static void activate_if_ready(struct context *c)
{
    if (read_ready(c) && c->media_ready) {
        c->ever_active = 1; c->activation_deadline = 0;
        change_phase(c, SHIZUKA_TRANSPORT_ACTIVE);
    }
}
static int write_credit_available(const struct context *c)
{
    /* Match the steady OLC policy: spdm_update_num_ipc_buffers requests one
     * WR pool node. This is a host credit policy, not a claimed DSP maximum.
     * Only a matching address-bearing WR_DONE returns this credit. */
    for (uint32_t i = 0; i < SLOT_COUNT; ++i)
        if (c->slots[i].state == SLOT_WR) return 0;
    return 1;
}
static void submit_read(struct context *c, uint32_t index)
{
    struct slot *s = &c->slots[index];
    uint8_t payload[32];
    uint32_t token;
    int32_t rc;
    if (!s->allocated || !s->handle || s->state != SLOT_LOCAL || sync_cpu(c, s, 0)) return;
    token = new_token(c); if (!token) return;
    zero_bytes(payload, sizeof(payload));
    put32(payload + 8, s->handle); put32(payload + 12, SHIZUKA_TRANSPORT_DATA_CAPACITY);
    put32(payload + 16, SHIZUKA_TRANSPORT_METADATA_OFFSET);
    put32(payload + 24, s->handle); put32(payload + 28, SHIZUKA_TRANSPORT_METADATA_CAPACITY);
    s->token = token; s->state = SLOT_RD; s->read_order = c->reads_submitted++;
    s->deadline = c->last_clock + c->options.buffer_timeout_ms;
    rc = send_packet(c, SHIZUKA_TRANSPORT_RD_MIID, token, RD_BUFFER, 0, payload, sizeof(payload));
    if (rc == 1) s->state = SLOT_LOCAL;
    else if (rc) s->state = SLOT_QUARANTINED;
}
static void handle_read(struct context *c, struct slot *s, const struct queued_packet *p)
{
    struct shizuka_transport_transform_result output;
    uint8_t payload[44];
    uint32_t size, md_size, rd_flags, token;
    uint64_t timestamp;
    int32_t rc;
    if (p->bytes != 56 || get32(p->payload + 4) || get32(p->payload + 8) ||
        get32(p->payload + 12) != s->handle ||
        get32(p->payload + 40) != SHIZUKA_TRANSPORT_METADATA_OFFSET ||
        get32(p->payload + 44) || get32(p->payload + 48) != s->handle) {
        s->state = SLOT_QUARANTINED; fail(c, SHIZUKA_TRANSPORT_REJECTED, 1); return;
    }
    /* Matching returned addresses establish the lease return even on a DSP
     * status error. Do not transform or reuse it when the status is bad. */
    s->state = SLOT_LOCAL;
    if (get32(p->payload) || get32(p->payload + 36)) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
    }
    if (c->error || c->need_close || c->graph_closed) return;
    size = get32(p->payload + 16); md_size = get32(p->payload + 52);
    rd_flags = get32(p->payload + 28);
    #if SHIZUKA_RELAY_RAW_ISLAND
    /* A changed MF can release a successful empty READ after the notification.
     * Invalid timestamps may retain old bits; flags0 is the authority. Accept
     * only the bounded initial two-lease bootstrap, never metadata-only data,
     * and never count/transform/forward this ownership-only completion. */
    if(c->prestart_reads_requested && !c->rd_frames && !size && !md_size &&
        !rd_flags && !get32(p->payload+32) && c->media_bytes &&
        c->startup_empty_returns<SLOT_COUNT) {
        ++c->startup_empty_returns;return;
    }
#endif
    if (c->pause_requested && !size && !md_size && !(rd_flags & ~VALID_TS) &&
        get32(p->payload + 32) <= 1) return; /* Empty successful STOP/flush return. */
    if (!size || size > SHIZUKA_TRANSPORT_DATA_CAPACITY || !md_size || (md_size & 3u) ||
        md_size > SHIZUKA_TRANSPORT_METADATA_CAPACITY || get32(p->payload + 32) != 1 ||
        (rd_flags & ~VALID_TS)) { fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return; }
    timestamp = (uint64_t)get32(p->payload + 20) | (uint64_t)get32(p->payload + 24) << 32;
    if (sync_cpu(c, s, 1)) return;
    if (c->pause_requested) {
        if (!c->host.validate_discard || !c->graph.actual_init_profile_attested ||
            c->host.validate_discard(c->host.user, c->graph.profile,
                s->memory.cpu_address, size, SHIZUKA_TRANSPORT_DATA_CAPACITY,
                s->memory.cpu_address + SHIZUKA_TRANSPORT_METADATA_OFFSET, md_size,
                SHIZUKA_TRANSPORT_METADATA_CAPACITY))
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 0);
        return;
    }
    zero_bytes(&output, sizeof(output));
    rc = c->host.transform(c->host.user, c->graph.profile, c->graph.delta_us,
        s->memory.cpu_address, size, SHIZUKA_TRANSPORT_DATA_CAPACITY,
        s->memory.cpu_address + SHIZUKA_TRANSPORT_METADATA_OFFSET, md_size,
        SHIZUKA_TRANSPORT_METADATA_CAPACITY, &output);
    if (rc || output.data_bytes != size || output.metadata_bytes > md_size ||
        (output.metadata_bytes & 3u) || output.wrappers_changed > size / 8 ||
        output.wr_flags != (0x10u | (rd_flags & VALID_TS)) ||
        ((rd_flags & VALID_TS) && output.timestamp_bits != timestamp)) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
    }
    ++c->rd_frames;
    if (sync_cpu(c, s, 0)) return;
    token = new_token(c); if (!token) return;
    zero_bytes(payload, sizeof(payload));
    put32(payload + 8, s->handle); put32(payload + 12, output.data_bytes);
    put32(payload + 16, (uint32_t)output.timestamp_bits);
    put32(payload + 20, (uint32_t)(output.timestamp_bits >> 32));
    /* Match OLC's timestamp-only flags. In particular, an R3 C0 supplemental
     * wrapper is a complete RD buffer, not proof of codec end-of-frame. The
     * parser's legacy frame-boundary bit must not force DSP CAPI EOF. */
    put32(payload + 24, shizuka_transport_wr_wire_flags(output.wr_flags));
    if (output.metadata_bytes) {
        put32(payload + 28, SHIZUKA_TRANSPORT_METADATA_OFFSET);
        put32(payload + 36, s->handle); put32(payload + 40, output.metadata_bytes);
    }
    s->wr_metadata_bytes = output.metadata_bytes; s->rewritten_wrappers = output.wrappers_changed;
    s->token = token; s->state = SLOT_WR;
    s->deadline = c->last_clock + c->options.buffer_timeout_ms;
    rc = send_packet(c, SHIZUKA_TRANSPORT_WR_MIID, token, WR_BUFFER, 0, payload, sizeof(payload));
    if (rc == 1) s->state = SLOT_LOCAL;
    else if (rc) s->state = SLOT_QUARANTINED;
}
static void handle_write(struct context *c, struct slot *s, const struct queued_packet *p)
{
    uint32_t md_address = s->wr_metadata_bytes ? SHIZUKA_TRANSPORT_METADATA_OFFSET : 0;
    uint32_t md_handle = s->wr_metadata_bytes ? s->handle : 0;
    if (p->bytes != 32 || get32(p->payload) || get32(p->payload + 4) ||
        get32(p->payload + 8) != s->handle || get32(p->payload + 16) != md_address ||
        get32(p->payload + 20) || get32(p->payload + 24) != md_handle) {
        s->state = SLOT_QUARANTINED; fail(c, SHIZUKA_TRANSPORT_REJECTED, 1); return;
    }
    s->state = SLOT_LOCAL;
    if (get32(p->payload + 12) || get32(p->payload + 28)) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
    }
    if (!c->pause_requested && !c->need_close && !c->graph_closed && !c->error) {
        ++c->wr_frames; c->rewritten_wrappers += s->rewritten_wrappers;
    }
}
static int event_matches(const struct context *c, const struct queued_packet *p)
{
    uint32_t i;
    for (i = 0; i < EVENT_COUNT; ++i)
        if (c->events[i].miid == p->src_port && c->events[i].event == p->opcode &&
            c->events[i].token && c->events[i].token == p->token) return 1;
    return 0;
}
static void handle_event(struct context *c, const struct queued_packet *p)
{
    uint32_t count, i, forwarded_count = 0;
    uint8_t forwarded[68];
    if (!event_matches(c, p) || !c->bound || c->graph_closed || c->need_close) return;
    if (p->opcode == 0x06001000u || p->opcode == 0x03001009u) {
        uint32_t declared;
        if (p->bytes < 12 || get32(p->payload) != 6 || !get32(p->payload + 4) ||
            get32(p->payload + 4) == 0xffffffffu ||
            (declared = get32(p->payload + 8)) > p->bytes - 12 ||
            p->bytes - 12 - declared > 3) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
        }
        for (i = 12 + declared; i < p->bytes; ++i)
            if (p->payload[i]) { fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return; }
        if (c->media_bytes) {
            if (c->media_bytes != p->bytes || !equal_bytes(c->media_format, p->payload, p->bytes))
                fail(c, SHIZUKA_TRANSPORT_REJECTED, 0);
            return;
        }
        c->media_bytes = p->bytes; c->media_pending = 1;
        copy_bytes(c->media_format, p->payload, p->bytes); return;
    }
    if (p->opcode == 0x03001004u) {
        if (p->bytes != 12 || get32(p->payload + 4) != 1 ||
            get32(p->payload + 8) != SHIZUKA_TRANSPORT_RD_MIID ||
            !get32(p->payload) || get32(p->payload) > SHIZUKA_TRANSPORT_DATA_CAPACITY) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
        }
        c->rd_frame_bytes = get32(p->payload); return;
    }
    if (p->opcode == 0x06001001u || p->opcode == 0x06001003u) {
        /* EOS/DFG ownership is intentionally not re-created by guessing an
         * internal metadata ACK. Owner must drain/close this fresh graph. */
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
    }
    if (p->bytes < 8 || get32(p->payload) != p->src_port ||
        !(count = get32(p->payload + 4)) || count > 8 || p->bytes != 8 + count * 8) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
    }
    /* Validate the whole event before changing state or queuing anything.
     * The common DSP property handler trusts the endpoint's port direction;
     * a syntactically valid type sent to the wrong endpoint corrupts it. */
    for (i = 0; i < count; ++i) {
        uint32_t type = get32(p->payload + 8 + i * 8);
        uint32_t value = get32(p->payload + 12 + i * 8);
        int valid = (p->opcode == 0x03001006u && type == 1 && value <= 1) ||
            (p->opcode == 0x03001005u && ((type == 2 && value <= 1) || (type == 3 && value <= 3))) ||
            (p->opcode == 0x03001007u && count == 1 && type == 3 && value <= 3);
        if (!valid) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
        }
    }
    if(p->opcode==0x03001005u) {
        uint32_t observed=0,value=UINT32_MAX;
        for(i=0;i<count;++i)if(get32(p->payload+8+i*8)==3) {
            observed=1;value=get32(p->payload+12+i*8);
        }
        if(observed) {
            c->wr_state_bytes=p->bytes;c->last_wr_state=value;
            if(c->wr_state_events!=UINT32_MAX)++c->wr_state_events;
        }
    }
    for (i = 0; i < count; ++i) {
        uint32_t type = get32(p->payload + 8 + i * 8);
        uint32_t value = get32(p->payload + 12 + i * 8);
        if (p->opcode == 0x03001007u) {
            c->last_rd_state=value;
            if(c->rd_state_events!=UINT32_MAX)++c->rd_state_events;
        }
        if (type == 3 && value != 2 && (c->graph_started || c->resume_requested) &&
            (!c->pause_requested || c->resume_requested)) {
            /* Only receipts already queued before the owner's START ACK may
             * be old PREPARE/STOP. The SPSC queue holds at most64 such records;
             * unsigned distance remains valid when its u32 index wraps. */
            uint32_t before=(uint32_t)(c->start_receive_cutoff-p->receipt);
            if(!before || before>QUEUE_COUNT) {
                fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return;
            }
            continue;
        }
        if (p->opcode == 0x03001007u) {
            c->rd_started = value == 2;
            /* This is RD's acknowledgement/observation, not WR input state.
             * Genuine OLC does not forward it to the external INPUT. */
            continue;
        }
        put32(forwarded + 4 + forwarded_count * 8, type);
        put32(forwarded + 8 + forwarded_count * 8, value); ++forwarded_count;
    }
    if (c->pause_requested || !forwarded_count) return;
    put32(forwarded, forwarded_count);
    queue_peer(c, p->src_port == SHIZUKA_TRANSPORT_RD_MIID ?
        SHIZUKA_TRANSPORT_WR_MIID : SHIZUKA_TRANSPORT_RD_MIID,
        forwarded, 4 + forwarded_count * 8, 0);
}
static void handle_packet(struct context *c, const struct queued_packet *p)
{
    uint32_t i;
    if (c->control.kind && p->token == c->control.token && p->src_port == c->control.peer &&
        (p->opcode == BASIC_RESPONSE || p->opcode == READY_RESPONSE || p->opcode == MAP_RESPONSE)) {
        handle_control(c, p); return;
    }
    for (i = 0; i < SLOT_COUNT; ++i) {
        struct slot *s = &c->slots[i];
        if (p->token != s->token || !s->token) continue;
        if (p->src_port == SHIZUKA_TRANSPORT_RD_MIID && s->state == SLOT_RD && p->opcode == RD_DONE) {
            /* Preserve stream order even if normalized callbacks are delivered
             * out of submission order. No frame can overtake an earlier slot. */
            copy_bytes(s->returned_read, p->payload, p->bytes < 56 ? p->bytes : 56);
            if (p->bytes != 56) {
                s->state = SLOT_QUARANTINED; fail(c, SHIZUKA_TRANSPORT_REJECTED, 1);
            } else s->state = SLOT_RETURNED;
            return;
        }
        if (p->src_port == SHIZUKA_TRANSPORT_WR_MIID && s->state == SLOT_WR && p->opcode == WR_DONE) {
            handle_write(c, s, p); return;
        }
        if (p->opcode == BASIC_RESPONSE &&
            ((s->state == SLOT_RD && p->src_port == SHIZUKA_TRANSPORT_RD_MIID) ||
             (s->state == SLOT_WR && p->src_port == SHIZUKA_TRANSPORT_WR_MIID))) {
            /* A basic error is not the address-bearing data completion. */
            s->state = SLOT_QUARANTINED; fail(c, SHIZUKA_TRANSPORT_REJECTED, 1); return;
        }
    }
    handle_event(c, p); /* Unknown, duplicate or stale token does not advance ownership. */
}

size_t shizuka_transport_context_bytes(void) { return sizeof(struct context); }

int32_t shizuka_transport_init(void *storage, size_t bytes,
    const struct shizuka_transport_host *host, const struct shizuka_transport_options *options)
{
    struct context *c = (struct context *)storage;
    if (!storage || ((uintptr_t)storage & 7u) || bytes < sizeof(*c) || !host || !options ||
        host->abi != SHIZUKA_TRANSPORT_ABI || host->bytes != sizeof(*host) ||
        options->abi != SHIZUKA_TRANSPORT_ABI || options->bytes != sizeof(*options) ||
        !host->register_port || !host->unregister_port || !host->send || !host->allocate ||
        !host->free || !host->cpu_sync || !host->transform || !host->clock_ms ||
        !options->token_seed || options->token_seed > 0xfffffe00u ||
        options->control_timeout_ms < 100 || options->control_timeout_ms > 30000 ||
        options->buffer_timeout_ms < 100 || options->buffer_timeout_ms > 60000 ||
        !options->hal_generation || !options->dsp_generation || options->reserved)
        return SHIZUKA_TRANSPORT_BAD_ARGUMENT;
    if (c->magic == MAGIC && c->attached) return SHIZUKA_TRANSPORT_BAD_STATE;
    zero_bytes(c, sizeof(*c)); c->magic = MAGIC;
    copy_bytes(&c->host, host, sizeof(*host)); copy_bytes(&c->options, options, sizeof(*options));
    c->next_token = options->token_seed;c->last_wr_state=UINT32_MAX; initialize_events(c);
    return 0;
}
int32_t shizuka_transport_attach(void *storage)
{
    struct context *c = checked(storage);
    if (!c || c->attached || c->phase != SHIZUKA_TRANSPORT_DETACHED) return SHIZUKA_TRANSPORT_BAD_STATE;
    if (c->host.register_port(c->host.user, SHIZUKA_TRANSPORT_CLIENT_PORT,
        shizuka_transport_receive, c)) return SHIZUKA_TRANSPORT_HOST_ERROR;
    c->attached = 1; change_phase(c, SHIZUKA_TRANSPORT_REGISTERED); return 0;
}
int32_t shizuka_transport_bind_graph(void *storage, const struct shizuka_transport_graph *graph)
{
    struct context *c = checked(storage);
    if (!c || !graph || !c->attached || !c->spf_ready || c->control.kind ||
        !(c->phase == SHIZUKA_TRANSPORT_READY || c->phase == SHIZUKA_TRANSPORT_CLOSED))
        return SHIZUKA_TRANSPORT_BAD_STATE;
    if (graph->abi != SHIZUKA_TRANSPORT_ABI || graph->bytes != sizeof(*graph) ||
        !graph->graph_epoch || graph->graph_epoch <= c->graph.graph_epoch ||
        graph->hal_generation != c->options.hal_generation ||
        graph->dsp_generation != c->options.dsp_generation || graph->delta_us > 524280 ||
        graph->delta_us % 8 || graph->profile != 1 || graph->subgraph_id != 0xb00000abu ||
        graph->container_id != 0xe00000e6u || graph->parent_client_id != SHIZUKA_TRANSPORT_CLIENT_PORT ||
        graph->rd_miid != SHIZUKA_TRANSPORT_RD_MIID || graph->wr_miid != SHIZUKA_TRANSPORT_WR_MIID ||
        graph->fresh_owned_graph != 1 || graph->endpoint_namespace_owned != 1 ||
        graph->original_imcl_preserved != 1 || graph->olc_parent_protocol_reviewed != 1 ||
        graph->actual_init_profile_attested > 1 || graph->open_acknowledged != 1)
        return SHIZUKA_TRANSPORT_REJECTED;
    copy_bytes(&c->graph, graph, sizeof(*graph));
    c->bound = 1; c->configured = c->graph_prepared = c->graph_started = c->graph_closed = 0;
    c->need_close = c->reset_confirmed = c->allocations_done = c->maps_done = c->events_done = 0;
    c->pause_requested = c->stop_acknowledged = c->resume_requested = c->ever_active = 0;
    c->activation_deadline = c->pause_deadline = 0;
    c->media_ready = c->media_bytes = c->media_pending = c->initial_peers_queued = 0;
    c->initial_peer_acks = c->rd_started = c->rd_frame_bytes = 0;
    c->prestart_reads_requested=c->prepared_peer_queued=c->prepared_peer_acked=0;
    c->prepared_barrier_completed=c->startup_empty_returns=0;
    c->start_receive_cutoff=c->rd_state_events=0;c->last_rd_state=UINT32_MAX;
    c->wr_state_bytes=c->wr_state_events=0;c->last_wr_state=UINT32_MAX;
    c->peer_head = c->peer_tail = 0; c->last_peer_bytes[0] = c->last_peer_bytes[1] = 0;
    c->error = 0; c->rd_frames = c->wr_frames = c->rewritten_wrappers = c->reads_submitted = c->next_read_order = 0;
    initialize_events(c); change_phase(c, SHIZUKA_TRANSPORT_CONFIGURING); return 0;
}
int32_t shizuka_transport_attest_profile(void *storage, uint64_t epoch, uint32_t profile,
    uint32_t accepted_init_profile_attested)
{
    struct context *c = checked(storage);
    if (!c || !c->bound || c->graph.graph_epoch != epoch || c->graph_closed || c->error || c->need_close)
        return SHIZUKA_TRANSPORT_REJECTED;
    if (profile != 1 || profile != c->graph.profile || accepted_init_profile_attested != 1) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return SHIZUKA_TRANSPORT_REJECTED;
    }
    c->graph.actual_init_profile_attested = 1;
    return 0;
}
int32_t shizuka_transport_prepare_reads(void *storage,uint64_t epoch)
{
#if SHIZUKA_RELAY_RAW_ISLAND
    struct context *c=checked(storage);
    if(!c || !c->attached || !c->bound || c->graph.graph_epoch!=epoch ||
        !c->configured || !c->graph.actual_init_profile_attested || !c->graph_prepared ||
        c->graph_started || c->ever_active || c->pause_requested || c->graph_closed ||
        c->error || c->need_close)return SHIZUKA_TRANSPORT_BAD_STATE;
    if(!c->prestart_reads_requested) {
        c->prestart_reads_requested=1;
        c->activation_deadline=c->last_clock+c->options.control_timeout_ms;
    }
    return 0;
#else
    (void)storage;(void)epoch;return SHIZUKA_TRANSPORT_BAD_STATE;
#endif
}
int32_t shizuka_transport_prepared_reads_ready(void *storage,uint64_t epoch)
{
    struct context *c=checked(storage);
    if(!c || !c->attached || !c->bound || c->graph.graph_epoch!=epoch ||
        !c->prestart_reads_requested || !c->graph_prepared || c->graph_started ||
        c->ever_active || c->pause_requested || c->graph_closed)return SHIZUKA_TRANSPORT_BAD_STATE;
    if(c->error || c->need_close)return c->error?c->error:SHIZUKA_TRANSPORT_BAD_STATE;
    return prepared_leases_ready(c)?1:0;
}
int32_t shizuka_transport_graph_stage(void *storage, uint64_t epoch, uint32_t stage,
    uint64_t independently_confirmed_dsp_generation)
{
    struct context *c = checked(storage);
    uint32_t i;
    if (!c || !c->attached || !c->bound || c->graph.graph_epoch != epoch) return SHIZUKA_TRANSPORT_BAD_STATE;
    if (stage == SHIZUKA_GRAPH_DSP_GENERATION_RESET) {
        if (!independently_confirmed_dsp_generation ||
            independently_confirmed_dsp_generation == c->options.dsp_generation)
            return SHIZUKA_TRANSPORT_REJECTED;
        c->reset_confirmed = 1; c->options.dsp_generation = independently_confirmed_dsp_generation;
        c->spf_ready = 0; c->control.kind = CONTROL_NONE;
        for (i = 0; i < SLOT_COUNT; ++i) c->slots[i].handle = 0;
        stage = SHIZUKA_GRAPH_CLOSED;
    } else if (independently_confirmed_dsp_generation) return SHIZUKA_TRANSPORT_BAD_ARGUMENT;
    if (stage == SHIZUKA_GRAPH_PREPARED && !c->graph_closed && !c->error && !c->need_close) c->graph_prepared = 1;
    else if (stage == SHIZUKA_GRAPH_STARTED && c->graph_prepared && !c->graph_closed && !c->error && !c->need_close) {
        if(c->prestart_reads_requested && !c->ever_active && !c->pause_requested && !c->graph_started &&
            !c->prepared_barrier_completed) {
            fail(c,SHIZUKA_TRANSPORT_REJECTED,0);return SHIZUKA_TRANSPORT_REJECTED;
        }
        if(!c->graph_started || c->pause_requested)
            c->start_receive_cutoff=__atomic_load_n(&c->receive_head,__ATOMIC_ACQUIRE);
        if (c->pause_requested) {
            if (!c->stop_acknowledged) return SHIZUKA_TRANSPORT_BAD_STATE;
            c->resume_requested = 1;
        } else if (!c->graph_started) {
            c->graph_started = 1;
            c->activation_deadline = c->last_clock + c->options.control_timeout_ms;
        }
    } else if (stage == SHIZUKA_GRAPH_PAUSING && c->configured && !c->graph_closed && !c->error && !c->need_close) {
        c->graph_started = 0; c->pause_requested = 1;
        c->stop_acknowledged = c->resume_requested = 0;
        c->activation_deadline = 0;
        c->pause_deadline = c->last_clock + c->options.control_timeout_ms;
        /* No memory command is pending after CONFIGURED. Format/property
         * commands are safe to abandon; they carry no shared buffer lease. */
        if (c->control.kind == CONTROL_FORMAT || c->control.kind == CONTROL_PEER)
            c->control.kind = CONTROL_NONE;
        c->peer_tail = c->peer_head; c->media_pending = 0;
        for (i = 0; i < SLOT_COUNT; ++i)
            if (c->slots[i].state == SLOT_RD || c->slots[i].state == SLOT_WR)
                c->slots[i].deadline = c->last_clock + c->options.buffer_timeout_ms;
        change_phase(c, SHIZUKA_TRANSPORT_PAUSING);
    } else if (stage == SHIZUKA_GRAPH_STOPPED && c->pause_requested && !c->graph_closed && !c->error && !c->need_close) {
        c->stop_acknowledged = 1; c->graph_started = 0; c->pause_deadline = 0;
    }
    else if (stage == SHIZUKA_GRAPH_STOPPING && !c->graph_closed) {
        c->graph_started = 0; c->need_close = 1; change_phase(c, SHIZUKA_TRANSPORT_DRAINING);
    } else if (stage == SHIZUKA_GRAPH_CLOSED) {
        c->graph_started = 0; c->graph_closed = 1; c->need_close = 0;
        c->pause_requested = c->stop_acknowledged = c->resume_requested = 0;
        c->activation_deadline = c->pause_deadline = 0;
        for (i = 0; i < SLOT_COUNT; ++i)
            if (c->slots[i].allocated) c->slots[i].state = SLOT_LOCAL;
        clear_nonmemory_control_after_close(c); change_phase(c, SHIZUKA_TRANSPORT_DRAINING);
    } else return SHIZUKA_TRANSPORT_BAD_STATE;
    return 0;
}
int32_t shizuka_transport_receive(void *storage, const struct shizuka_transport_packet *packet)
{
    struct context *c = checked(storage);
    struct queued_packet *out;
    uint32_t head, tail;
    if (!c || !packet || packet->src_domain != 2 || packet->dst_domain != 3 ||
        packet->dst_port != SHIZUKA_TRANSPORT_CLIENT_PORT || packet->reserved ||
        (packet->payload_bytes && !packet->payload)) return SHIZUKA_TRANSPORT_REJECTED;
    if (packet->payload_bytes > SHIZUKA_TRANSPORT_MAX_PACKET_PAYLOAD) {
        __atomic_store_n(&c->receive_overflow, 1u, __ATOMIC_RELEASE); return SHIZUKA_TRANSPORT_REJECTED;
    }
    head = __atomic_load_n(&c->receive_head, __ATOMIC_RELAXED);
    tail = __atomic_load_n(&c->receive_tail, __ATOMIC_ACQUIRE);
    if ((uint32_t)(head - tail) >= QUEUE_COUNT) {
        __atomic_store_n(&c->receive_overflow, 1u, __ATOMIC_RELEASE); return SHIZUKA_TRANSPORT_REJECTED;
    }
    out = &c->received[head % QUEUE_COUNT];
    out->src_port = packet->src_port; out->token = packet->token;
    out->receipt=head;
    out->opcode = packet->opcode; out->bytes = packet->payload_bytes;
    copy_bytes(out->payload, packet->payload, packet->payload_bytes);
    __atomic_store_n(&c->receive_head, head + 1u, __ATOMIC_RELEASE);
    return 0;
}
static void cleanup(struct context *c)
{
    uint32_t i;
    uint8_t payload[4];
    if (!c->graph_closed) return;
    if (c->control.kind) return; /* MAP/UNMAP may still resolve once; never retry. */
    for (i = 0; i < SLOT_COUNT; ++i) {
        struct slot *s = &c->slots[i];
        if (!s->allocated) continue;
        if (s->handle) {
            if (s->unmap_attempted) {
                change_phase(c, SHIZUKA_TRANSPORT_UNCERTAIN); return;
            }
            put32(payload, s->handle);
            s->unmap_attempted = 1;
            (void)control_send(c, CONTROL_UNMAP, i, 1, UNMAP_COMMAND, 0, payload, sizeof(payload));
            return;
        }
        if (sync_cpu(c, s, 1)) { change_phase(c, SHIZUKA_TRANSPORT_UNCERTAIN); return; }
        if (c->host.free(c->host.user, &s->memory)) {
            s->cpu_uncertain = 1; fail(c, SHIZUKA_TRANSPORT_HOST_ERROR, 1); return;
        }
        zero_bytes(s, sizeof(*s));
    }
    change_phase(c, SHIZUKA_TRANSPORT_CLOSED);
}
int32_t shizuka_transport_step(void *storage)
{
    struct context *c = checked(storage);
    uint32_t head, tail, i;
    uint64_t now;
    if (!c || !c->attached) return SHIZUKA_TRANSPORT_BAD_STATE;
    now = c->host.clock_ms(c->host.user);
    if (now < c->last_clock || now > UINT64_MAX - 60000u) {
        fail(c, SHIZUKA_TRANSPORT_HOST_ERROR, 1); return SHIZUKA_TRANSPORT_HOST_ERROR;
    }
    c->last_clock = now;
    if (!c->error && !c->need_close &&
        ((c->activation_deadline && now >= c->activation_deadline && c->phase != SHIZUKA_TRANSPORT_ACTIVE) ||
         (c->pause_deadline && now >= c->pause_deadline && !c->stop_acknowledged)))
        fail(c, SHIZUKA_TRANSPORT_CLEANUP_UNKNOWN, 1);
    /* Expire before consuming queued replies. A reply observed after the
     * bound can resolve its original MAP/UNMAP cleanup, but cannot silently
     * revive data activation after an unobserved deadline. */
    if (c->control.kind && !c->control.timed_out && now >= c->control.deadline) {
        uint32_t memory_operation = c->control.kind == CONTROL_MAP || c->control.kind == CONTROL_UNMAP;
        c->control.timed_out = 1;
        if (!memory_operation) c->control.kind = CONTROL_NONE;
        fail(c, SHIZUKA_TRANSPORT_CLEANUP_UNKNOWN, memory_operation);
    }
    for (i = 0; i < SLOT_COUNT; ++i) {
        struct slot *s = &c->slots[i];
        if ((s->state == SLOT_RD || s->state == SLOT_WR) && now >= s->deadline) {
            s->state = SLOT_QUARANTINED; fail(c, SHIZUKA_TRANSPORT_CLEANUP_UNKNOWN, 1);
        }
    }
    if (__atomic_load_n(&c->receive_overflow, __ATOMIC_ACQUIRE) && !c->error) {
        fail(c, SHIZUKA_TRANSPORT_REJECTED, 1);
        /* Queue is still drained to recover any exact pending owned handle. */
    }
    tail = __atomic_load_n(&c->receive_tail, __ATOMIC_RELAXED);
    head = __atomic_load_n(&c->receive_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        struct queued_packet p;
        copy_bytes(&p, &c->received[tail % QUEUE_COUNT], sizeof(p));
        __atomic_store_n(&c->receive_tail, ++tail, __ATOMIC_RELEASE);
        handle_packet(c, &p);
        head = __atomic_load_n(&c->receive_head, __ATOMIC_ACQUIRE);
    }
    activate_if_ready(c);
    for (;;) {
        struct slot *next = (struct slot *)0;
        struct queued_packet p;
        /* The real OLC posts RD buffers on OPFS before output media format is
         * available. Keep complete returned buffers in submission order until
         * WR has accepted that actual format and every data gate is positive.
         * Pause/fault/close still drains exact returns through handle_read's
         * ownership checks, without forwarding them to WR. */
        for (i = 0; i < SLOT_COUNT; ++i)
            if (c->slots[i].state == SLOT_RETURNED && c->slots[i].read_order == c->next_read_order)
                next = &c->slots[i];
        if (!next) break;
        /* A RAW startup MF-only notification may return an empty owned lease.
         * Recycle only that exact zero/zero shape before data gates; malformed
         * fields still fail handle_read. Nonempty returns wait for START/MF. */
        if (!startup_empty_candidate(c,next)) {
            if (!(read_ready(c) && c->media_ready) && !c->pause_requested &&
                !c->error && !c->need_close && !c->graph_closed) break;
            /* One steady WR credit; cleanup still drains exact RD ownership. */
            if (!write_credit_available(c) && !c->pause_requested && !c->error &&
                !c->need_close && !c->graph_closed) break;
        }
        zero_bytes(&p, sizeof(p)); p.bytes = 56;
        copy_bytes(p.payload, next->returned_read, 56);
        ++c->next_read_order; handle_read(c, next, &p);
    }
    if (c->graph_closed) { cleanup(c); return c->error; }
    if (c->error || c->need_close) return c->error ? c->error : SHIZUKA_TRANSPORT_BAD_STATE;
    if (c->pause_requested) {
        uint32_t all_returned = 1;
        for (i = 0; i < SLOT_COUNT; ++i) if (c->slots[i].state != SLOT_LOCAL) all_returned = 0;
        if (!c->stop_acknowledged || !all_returned || c->control.kind) return 0;
        if (!c->resume_requested) { change_phase(c, SHIZUKA_TRANSPORT_PAUSED); return 0; }
        /* Old STOP events were drained while pause_requested was still set.
         * Reuse the epoch's accepted INIT/delta and owned maps/registrations. */
        c->pause_requested = c->stop_acknowledged = c->resume_requested = 0;
        c->graph_started = 1; c->initial_peers_queued = c->initial_peer_acks = 0;
        c->activation_deadline = now + c->options.control_timeout_ms;
        c->rd_started = 0; c->rd_frame_bytes = 0;
        c->media_ready = 0; c->media_pending = c->media_bytes != 0;
        c->peer_tail = c->peer_head; c->last_peer_bytes[0] = c->last_peer_bytes[1] = 0;
        change_phase(c, SHIZUKA_TRANSPORT_CONFIGURED);
    }
    if (!c->spf_ready) {
        if (!c->control.kind) (void)control_send(c, CONTROL_READY, 0, 1, READY_QUERY, 0, (const uint8_t *)0, 0);
        return c->error;
    }
    if (!c->bound) return 0;
    if (c->allocations_done < SLOT_COUNT) {
        struct slot *s = &c->slots[c->allocations_done];
        zero_bytes(&s->memory, sizeof(s->memory)); s->memory.fd = -1; s->memory.subsystem_id = 2;
        if (c->host.allocate(c->host.user, &s->memory)) {
            fail(c, SHIZUKA_TRANSPORT_HOST_ERROR, 0); return c->error;
        }
        s->allocated = 1; s->cpu_owned = 1; s->state = SLOT_LOCAL;
        if (!s->memory.cpu_address || ((uintptr_t)s->memory.cpu_address & 63u) ||
            (uintptr_t)s->memory.cpu_address > UINTPTR_MAX - SHIZUKA_TRANSPORT_ALLOCATION_BYTES ||
            s->memory.capacity != SHIZUKA_TRANSPORT_ALLOCATION_BYTES ||
            s->memory.fd < 0 || s->memory.offset_mode != 1 || s->memory.subsystem_id != 2) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return c->error;
        }
        if (c->allocations_done && (s->memory.fd == c->slots[0].memory.fd ||
            ((uintptr_t)s->memory.cpu_address < (uintptr_t)c->slots[0].memory.cpu_address + SHIZUKA_TRANSPORT_ALLOCATION_BYTES &&
             (uintptr_t)c->slots[0].memory.cpu_address < (uintptr_t)s->memory.cpu_address + SHIZUKA_TRANSPORT_ALLOCATION_BYTES))) {
            fail(c, SHIZUKA_TRANSPORT_REJECTED, 0); return c->error;
        }
        ++c->allocations_done;
    }
    if (!c->control.kind && c->maps_done < c->allocations_done) map_slot(c, c->maps_done);
    if (!c->control.kind && c->maps_done == SLOT_COUNT && c->events_done < EVENT_COUNT)
        register_event(c, c->events_done);
    if (c->events_done == EVENT_COUNT && !c->configured) {
        c->configured = 1; change_phase(c, SHIZUKA_TRANSPORT_CONFIGURED);
    }
    if(c->prestart_reads_requested && c->graph_prepared && !c->graph_started &&
        !c->prepared_peer_queued && !c->ever_active) {
        rd_peer_state(c,1,2);c->prepared_peer_queued=1;
    }
    if (c->configured && c->graph_started && !c->initial_peers_queued) initial_peers(c);
    if (!c->control.kind && c->configured) {
        if (c->peer_tail != c->peer_head) {
            struct peer_update update;
            copy_bytes(&update, &c->peer_queue[c->peer_tail % PEER_QUEUE_COUNT], sizeof(update));
            ++c->peer_tail; send_peer(c, &update);
        } else if (c->media_pending && c->media_bytes) {
            (void)control_send(c, CONTROL_FORMAT, 0, SHIZUKA_TRANSPORT_WR_MIID,
                WR_FORMAT, 0, c->media_format, c->media_bytes);
        }
    }
    if (read_ready(c) || prepared_read_ready(c)) {
        /* Bootstrap only the bounded owned RD leases. MEDIA_FORMAT/WR-format
         * ACK and the unchanged strict transform are required before any WR. */
        activate_if_ready(c);
        for (i = 0; i < SLOT_COUNT && !c->error; ++i)
            if (c->slots[i].state == SLOT_LOCAL) submit_read(c, i);
    }
    /* Latch completion, not mutable lease state: a real frame may be returned
     * after owner START was sent but before its synchronous ACK is published. */
    if(prepared_leases_ready(c))c->prepared_barrier_completed=1;
    return c->error;
}
int32_t shizuka_transport_get_status(void *storage, struct shizuka_transport_status *status)
{
    struct context *c = checked(storage);
    uint32_t i;
    if (!c || !status || status->abi != SHIZUKA_TRANSPORT_ABI || status->bytes != sizeof(*status))
        return SHIZUKA_TRANSPORT_BAD_ARGUMENT;
    zero_bytes(status, sizeof(*status)); status->abi = SHIZUKA_TRANSPORT_ABI; status->bytes = sizeof(*status);
    status->phase = c->phase; status->error = c->error; status->graph_epoch = c->graph.graph_epoch;
    status->delta_us = c->graph.delta_us; status->rd_frames = c->rd_frames; status->wr_frames = c->wr_frames;
    status->rewritten_wrappers = c->rewritten_wrappers;
    status->registration_ready = c->attached && c->spf_ready && !c->error;
    status->configuration_ready = c->configured; status->media_format_ready = c->media_ready;
    status->media_format_bytes = c->media_bytes;
    status->startup_flags=(c->configured?1u:0u)|(c->graph.actual_init_profile_attested?2u:0u)|
        (c->graph_prepared?4u:0u)|(c->graph_started?8u:0u)|(c->initial_peer_acks==1?16u:0u)|
        (c->rd_frame_bytes?32u:0u)|(c->media_ready?64u:0u)|(c->rd_started?128u:0u)|
        (c->pause_requested?256u:0u)|(c->need_close?512u:0u)|(c->error?1024u:0u)|(c->graph_closed?2048u:0u)|
        (c->prestart_reads_requested?4096u:0u)|(c->prepared_peer_acked?8192u:0u)|
        (c->prepared_barrier_completed?16384u:0u);
    status->rd_frame_bytes=c->rd_frame_bytes;status->initial_peer_acks=c->initial_peer_acks;
    status->last_rd_state=c->last_rd_state;status->rd_state_events=c->rd_state_events;
    status->wr_state_bytes=c->wr_state_bytes;status->wr_state_events=c->wr_state_events;
    status->last_wr_state=c->last_wr_state;
    status->wr_peer_bytes=c->last_peer_bytes[1];
    if(c->last_peer_bytes[1]>=4)status->wr_peer_count=get32(c->last_peer_payload[1]);
    if(c->last_peer_bytes[1]>=12){
        status->wr_peer_type=get32(c->last_peer_payload[1]+4);
        status->wr_peer_value=get32(c->last_peer_payload[1]+8);
    }
    status->need_graph_close = c->need_close; status->pending_control = c->control.kind;
    status->received_overflow = __atomic_load_n(&c->receive_overflow, __ATOMIC_ACQUIRE);
    status->pause_ready = c->phase == SHIZUKA_TRANSPORT_PAUSED;
    for (i = 0; i < SLOT_COUNT; ++i) {
        if (c->slots[i].allocated) ++status->unresolved_allocations;
        if (c->slots[i].handle) ++status->unresolved_mappings;
        if (c->slots[i].state == SLOT_RD || c->slots[i].state == SLOT_RETURNED ||
            c->slots[i].state == SLOT_WR || c->slots[i].state == SLOT_QUARANTINED)
            ++status->unresolved_buffers;
    }
    if (c->control.kind == CONTROL_MAP) ++status->unresolved_mappings;
    return 0;
}
int32_t shizuka_transport_detach(void *storage)
{
    struct context *c = checked(storage);
    uint32_t i;
    if (!c || !c->attached || c->control.kind || (c->bound && c->phase != SHIZUKA_TRANSPORT_CLOSED))
        return SHIZUKA_TRANSPORT_BAD_STATE;
    for (i = 0; i < SLOT_COUNT; ++i) if (c->slots[i].allocated) return SHIZUKA_TRANSPORT_CLEANUP_UNKNOWN;
    if (c->host.unregister_port(c->host.user, SHIZUKA_TRANSPORT_CLIENT_PORT)) return SHIZUKA_TRANSPORT_HOST_ERROR;
    c->attached = 0; change_phase(c, SHIZUKA_TRANSPORT_DETACHED); return 0;
}
