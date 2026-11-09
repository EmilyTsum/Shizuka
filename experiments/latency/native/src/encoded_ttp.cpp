// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/encoded_ttp.hpp"

#include <limits>
#include <sstream>

namespace shizuka::latency {
namespace {
constexpr std::uint32_t kFrameInfo = 0x0a001052;
constexpr std::uint32_t kPcmFrameLength = 0x0a001037;
constexpr std::uint32_t kPcmMediaFormat = 0x0a001038;
constexpr std::uint32_t kZeroPadding = 0x0a001039;
constexpr std::uint32_t kTimestampValid = 0x80000000U;
constexpr std::uint32_t kEndOfFrame = 0x10;
constexpr std::array<std::size_t, 9> kPayloadBytes{348, 656, 140, 152, 560, 760, 960, 348, 980};

std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (static_cast<std::uint16_t>(bytes[at + 1]) << 8));
}
std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}
void put32(std::array<std::uint8_t, 44>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void validateDelta(std::uint64_t delta) {
    if (delta % kTtpQuantumUs != 0 || delta > kMaximumTtpUs)
        throw Rejected("delta_us must be an exact nonnegative 8-us increment within the wire range");
}
struct MetadataPart { std::size_t begin, end; bool consume; };
struct MetadataResult {
    std::vector<MetadataPart> parts;
    std::uint64_t timestamp = 0;
    std::uint32_t flags = 0;
};
MetadataResult parseMetadata(const RDBuffer& input) {
    const auto& md = input.metadata;
    MetadataResult result;
    std::size_t cursor = 0, frame_count = 0;
    while (cursor < md.size()) {
        if (md.size() - cursor < 24) throw Rejected("truncated metadata header");
        const auto id = u32(md, cursor), flags = u32(md, cursor + 4);
        const auto offset = u32(md, cursor + 8), low = u32(md, cursor + 12);
        const auto high = u32(md, cursor + 16), size = u32(md, cursor + 20);
        std::uint32_t expected = 0;
        switch (id) {
        case kFrameInfo: expected = 16; break;
        case kPcmFrameLength: case kZeroPadding: expected = 4; break;
        case kPcmMediaFormat: expected = 40; break;
        default: throw Rejected("metadata ID is outside the reviewed allowlist");
        }
        if ((flags & ~0x7fU) || (flags & 0x40U) || (flags & 0x0eU) || low || high)
            throw Rejected("metadata extensions, tracking, tokens or reserved flags are unreviewed");
        if (size % 4 || size != expected || size > md.size() - cursor - 24)
            throw Rejected("invalid or truncated naturally aligned metadata payload");
        if (offset > input.data.size()) throw Rejected("metadata offset exceeds encoded data");
        const auto end = cursor + 24 + size;
        if (id == kFrameInfo) {
            ++frame_count;
            if (offset || u32(md, cursor + 24) != input.data.size())
                throw Rejected("frame-info must cover the entire RD buffer at offset zero");
            const auto ts_flags = u32(md, cursor + 36);
            if (ts_flags & ~kTimestampValid) throw Rejected("reserved frame-info timestamp flags");
            result.timestamp = u32(md, cursor + 28) |
                               (static_cast<std::uint64_t>(u32(md, cursor + 32)) << 32);
            result.flags = kEndOfFrame | (ts_flags & kTimestampValid);
        }
        result.parts.push_back({cursor, end, id == kFrameInfo});
        cursor = end;
    }
    if (frame_count != 1) throw Rejected("exactly one encoder frame-info record is required");
    return result;
}
std::vector<WrapperChange> parseWrappers(const std::vector<std::uint8_t>& data,
                                       std::uint64_t delta) {
    std::vector<WrapperChange> result;
    std::size_t cursor = 0;
    while (cursor < data.size()) {
        if (data.size() - cursor < 8) throw Rejected("truncated R3 wrapper header");
        const auto old = u16(data, cursor);
        const auto interval = data[cursor + 2], type = data[cursor + 3];
        const auto flags = data[cursor + 4], marker = data[cursor + 7];
        if (type >= kPayloadBytes.size()) throw Rejected("unknown R3 packet type");
        if ((flags != 0 && flags != 0x80 && flags != 0xa0 && flags != 0xc0) ||
            data[cursor + 5] || data[cursor + 6] ||
            (marker != 0xad && marker != 0xae && marker != 0xaf) || !interval || interval % 4)
            throw Rejected("unsupported R3 wrapper header");
        const auto size = 8 + kPayloadBytes[type];
        if (size > data.size() - cursor) throw Rejected("truncated R3 packet");
        const bool secondary = flags == 0xc0;
        if ((secondary && old) || (!secondary && delta && !old))
            throw Rejected("unreviewed zero-primary or nonzero-secondary TTP semantics");
        result.push_back({cursor, old, secondary ? std::uint16_t{0} : applyDelta(old, delta), secondary});
        cursor += size;
    }
    return result;
}
void mapped(Mapping mapping, std::size_t bytes) {
    constexpr std::uint64_t end = std::uint64_t{1} << 36;
    if (!bytes || bytes > std::numeric_limits<std::uint32_t>::max() || !mapping.handle ||
        !mapping.capacity || mapping.address % 64)
        throw Rejected("expected an owned nonzero APM mapping with aligned bounded buffer");
    if (mapping.mode == AddressMode::Physical) {
        if (!mapping.address || mapping.address >= end || mapping.capacity > end - mapping.address ||
            bytes > mapping.capacity)
            throw Rejected("physical buffer exceeds its declared 36-bit mapped span");
    } else if (mapping.mode == AddressMode::Offset) {
        if (mapping.capacity > std::numeric_limits<std::uint32_t>::max() || mapping.capacity % 4096 ||
            mapping.address > mapping.capacity || bytes > mapping.capacity - mapping.address)
            throw Rejected("offset buffer exceeds its single 4-KiB-aligned APM mapped region");
    } else throw Rejected("unknown APM address mode");
}
std::string timestampString(std::uint64_t bits) {
    if (bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::to_string(bits);
    return "-" + std::to_string((~bits) + 1);
}
}  // namespace

Profile parseProfile(const std::string& name) {
    if (name != kReviewedProfile) throw Rejected("only stock-r3-hq-44100 is reviewed; LL/Lossless/other rates are unsupported");
    return Profile::StockR3Hq44100;
}

std::uint16_t applyDelta(std::uint16_t old, std::uint64_t delta) {
    validateDelta(delta);
    const std::uint64_t updated = static_cast<std::uint64_t>(old) + delta / kTtpQuantumUs;
    if (updated > std::numeric_limits<std::uint16_t>::max()) throw Rejected("serialized TTP would overflow");
    return static_cast<std::uint16_t>(updated);
}

WRBuffer validateAndRewrite(const RDBuffer& input, std::uint64_t delta, Profile profile) {
    if (profile != Profile::StockR3Hq44100) throw Rejected("unreviewed active profile");
    validateDelta(delta);
    if (input.data.empty() || input.data.size() > kMaximumEncodedBytes ||
        input.metadata.size() > kMaximumMetadataBytes)
        throw Rejected("RD buffer exceeds a bounded size or contains no data");
    const auto metadata = parseMetadata(input);
    auto wrappers = parseWrappers(input.data, delta);
    // Both parsers completed successfully. Only now produce a separate WR copy.
    WRBuffer output{input.data, {}, metadata.timestamp, metadata.flags, std::move(wrappers)};
    output.metadata.reserve(input.metadata.size());
    for (const auto& part : metadata.parts) if (!part.consume)
        output.metadata.insert(output.metadata.end(), input.metadata.begin() + part.begin,
                               input.metadata.begin() + part.end);
    for (const auto& wrapper : output.wrappers) {
        output.data[wrapper.offset] = static_cast<std::uint8_t>(wrapper.after_units);
        output.data[wrapper.offset + 1] = static_cast<std::uint8_t>(wrapper.after_units >> 8);
    }
    return output;
}

std::array<std::uint8_t, 44> serializeWriteCommand(const WRBuffer& output, Mapping data, Mapping metadata) {
    if (output.data.size() > kMaximumEncodedBytes || output.metadata.size() > kMaximumMetadataBytes ||
        (output.flags & ~(kTimestampValid | kEndOfFrame)) || !(output.flags & kEndOfFrame))
        throw Rejected("invalid WR buffer shape");
    mapped(data, output.data.size());
    if (data.mode != metadata.mode) throw Rejected("data and metadata address modes must match");
    if (output.metadata.empty()) {
        if (metadata.address || metadata.handle || metadata.capacity) throw Rejected("empty metadata requires zero mapping");
    } else mapped(metadata, output.metadata.size());
    std::array<std::uint8_t, 44> command{};
    put32(command, 0, static_cast<std::uint32_t>(data.address));
    put32(command, 4, static_cast<std::uint32_t>(data.address >> 32));
    put32(command, 8, data.handle);
    put32(command, 12, static_cast<std::uint32_t>(output.data.size()));
    put32(command, 16, static_cast<std::uint32_t>(output.timestamp_bits));
    put32(command, 20, static_cast<std::uint32_t>(output.timestamp_bits >> 32));
    put32(command, 24, output.flags);
    put32(command, 28, static_cast<std::uint32_t>(metadata.address));
    put32(command, 32, static_cast<std::uint32_t>(metadata.address >> 32));
    put32(command, 36, metadata.handle);
    put32(command, 40, static_cast<std::uint32_t>(output.metadata.size()));
    return command;
}

std::string reportJson(const WRBuffer& output, std::uint64_t delta) {
    std::size_t secondary = 0, changed = 0;
    for (const auto& w : output.wrappers) { secondary += w.secondary; changed += w.before_units != w.after_units; }
    std::ostringstream report;
    report << "{\n  \"schema\":1,\n  \"status\":\"offline transform only; no graph send or receiver validation\",\n"
           << "  \"active_profile\":\"" << kReviewedProfile << "\",\n  \"delta_us\":" << delta
           << ",\n  \"data_bytes\":" << output.data.size() << ",\n  \"metadata_bytes\":" << output.metadata.size()
           << ",\n  \"wrapper_count\":" << output.wrappers.size() << ",\n  \"changed_wrapper_count\":" << changed
           << ",\n  \"secondary_zero_sentinels_preserved\":" << secondary
           << ",\n  \"timestamp_us\":" << timestampString(output.timestamp_bits)
           << ",\n  \"timestamp_bits\":\"" << output.timestamp_bits << "\",\n  \"wr_flags\":" << output.flags
           << ",\n  \"encoded_body_bytes_unchanged\":true,\n  \"forwarded_metadata_bytes_unchanged\":true,\n"
           << "  \"graph_io_performed\":false,\n  \"signed_dsp_modified\":false,\n  \"wrappers\":[";
    for (std::size_t i = 0; i < output.wrappers.size(); ++i) {
        const auto& w = output.wrappers[i];
        if (i) report << ',';
        report << "{\"offset\":" << w.offset << ",\"before_us\":" << w.before_units * 8U
               << ",\"after_us\":" << w.after_units * 8U << ",\"secondary\":" << (w.secondary ? "true" : "false") << '}';
    }
    return report.str() + "]\n}\n";
}
}  // namespace shizuka::latency
