// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace shizuka::latency {

constexpr std::size_t kMaximumEncodedBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumMetadataBytes = 64 * 1024;
constexpr std::uint64_t kTtpQuantumUs = 8;
constexpr std::uint64_t kMaximumTtpUs = 65535 * kTtpQuantumUs;
constexpr const char* kReviewedProfile = "stock-r3-hq-44100";

class Rejected : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The graph owner must identify this format. Plausible encoded bytes are not
// evidence of the active codec, sample rate or encoder mode.
enum class Profile { StockR3Hq44100 };
Profile parseProfile(const std::string& name);

struct RDBuffer {
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> metadata;
};

struct WrapperChange {
    std::size_t offset;
    std::uint16_t before_units;
    std::uint16_t after_units;
    bool secondary;
};

struct WRBuffer {
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> metadata;
    // Keep the original timestamp bit pattern, including negative timestamps.
    std::uint64_t timestamp_bits;
    std::uint32_t flags;
    std::vector<WrapperChange> wrappers;
};

// Snapshot delta_us once for this complete RD buffer. No graph, file, property,
// decoder or DSP-image operation occurs here. Rejection never mutates input.
WRBuffer validateAndRewrite(const RDBuffer& input, std::uint64_t delta_us, Profile profile);
std::uint16_t applyDelta(std::uint16_t old_units, std::uint64_t delta_us);

enum class AddressMode { Physical, Offset };
struct Mapping {
    AddressMode mode;
    // Physical: accessible address/span. Offset: byte offset within one owned
    // APM region whose actual (4-KiB-aligned uint32) size is capacity.
    std::uint64_t address;
    std::uint32_t handle;
    std::uint64_t capacity;
};

// Serializes the 44-byte WR command body only. This does not validate ownership
// of existing APM mappings, synchronize caches or send a command.
std::array<std::uint8_t, 44> serializeWriteCommand(const WRBuffer& output,
                                               Mapping data, Mapping metadata);
std::string reportJson(const WRBuffer& output, std::uint64_t delta_us);

}  // namespace shizuka::latency
