// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace s25::ll {
// Classic aptX Low Latency, separate from Adaptive LOW_LATENCY.
constexpr int kPublicIndex = 13;
constexpr uint64_t kPublicId = 0x02000affULL;
constexpr uint64_t kDescriptor = 0x5332354c00000000ULL;
constexpr size_t kMaxCie = 20;
struct Capabilities {
    uint32_t vendor;
    uint8_t rates;
    bool extended, backchannel;
    uint16_t target, initial, working;
    uint8_t sra_max, sra_average;
};
struct Request {
    uint32_t rate;
    uint8_t bits;
    bool backchannel;
    size_t peer_mtu;
};
struct Plan {
    uint8_t wire[kMaxCie];
    size_t wire_size;
    uint8_t aptx[10]; // Local normal aptX CIE only; never send this in place of LL.
    uint32_t rate;
    uint8_t bits, channels;
    uint16_t frames_per_packet, packet_bytes; // 4 PCM samples/channel -> 4 encoded bytes.
};
enum class Error {
    Ok,
    Malformed,
    Unsupported,
    NoPeer,
    BackendUnavailable,
    BackendFailure,
    WrongPeer,
    PacketTooLarge
};
Error parse(const uint8_t *cie, size_t size, Capabilities *result);
Error negotiate(const uint8_t *cie, size_t size, const Request &request, Plan *result);
// The normal aptX coding frame is unchanged. LL sends raw aptX, without an RTP header.
// A transport backend MUST enforce packet_bytes and pacing; changing the UI/vendor ID is
// insufficient.
struct Backend {
    void *context;
    bool (*configure)(void *, const Plan &);
    // One callback per whole aptX frame: 4 interleaved stereo PCM16 samples (16 bytes).
    bool (*encode_frame)(void *, const uint8_t *pcm16, uint8_t *out4);
    bool (*send_packet)(void *, const uint8_t *encoded, size_t bytes, uint32_t pcm_frames);
    void (*stop)(void *);
};
class Session {
  public:
    Error discover(const uint8_t address[6], const uint8_t *cie, size_t size);
    Error bind(const Backend &backend);
    Error begin(const uint8_t address[6], const Request &request);
    Error encode(const uint8_t *pcm, size_t bytes);
    Error flush();
    void stop();
    void reset();
    bool available(const uint8_t address[6]) const;
    bool active() const { return active_; }
    const Plan &plan() const { return plan_; }

  private:
    uint8_t address_[6]{}, cie_[kMaxCie]{}, packet_[360]{};
    size_t cie_size_ = 0, packet_size_ = 0;
    Backend backend_{};
    Plan plan_{};
    bool active_ = false;
};
// Optional host-encoder session. Samsung live offload is wired separately in aptx_ll_native.cpp.
Session &android_session();
} // namespace s25::ll
