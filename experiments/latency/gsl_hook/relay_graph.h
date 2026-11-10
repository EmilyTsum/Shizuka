/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKA_RELAY_GRAPH_H
#define SHIZUKA_RELAY_GRAPH_H
#include <stddef.h>
#include <stdint.h>

#define SHIZUKA_RELAY_SG UINT32_C(0xb00000ab)
#define SHIZUKA_RELAY_CONTAINER UINT32_C(0xe00000e6)
#define SHIZUKA_RELAY_ENCODER_CONTAINER UINT32_C(0xe00000e5)
#define SHIZUKA_RELAY_RD UINT32_C(0x43f7)
#define SHIZUKA_RELAY_WR UINT32_C(0x43fa)
#define SHIZUKA_RELAY_GRAPH_GROWTH 56U
#define SHIZUKA_RELAY_MAX_SPF (1024U*1024U)

/* Source-build experiment only. Production retains the reviewed same-container
 * graph unless the embedding builder explicitly enables this alternative.
 * The alternative uses only the two existing fresh SG-owned containers: E5
 * carries PCM/converter/encoder/RD; E6 carries the RAW-only WR source.
 */
#ifndef SHIZUKA_RELAY_RAW_ISLAND
#define SHIZUKA_RELAY_RAW_ISLAND 0
#endif
#if SHIZUKA_RELAY_RAW_ISLAND != 0 && SHIZUKA_RELAY_RAW_ISLAND != 1
#error "SHIZUKA_RELAY_RAW_ISLAND must be zero or one"
#endif

enum shizuka_graph_result {
    SHIZUKA_GRAPH_OK=0,
    SHIZUKA_GRAPH_INVALID=-1,
    SHIZUKA_GRAPH_CAPACITY=-2,
    SHIZUKA_GRAPH_COLLISION=-3,
    SHIZUKA_GRAPH_OVERLAP=-4
};

/* SPF body only: a sequence of apm_module_param_data_t (16B) + aligned payload.
 * Caller must establish graph/session/namespace/transport ownership separately.
 * This function performs no device operation and allocates no memory.
 */
int shizuka_relay_graph_measure(const uint8_t *input, size_t input_size,
                              uint32_t owned_client_port, size_t *output_size);

/* input and output must not overlap. The original input never changes. Output
 * is workspace and must be committed by the caller only on success. It replaces
 * the existing e6 parent=ffffffff field, never appends a duplicate property.
 * It reuses the two fresh SG-owned logger IDs as RD/WR: no new DSP instance ID.
 * Every original IMCL/other parameter and all unrelated graph bytes survive.
 */
int shizuka_relay_graph_transform(const uint8_t *input, size_t input_size,
                                uint32_t owned_client_port, uint8_t *output,
                                size_t capacity, size_t *output_size);
int shizuka_relay_graph_verify(const uint8_t *input, size_t input_size,
                             uint32_t owned_client_port);
#endif
