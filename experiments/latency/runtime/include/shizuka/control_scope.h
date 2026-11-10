/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_CONTROL_SCOPE_H
#define SHIZUKA_CONTROL_SCOPE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SHIZUKA_CONTROL_TARGET_SG UINT32_C(0xb00000ab)
#define SHIZUKA_CONTROL_MAX_PACKET_BYTES 65536u
#define SHIZUKA_CONTROL_MAX_SUBGRAPHS 4096u

/* Inspect an owned, readable outgoing GPR packet before original GSL can free
 * it. capacity is the caller-known readable allocation size, not an unchecked
 * external packet_size. No allocation, lock, import, GPR call or OOB dereference.
 *
 * PREPARE/START/STOP/CLOSE only. Require APPS3 -> ADSP2/APM1, version0/header24,
 * inband APM header16, a complete aligned parameter stream, and at most one
 * APM1 / APM_PARAM_ID_SUB_GRAPH_LIST(08001005). That list must have an exact
 * count/size and unique nonzero IDs. Other well-formed parameters (including
 * CLOSE connection-only data) cannot prove the target SG closed.
 *
 *  1: target SG00ab present exactly once in the explicit list
 *  0: well-formed command without target (including connection-only CLOSE)
 * -1: malformed, unsupported, over bound, or OOB/unknown target scope
 *
 * An owned CLOSE with -1 must not be interpreted as a confirmed SG close or
 * permission to free memory; keep ownership quarantined until a proven normal
 * close or independent HAL/DSP termination. Sourceport/token ACK identity is a
 * separate caller responsibility and cannot replace this target proof.
 */
int shizuka_control_scope_target(const void *packet,size_t capacity);
/* OOB counterpart for a CPU view positively captured from the exact original
 * GSL allocator. Caller must verify live msg/packet/body identity, source port,
 * thread/epoch, map handle and ADSP address against that allocation registry
 * before this call; this function cannot establish allocator ownership itself.
 * Never pass the packet's physical/ADSP address as body. The view must remain
 * CPU-owned/readable until parsing finishes, before original send/free.
 * Require a header-only GPR packet + APM header with nonzero map handle and
 * payload_size <= body_capacity <= 64 KiB. Packet/body allocations cannot alias.
 * The same exact read-only parameter validation and 1/0/-1 semantics apply.
 */
int shizuka_control_scope_target_owned_body(const void *packet,size_t packet_capacity,
    const void *body,size_t body_capacity);
/* Passive observation only: same exact inband validation, but require normal
 * START and inspect downstream connproxy SGb000006c.1=explicitly present,
 * 0=valid absent,-1=unknown/malformed/OOB. Does not prove START ACK, graph/port
 * ownership, actual CP state or audio consumption. Existing target APIs above
 * remain the only target00ab scope authority. Never dereferences OOB memory. */
int shizuka_control_scope_start_peer_present(const void *packet,size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
