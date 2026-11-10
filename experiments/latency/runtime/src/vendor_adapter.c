/* SPDX-License-Identifier: GPL-2.0-only */
#include "shizuka/vendor_adapter.h"

#define ADAPTER_MAGIC 0x535a5641u
#define MEMORY_COUNT 2u
#define DMA_BUF_IOCTL_SYNC 0x40086200u
#define GPR_HEADER_BYTES 24u

_Static_assert(sizeof(void *) == 8, "reviewed adapter is ARM64/64-bit only");
_Static_assert(sizeof(struct shizuka_vendor_gpr_packet) == 24, "GPR packet ABI");
_Static_assert(offsetof(struct shizuka_vendor_gpr_packet, src_port) == 8, "GPR source port");
_Static_assert(offsetof(struct shizuka_vendor_gpr_packet, dst_port) == 12, "GPR destination port");
_Static_assert(offsetof(struct shizuka_vendor_gpr_packet, opcode) == 20, "GPR opcode");
_Static_assert(sizeof(struct shizuka_vendor_gpr_alloc) == 40, "GPR alloc ABI");
_Static_assert(offsetof(struct shizuka_vendor_gpr_alloc, src_port) == 4, "alloc source port");
_Static_assert(offsetof(struct shizuka_vendor_gpr_alloc, dst_domain) == 8, "alloc destination domain");
_Static_assert(offsetof(struct shizuka_vendor_gpr_alloc, dst_port) == 12, "alloc destination port");
_Static_assert(offsetof(struct shizuka_vendor_gpr_alloc, client_data) == 16, "alloc client data");
_Static_assert(offsetof(struct shizuka_vendor_gpr_alloc, token) == 20, "alloc token");
_Static_assert(offsetof(struct shizuka_vendor_gpr_alloc, ret_packet) == 32, "alloc output pointer");
_Static_assert(sizeof(struct shizuka_vendor_shmem) == 80, "OSAL layout-v1 ABI");
_Static_assert(offsetof(struct shizuka_vendor_shmem, buf_size) == 8, "OSAL size");
_Static_assert(offsetof(struct shizuka_vendor_shmem, mem_type) == 16, "OSAL memory type");
_Static_assert(offsetof(struct shizuka_vendor_shmem, ipa_lsw) == 24, "OSAL IPA/FD");
_Static_assert(offsetof(struct shizuka_vendor_shmem, vaddr) == 40, "OSAL CPU address");
_Static_assert(offsetof(struct shizuka_vendor_shmem, metadata) == 48, "OSAL private metadata");
_Static_assert(offsetof(struct shizuka_vendor_shmem, num_sys_id) == 56, "OSAL u8 subsystem count");
_Static_assert(offsetof(struct shizuka_vendor_shmem, sys_id) == 64, "OSAL u8 subsystem list");
_Static_assert(offsetof(struct shizuka_vendor_shmem, platform_info) == 72, "OSAL platform info");
_Static_assert(offsetof(struct shizuka_vendor_shmem, flags) == 76, "OSAL flags");
_Static_assert(sizeof(((struct shizuka_transport_memory *)0)->allocator_cookie) == 80, "cookie capacity");

struct memory_record {
    struct shizuka_transport_memory *owner;
    struct shizuka_vendor_shmem info;
    uint32_t cpu_owned, quarantined, free_attempted;
};
struct vendor_adapter {
    uint32_t magic, registered, deregister_attempted, retired;
    uint32_t accepting, inflight, delivery_active;
    struct shizuka_vendor_functions functions;
    shizuka_transport_receive_fn receive;
    void *receive_context;
    struct memory_record memory[MEMORY_COUNT];
    uint32_t packets_received, packets_delivered, packets_rejected;
    uint32_t packets_free_attempted, packets_free_failed;
    uint32_t sends_accepted, sends_failed;
};

static void copy_bytes(void *out, const void *in, size_t bytes)
{
    uint8_t *d = out; const uint8_t *s = in; size_t i;
    for (i = 0; i < bytes; ++i) d[i] = s[i];
}
static void zero_bytes(void *out, size_t bytes)
{
    uint8_t *d = out; size_t i; for (i = 0; i < bytes; ++i) d[i] = 0;
}
static int equal_bytes(const void *a, const void *b, size_t bytes)
{
    const uint8_t *p = a, *q = b; size_t i;
    for (i = 0; i < bytes; ++i) if (p[i] != q[i]) return 0;
    return 1;
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static struct vendor_adapter *checked(void *storage)
{
    struct vendor_adapter *a = storage;
    return a && !((uintptr_t)a & 7u) && a->magic == ADAPTER_MAGIC ? a : (struct vendor_adapter *)0;
}
static void receive_error(struct vendor_adapter *a, uint32_t reason)
{
    __atomic_add_fetch(&a->packets_rejected, 1u, __ATOMIC_RELAXED);
    a->functions.receive_error(a->functions.user, reason);
}
static uint32_t free_packet(struct vendor_adapter *a, struct shizuka_vendor_gpr_packet *p)
{
    uint32_t rc;
    __atomic_add_fetch(&a->packets_free_attempted, 1u, __ATOMIC_RELAXED);
    rc = a->functions.gpr_free(p);
    if (rc) {
        __atomic_add_fetch(&a->packets_free_failed, 1u, __ATOMIC_RELAXED);
        receive_error(a, SHIZUKA_VENDOR_PACKET_FREE_FAILED);
    }
    return rc;
}
static int32_t register_port(void *storage, uint32_t port, shizuka_transport_receive_fn receive, void *context)
{
    struct vendor_adapter *a = checked(storage);
    uint32_t rc;
    if (!a || port != SHIZUKA_TRANSPORT_CLIENT_PORT || !receive || !context ||
        a->registered || a->retired || a->deregister_attempted ||
        __atomic_load_n(&a->inflight, __ATOMIC_SEQ_CST)) return -1;
    a->receive = receive; a->receive_context = context;
    __atomic_store_n(&a->accepting, 1u, __ATOMIC_SEQ_CST);
    rc = a->functions.gpr_register(port, a->functions.callback_entry, a);
    if (rc) {
        __atomic_store_n(&a->accepting, 0u, __ATOMIC_SEQ_CST); return -1;
    }
    a->registered = 1; return 0;
}
static int32_t unregister_port(void *storage, uint32_t port)
{
    struct vendor_adapter *a = checked(storage);
    uint32_t i;
    if (!a || port != SHIZUKA_TRANSPORT_CLIENT_PORT) return -1;
    /* A quarantined allocator object is not silently discarded on detach. */
    for (i = 0; i < MEMORY_COUNT; ++i) if (a->memory[i].owner) return -1;
    __atomic_store_n(&a->accepting, 0u, __ATOMIC_SEQ_CST);
    if (a->registered) {
        if (a->deregister_attempted) return -1;
        a->deregister_attempted = 1;
        if (a->functions.gpr_deregister(port)) return -1;
        a->registered = 0; a->retired = 1;
    } else if (!a->retired) return -1;
    if (__atomic_load_n(&a->inflight, __ATOMIC_SEQ_CST)) return -1;
    a->receive = (shizuka_transport_receive_fn)0; a->receive_context = (void *)0;
    return 0;
}
static int allowed_send(const struct shizuka_transport_packet *p)
{
    if (!p || p->src_domain != 3 || p->dst_domain != 2 || p->reserved ||
        p->src_port != SHIZUKA_TRANSPORT_CLIENT_PORT || !p->token ||
        p->payload_bytes > SHIZUKA_TRANSPORT_MAX_PACKET_PAYLOAD ||
        (p->payload_bytes && !p->payload)) return 0;
    if (p->opcode == 0x01001021u)
        return p->dst_port == 1 && !p->payload_bytes && !p->client_data;
    if (p->opcode == 0x0100100cu)
        return p->dst_port == 1 && p->payload_bytes == 20 && p->client_data == 1;
    if (p->opcode == 0x0100100du)
        return p->dst_port == 1 && p->payload_bytes == 4 && !p->client_data;
    if (p->client_data || !(p->dst_port == SHIZUKA_TRANSPORT_RD_MIID || p->dst_port == SHIZUKA_TRANSPORT_WR_MIID)) return 0;
    if (p->opcode == 0x0100100eu) return p->payload_bytes == 40;
    if (p->opcode == 0x01001006u) return p->payload_bytes >= 40 && p->payload_bytes <= 104 && !(p->payload_bytes & 7u);
    if (p->opcode == 0x04001001u) return p->dst_port == SHIZUKA_TRANSPORT_WR_MIID && p->payload_bytes >= 12;
    if (p->opcode == 0x0400100bu) return p->dst_port == SHIZUKA_TRANSPORT_RD_MIID && p->payload_bytes == 32;
    if (p->opcode == 0x0400100au) return p->dst_port == SHIZUKA_TRANSPORT_WR_MIID && p->payload_bytes == 44;
    return 0;
}
static int32_t send_packet(void *storage, const struct shizuka_transport_packet *p)
{
    struct vendor_adapter *a = checked(storage);
    struct shizuka_vendor_gpr_alloc request;
    struct shizuka_vendor_gpr_packet *packet = (struct shizuka_vendor_gpr_packet *)0;
    const uint8_t *b;
    uint32_t rc, i;
    if (!a || !a->registered || !__atomic_load_n(&a->accepting, __ATOMIC_SEQ_CST) || !allowed_send(p)) return 1;
    if (p->opcode == 0x0100100cu) {
        uint32_t fd = get32(p->payload + 8), found = 0;
        if (get32(p->payload) != 0x00010003u || get32(p->payload + 4) != 4 ||
            get32(p->payload + 12) || get32(p->payload + 16) != SHIZUKA_TRANSPORT_ALLOCATION_BYTES) return 1;
        for (i = 0; i < MEMORY_COUNT; ++i)
            if (a->memory[i].owner && !a->memory[i].quarantined && a->memory[i].info.ipa_lsw == fd) found = 1;
        if (!found) return 1;
    }
    zero_bytes(&request, sizeof(request));
    request.src_domain = p->src_domain; request.src_port = p->src_port;
    request.dst_domain = p->dst_domain; request.dst_port = p->dst_port;
    request.client_data = p->client_data; request.token = p->token;
    request.opcode = p->opcode; request.payload_size = p->payload_bytes; request.ret_packet = &packet;
    rc = a->functions.gpr_alloc(&request);
    if (rc || !packet) {
        if (packet) (void)free_packet(a, packet);
        ++a->sends_failed; return 1; /* No send was attempted. */
    }
    b = (const uint8_t *)packet;
    if (((uintptr_t)packet & 3u) || get32(b) != ((GPR_HEADER_BYTES + p->payload_bytes) << 8 | 0x60u) ||
        b[4] != p->dst_domain || b[5] != p->src_domain || b[6] != p->client_data || b[7] ||
        get32(b + 8) != p->src_port || get32(b + 12) != p->dst_port ||
        get32(b + 16) != p->token || get32(b + 20) != p->opcode) {
        (void)free_packet(a, packet); ++a->sends_failed; return 1;
    }
    copy_bytes((uint8_t *)packet + GPR_HEADER_BYTES, p->payload, p->payload_bytes);
    rc = a->functions.gpr_send(packet);
    if (rc) {
        /* API returns packet ownership on failure, but does not establish
         * absence of a remote side effect. Conservatively report UNKNOWN. */
        (void)free_packet(a, packet); ++a->sends_failed; return 2;
    }
    ++a->sends_accepted; return 0; /* Accepted packet belongs to vendor GPR. */
}
static struct memory_record *find_memory(struct vendor_adapter *a, struct shizuka_transport_memory *m)
{
    uint32_t i; for (i = 0; i < MEMORY_COUNT; ++i) if (a->memory[i].owner == m) return &a->memory[i];
    return (struct memory_record *)0;
}
static int memory_matches(const struct memory_record *r, const struct shizuka_transport_memory *m)
{
    return r && !r->quarantined && !r->free_attempted && r->owner == m &&
        r->info.sys_id == &m->subsystem_id && m->subsystem_id == 2 &&
        m->cpu_address == r->info.vaddr && m->capacity == r->info.buf_size &&
        m->fd >= 0 && (uint32_t)m->fd == r->info.ipa_lsw && m->offset_mode == 1 &&
        equal_bytes(m->allocator_cookie, &r->info, sizeof(r->info));
}
static int32_t allocate_memory(void *storage, struct shizuka_transport_memory *m)
{
    struct vendor_adapter *a = checked(storage);
    struct memory_record *record = (struct memory_record *)0;
    struct shizuka_vendor_shmem info;
    uint32_t i;
    int32_t rc;
    if (!a || !m || ((uintptr_t)m & 7u) || m->subsystem_id != 2 || m->cpu_address ||
        m->capacity || m->fd != -1 || m->offset_mode || find_memory(a, m)) return -1;
    for (i = 0; i < MEMORY_COUNT; ++i) {
        if (a->memory[i].quarantined) return -1;
        if (!a->memory[i].owner && !record) record = &a->memory[i];
    }
    if (!record) return -1;
    zero_bytes(&info, sizeof(info));
    info.buf_size = SHIZUKA_TRANSPORT_ALLOCATION_BYTES;
    info.num_sys_id = 1; info.sys_id = &m->subsystem_id;
    rc = a->functions.shmem_alloc(&info);
    /* OSAL calls are synchronous and retain only the returned private metadata
     * and the stable byte sys-id pointer, not this transient POD's address.
     * Byte-copy the representation to avoid aliasing the opaque u64 cookie. */
    copy_bytes(m->allocator_cookie, &info, sizeof(info));
    if (rc) {
        if (info.vaddr || info.metadata || info.ipa_lsw || info.ipa_msw) {
            record->owner = m; copy_bytes(&record->info, &info, sizeof(info)); record->quarantined = 1;
        }
        return -1; /* Vendor handles its failure cleanup; never call free twice. */
    }
    record->owner = m; record->cpu_owned = 1; copy_bytes(&record->info, &info, sizeof(info));
    if (info.cache_type || info.buf_size != SHIZUKA_TRANSPORT_ALLOCATION_BYTES || info.mem_type ||
        info.index_type != 1 || info.ipa_lsw > 0x7fffffffu || info.ipa_msw || !info.vaddr ||
        ((uintptr_t)info.vaddr & 63u) || !info.metadata || info.num_sys_id != 1 ||
        info.sys_id != &m->subsystem_id || info.platform_info || info.flags) {
        record->quarantined = 1; return -1;
    }
    m->cpu_address = info.vaddr; m->capacity = (uint32_t)info.buf_size;
    m->fd = (int32_t)info.ipa_lsw; m->offset_mode = 1;
    return 0;
}
static int32_t release_memory(void *storage, struct shizuka_transport_memory *m)
{
    struct vendor_adapter *a = checked(storage);
    struct memory_record *record;
    struct shizuka_vendor_shmem info;
    if (!a || !m || !(record = find_memory(a, m)) || !memory_matches(record, m) || !record->cpu_owned) return -1;
    record->free_attempted = 1; copy_bytes(&info, &record->info, sizeof(info));
    if (a->functions.shmem_free(&info)) { record->quarantined = 1; return -1; }
    zero_bytes(m->allocator_cookie, sizeof(m->allocator_cookie));
    m->cpu_address = (uint8_t *)0; m->capacity = 0; m->fd = -1; m->offset_mode = 0;
    zero_bytes(record, sizeof(*record)); return 0;
}
static int32_t cpu_sync(void *storage, struct shizuka_transport_memory *m, uint32_t begin)
{
    struct vendor_adapter *a = checked(storage);
    struct memory_record *record;
    uint64_t flags = begin ? 3u : 7u;
    if (!a || !m || begin > 1 || !(record = find_memory(a, m)) || !memory_matches(record, m) ||
        record->cpu_owned == begin) return -1;
    if (a->functions.raw_ioctl(a->functions.user, m->fd, DMA_BUF_IOCTL_SYNC, &flags)) {
        record->quarantined = 1; return -1;
    }
    record->cpu_owned = begin; return 0;
}
static int32_t transform(void *storage, uint32_t profile, uint64_t delta_us,
    uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
    uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity,
    struct shizuka_transport_transform_result *out)
{
    struct vendor_adapter *a = checked(storage);
    struct shz_encoded_result result;
    int rc;
    if (!a || !out) return -1;
    rc = shz_encoded_transform(data, data_bytes, data_capacity, metadata, metadata_bytes,
        metadata_capacity, delta_us, profile, &result);
    if (a->functions.transform_result)
        a->functions.transform_result(a->functions.user, rc, profile, delta_us, rc ? (const struct shz_encoded_result *)0 : &result);
    if (rc) return rc;
    out->data_bytes = result.data_bytes; out->metadata_bytes = result.metadata_bytes;
    out->timestamp_bits = result.timestamp_bits; out->wr_flags = result.wr_flags;
    out->wrappers_changed = result.changed_wrapper_count; return 0;
}
static uint64_t clock_ms(void *storage)
{
    struct vendor_adapter *a = checked(storage); return a ? a->functions.clock_ms(a->functions.user) : 0;
}
static int32_t validate_discard(void *storage, uint32_t profile,
    uint8_t *data, uint32_t data_bytes, uint32_t data_capacity,
    uint8_t *metadata, uint32_t metadata_bytes, uint32_t metadata_capacity)
{
    struct shz_encoded_result ignored;
    if (!checked(storage)) return -1;
    /* Delta zero validates/compacts private discarded data without changing
     * encoded TTP or publishing an applied/transform telemetry sample. */
    return shz_encoded_transform(data, data_bytes, data_capacity, metadata,
        metadata_bytes, metadata_capacity, 0, profile, &ignored);
}
static void notify(void *storage, uint32_t phase, int32_t error, uint32_t need_close)
{
    struct vendor_adapter *a = checked(storage);
    if (a && a->functions.notify) a->functions.notify(a->functions.user, phase, error, need_close);
}
size_t shizuka_vendor_adapter_bytes(void) { return sizeof(struct vendor_adapter); }
int32_t shizuka_vendor_adapter_init(void *storage, size_t bytes, const struct shizuka_vendor_functions *functions)
{
    struct vendor_adapter *a = storage;
    if (!a || ((uintptr_t)a & 7u) || bytes < sizeof(*a) || !functions ||
        functions->abi != SHIZUKA_VENDOR_ADAPTER_ABI || functions->bytes != sizeof(*functions) ||
        functions->abi_profile != SHIZUKA_VENDOR_DZIF_LAYOUT_V1 || functions->identity_verified != 1 ||
        !functions->gpr_register || !functions->gpr_deregister || !functions->gpr_alloc ||
        !functions->gpr_send || !functions->gpr_free || !functions->shmem_alloc ||
        !functions->shmem_free || !functions->callback_entry || !functions->raw_ioctl ||
        !functions->clock_ms || !functions->receive_error) return -1;
    if (a->magic == ADAPTER_MAGIC) return -1;
    zero_bytes(a, sizeof(*a)); a->magic = ADAPTER_MAGIC;
    copy_bytes(&a->functions, functions, sizeof(*functions)); return 0;
}
int32_t shizuka_vendor_make_host(void *storage, struct shizuka_transport_host *host)
{
    struct vendor_adapter *a = checked(storage);
    if (!a || !host) return -1;
    zero_bytes(host, sizeof(*host)); host->abi = SHIZUKA_TRANSPORT_ABI; host->bytes = sizeof(*host);
    host->user = a; host->register_port = register_port; host->unregister_port = unregister_port;
    host->send = send_packet; host->allocate = allocate_memory; host->free = release_memory;
    host->cpu_sync = cpu_sync; host->transform = transform; host->clock_ms = clock_ms; host->notify = notify;
    host->validate_discard = validate_discard;
    return 0;
}
uint32_t shizuka_vendor_receive(void *storage, struct shizuka_vendor_gpr_packet *packet)
{
    struct vendor_adapter *a = checked(storage);
    struct shizuka_transport_packet normalized;
    const uint8_t *b = (const uint8_t *)packet;
    uint32_t header, size, expected = 0;
    if (!a || !packet) return 2;
    __atomic_add_fetch(&a->inflight, 1u, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&a->packets_received, 1u, __ATOMIC_RELAXED);
    if (__atomic_load_n(&a->accepting, __ATOMIC_SEQ_CST)) {
        if (!__atomic_compare_exchange_n(&a->delivery_active, &expected, 1u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            receive_error(a, SHIZUKA_VENDOR_CONCURRENT_CALLBACK);
        } else {
            if (((uintptr_t)packet & 3u)) receive_error(a, SHIZUKA_VENDOR_INVALID_PACKET);
            else {
                header = get32(b); size = header >> 8;
                if ((header & 0xffu) != 0x60u || size < GPR_HEADER_BYTES ||
                    size > GPR_HEADER_BYTES + SHIZUKA_TRANSPORT_MAX_PACKET_PAYLOAD ||
                    b[4] != 3 || b[5] != 2 || (b[6] & 0xf0u) || b[7] ||
                    get32(b + 12) != SHIZUKA_TRANSPORT_CLIENT_PORT ||
                    !(get32(b + 8) == 1 || get32(b + 8) == SHIZUKA_TRANSPORT_RD_MIID || get32(b + 8) == SHIZUKA_TRANSPORT_WR_MIID))
                    receive_error(a, SHIZUKA_VENDOR_INVALID_PACKET);
                else {
                    zero_bytes(&normalized, sizeof(normalized));
                    normalized.src_domain = b[5]; normalized.dst_domain = b[4]; normalized.client_data = b[6];
                    normalized.src_port = get32(b + 8); normalized.dst_port = get32(b + 12);
                    normalized.token = get32(b + 16); normalized.opcode = get32(b + 20);
                    normalized.payload_bytes = size - GPR_HEADER_BYTES; normalized.payload = b + GPR_HEADER_BYTES;
                    if (a->receive(a->receive_context, &normalized)) receive_error(a, SHIZUKA_VENDOR_RECEIVER_REJECTED);
                    else __atomic_add_fetch(&a->packets_delivered, 1u, __ATOMIC_RELAXED);
                }
            }
            __atomic_store_n(&a->delivery_active, 0u, __ATOMIC_SEQ_CST);
        }
    }
    (void)free_packet(a, packet);
    __atomic_sub_fetch(&a->inflight, 1u, __ATOMIC_SEQ_CST);
    return 0; /* Packet consumed even if malformed or already detached. */
}
int32_t shizuka_vendor_get_status(void *storage, struct shizuka_vendor_status *status)
{
    struct vendor_adapter *a = checked(storage);
    uint32_t i;
    if (!a || !status || status->abi != SHIZUKA_VENDOR_ADAPTER_ABI || status->bytes != sizeof(*status)) return -1;
    zero_bytes(status, sizeof(*status)); status->abi = SHIZUKA_VENDOR_ADAPTER_ABI; status->bytes = sizeof(*status);
    status->accepting = __atomic_load_n(&a->accepting, __ATOMIC_SEQ_CST); status->registered = a->registered;
    status->inflight = __atomic_load_n(&a->inflight, __ATOMIC_SEQ_CST); status->retired = a->retired;
    status->packets_received = __atomic_load_n(&a->packets_received, __ATOMIC_RELAXED);
    status->packets_delivered = __atomic_load_n(&a->packets_delivered, __ATOMIC_RELAXED);
    status->packets_rejected = __atomic_load_n(&a->packets_rejected, __ATOMIC_RELAXED);
    status->packets_free_attempted = __atomic_load_n(&a->packets_free_attempted, __ATOMIC_RELAXED);
    status->packets_free_failed = __atomic_load_n(&a->packets_free_failed, __ATOMIC_RELAXED);
    status->sends_accepted = a->sends_accepted; status->sends_failed = a->sends_failed;
    for (i = 0; i < MEMORY_COUNT; ++i) if (a->memory[i].owner) {
        ++status->allocations_owned; if (a->memory[i].quarantined) ++status->allocations_quarantined;
    }
    return 0;
}
