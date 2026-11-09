// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/encoded_ttp.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>

using namespace shizuka::latency;
namespace {
std::size_t checks = 0;
void require(bool value) { ++checks; if (!value) throw std::runtime_error("native test failed"); }
template<class Function> void rejects(Function function) {
    ++checks;
    try { function(); } catch (const Rejected&) { return; }
    throw std::runtime_error("invalid transform was accepted");
}
void append32(std::vector<std::uint8_t>& bytes, std::uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(word >> (8 * i)));
}
void set32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(word >> (8 * i));
}
void record(std::vector<std::uint8_t>& bytes, std::uint32_t id, const std::vector<std::uint8_t>& payload,
            std::uint32_t flags = 0x21, std::uint32_t offset = 0) {
    for (const auto word : {id, flags, offset, 0U, 0U, static_cast<std::uint32_t>(payload.size())}) append32(bytes, word);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}
std::vector<std::uint8_t> packet(std::uint16_t ttp = 24625, std::uint8_t type = 1,
                               std::uint8_t role = 0xa0, std::uint8_t marker = 0xae) {
    constexpr std::size_t sizes[]{348, 656, 140, 152, 560, 760, 960, 348, 980};
    std::vector<std::uint8_t> result{static_cast<std::uint8_t>(ttp), static_cast<std::uint8_t>(ttp >> 8), 120, type, role, 0, 0, marker};
    for (std::size_t i = 0; i < sizes[type]; ++i) result.push_back(static_cast<std::uint8_t>(i * 17 + 3));
    return result;
}
RDBuffer fixture(std::vector<std::uint8_t> data, std::uint64_t timestamp = 0xfffffffedcba9877ULL) {
    std::vector<std::uint8_t> md;
    record(md, 0x0a001037, {0xa0, 5, 0, 0}, 0x21, 2);
    record(md, 0x0a001038, std::vector<std::uint8_t>(40, 0x53));
    record(md, 0x0a001039, {0, 0, 0, 0});
    std::vector<std::uint8_t> frame;
    append32(frame, static_cast<std::uint32_t>(data.size()));
    append32(frame, static_cast<std::uint32_t>(timestamp));
    append32(frame, static_cast<std::uint32_t>(timestamp >> 32));
    append32(frame, 0x80000000);
    record(md, 0x0a001052, frame, 0);
    return {std::move(data), std::move(md)};
}
void transparent(const RDBuffer& input, const WRBuffer& output) {
    require(input.data.size() == output.data.size());
    require(input.metadata.size() == output.metadata.size() + 40);
    require(std::equal(output.metadata.begin(), output.metadata.end(), input.metadata.begin()));
    std::vector<bool> timing(input.data.size(), false);
    for (const auto& w : output.wrappers) {
        timing[w.offset] = true; timing[w.offset + 1] = true;
        if (w.secondary) require(w.before_units == 0 && w.after_units == 0);
    }
    for (std::size_t i = 0; i < input.data.size(); ++i)
        if (!timing[i]) require(input.data[i] == output.data[i]);
}
void math() {
    for (const std::uint64_t delta : {0ULL, 50000ULL, 100000ULL, 150000ULL}) {
        for (std::uint32_t old = 0; old <= 65535; ++old) {
            const auto expected = old * 8ULL + delta;
            if (expected <= 524280) require(applyDelta(static_cast<std::uint16_t>(old), delta) * 8ULL == expected);
            else rejects([&] { applyDelta(static_cast<std::uint16_t>(old), delta); });
        }
    }
    for (const std::uint64_t bad : {1ULL, 7ULL, 9ULL, 524288ULL, 18446744073709551608ULL, 18446744073709551615ULL})
        rejects([&] { applyDelta(24625, bad); });
    require(applyDelta(0, 524280) == 65535);
    rejects([] { applyDelta(1, 524280); });
}
void formats() {
    for (std::uint8_t type = 0; type < 9; ++type) for (std::uint8_t role : {0, 0x80, 0xa0, 0xc0})
        for (std::uint8_t marker : {0xad, 0xae, 0xaf}) {
            const auto input = fixture(packet(role == 0xc0 ? 0 : 24625, type, role, marker));
            const auto output = validateAndRewrite(input, 150000, Profile::StockR3Hq44100);
            require(output.wrappers.front().after_units == (role == 0xc0 ? 0 : 43375));
            transparent(input, output);
        }
    auto pair = packet(); const auto secondary = packet(0, 1, 0xc0);
    pair.insert(pair.end(), secondary.begin(), secondary.end());
    const auto input = fixture(pair), before = input;
    const auto output = validateAndRewrite(input, 150000, parseProfile(kReviewedProfile));
    require(input.data == before.data && input.metadata == before.metadata);
    require(output.wrappers.size() == 2);
    require(output.wrappers[0].before_units * 8U == 197000);
    require(output.wrappers[0].after_units * 8U == 347000);
    require(output.wrappers[1].before_units == 0 && output.wrappers[1].after_units == 0);
    require(output.flags == 0x80000010 && output.timestamp_bits == 0xfffffffedcba9877ULL);
    require(output.data.size() == pair.size() && std::equal(pair.begin() + 2, pair.end(), output.data.begin() + 2));
    transparent(input, output);
    const auto unchanged = validateAndRewrite(input, 0, Profile::StockR3Hq44100);
    require(unchanged.data == pair);
    for (std::uint64_t timestamp : {0ULL, 0xffffffffULL, 0x100000000ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL}) {
        const auto current = validateAndRewrite(fixture(pair, timestamp), 0, Profile::StockR3Hq44100);
        require(current.timestamp_bits == timestamp);
        const auto body = serializeWriteCommand(current, {AddressMode::Physical, 0x123456780, 7, 4096}, {AddressMode::Physical, 0x234567880, 8, 4096});
        require(body.size() == 44 && body[0] == 0x80 && body[4] == 1 && body[8] == 7 && body[24] == 0x10 && body[27] == 0x80);
        for (unsigned i = 0; i < 8; ++i) require(body[16 + i] == static_cast<std::uint8_t>(timestamp >> (8 * i)));
        require(body[28] == 0x80 && body[32] == 2 && body[36] == 8);
    }
    for (const auto address : {0ULL, 65ULL, 0x1000000000ULL, 0xfffffffc0ULL})
        rejects([&] { serializeWriteCommand(output, {AddressMode::Physical, address, 7, 4096}, {AddressMode::Physical, 128, 1, 4096}); });
    rejects([&] { serializeWriteCommand(output, {AddressMode::Physical, 64, 0, 4096}, {AddressMode::Physical, 128, 1, 4096}); });
    rejects([&] { serializeWriteCommand(output, {AddressMode::Physical, 64, 1, 4096}, {AddressMode::Physical, 128, 0, 4096}); });
    const auto offset_body = serializeWriteCommand(output,
        {AddressMode::Offset, 0, 7, 4096}, {AddressMode::Offset, 64, 8, 4096});
    require(offset_body[0] == 0 && offset_body[4] == 0 && offset_body[28] == 64);
    for (const auto offset : {4032ULL, 4096ULL, 0x100000000ULL, 0xffffffffffffffffULL})
        rejects([&] { serializeWriteCommand(output, {AddressMode::Offset, offset, 7, 4096}, {AddressMode::Offset, 0, 8, 4096}); });
    rejects([&] { serializeWriteCommand(output, {AddressMode::Offset, 0, 7, 2048}, {AddressMode::Offset, 0, 8, 4096}); });
    rejects([&] { serializeWriteCommand(output, {AddressMode::Offset, 0, 7, 0x100000000ULL}, {AddressMode::Offset, 0, 8, 4096}); });
    rejects([&] { serializeWriteCommand(output, {AddressMode::Offset, 0, 7, 4096}, {AddressMode::Physical, 64, 8, 4096}); });
    rejects([&] { serializeWriteCommand(output, {static_cast<AddressMode>(7), 64, 7, 4096}, {AddressMode::Physical, 64, 8, 4096}); });
    auto without_bt = input; without_bt.metadata.erase(without_bt.metadata.begin(), without_bt.metadata.end() - 40);
    const auto empty = validateAndRewrite(without_bt, 0, Profile::StockR3Hq44100);
    require(empty.metadata.empty());
    require(serializeWriteCommand(empty, {AddressMode::Physical, 64, 1, 4096}, {AddressMode::Physical, 0, 0, 0})[40] == 0);
    rejects([&] { serializeWriteCommand(empty, {AddressMode::Physical, 64, 1, 4096}, {AddressMode::Physical, 128, 1, 4096}); });
}
void malformed() {
    const auto valid = fixture(packet());
    for (const auto& profile : {"legacy-hq", "r3-hq", "stock-r3-lossless-44100", "stock-r3-ll-44100", "stock-r3-hq-48000"})
        rejects([&] { parseProfile(profile); });
    rejects([&] { validateAndRewrite(valid, 0, static_cast<Profile>(5)); });
    for (const auto& change : std::vector<std::pair<std::size_t, std::uint8_t>>{{2,0},{2,30},{3,9},{4,0x7f},{5,1},{6,1},{7,0}}) {
        auto input = valid; input.data[change.first] = change.second;
        rejects([&] { validateAndRewrite(input, 150000, Profile::StockR3Hq44100); });
    }
    for (std::size_t cut = 0; cut < 8; ++cut) {
        auto input = valid; input.data.resize(cut);
        rejects([&] { validateAndRewrite(input, 150000, Profile::StockR3Hq44100); });
    }
    for (const auto& change : std::vector<std::pair<std::size_t, std::uint32_t>>{{0,1},{4,0x40},{4,0x80},{4,2},{12,1},{16,1},{8,999999},{20,3},{20,0xffffffff}}) {
        auto input = valid; set32(input.metadata, change.first, change.second);
        rejects([&] { validateAndRewrite(input, 150000, Profile::StockR3Hq44100); });
    }
    auto bad = valid; bad.metadata.back() |= 1;
    // Set a reserved timestamp flag, not an encoded payload byte.
    set32(bad.metadata, bad.metadata.size() - 4, 1);
    rejects([&] { validateAndRewrite(bad, 150000, Profile::StockR3Hq44100); });
    bad = valid; bad.metadata.insert(bad.metadata.end(), valid.metadata.end() - 40, valid.metadata.end());
    rejects([&] { validateAndRewrite(bad, 150000, Profile::StockR3Hq44100); });
    bad = valid; bad.metadata.pop_back();
    rejects([&] { validateAndRewrite(bad, 150000, Profile::StockR3Hq44100); });
    bad = fixture(packet(1, 1, 0xc0));
    rejects([&] { validateAndRewrite(bad, 150000, Profile::StockR3Hq44100); });
    bad = fixture(packet(0));
    rejects([&] { validateAndRewrite(bad, 150000, Profile::StockR3Hq44100); });
    auto pair = packet(); const auto overflow = packet(50000);
    pair.insert(pair.end(), overflow.begin(), overflow.end());
    bad = fixture(pair); const auto saved = bad;
    rejects([&] { validateAndRewrite(bad, 150000, Profile::StockR3Hq44100); });
    require(bad.data == saved.data && bad.metadata == saved.metadata);
    bad = valid; bad.data.resize(kMaximumEncodedBytes + 1);
    rejects([&] { validateAndRewrite(bad, 0, Profile::StockR3Hq44100); });
    bad = valid; bad.metadata.resize(kMaximumMetadataBytes + 1);
    rejects([&] { validateAndRewrite(bad, 0, Profile::StockR3Hq44100); });
}
void mutations() {
    std::mt19937 random(0x5a17);
    const auto base = fixture(packet());
    for (unsigned i = 0; i < 20000; ++i) {
        auto input = base;
        auto& bytes = i % 2 ? input.data : input.metadata;
        const auto at = random() % bytes.size(); bytes[at] ^= static_cast<std::uint8_t>(1U << (random() % 8));
        const auto original = input;
        try {
            const auto out = validateAndRewrite(input, 150000, Profile::StockR3Hq44100);
            transparent(input, out);
        } catch (const Rejected&) { }
        require(original.data == input.data && original.metadata == input.metadata);
    }
    for (unsigned i = 0; i < 10000; ++i) {
        RDBuffer input;
        input.data.resize(random() % 128); input.metadata.resize(random() % 128);
        for (auto& value : input.data) value = static_cast<std::uint8_t>(random());
        for (auto& value : input.metadata) value = static_cast<std::uint8_t>(random());
        rejects([&] { validateAndRewrite(input, 150000, Profile::StockR3Hq44100); });
    }
}
}  // namespace
int main() {
    try {
        math(); formats(); malformed(); mutations();
        std::cout << "native transform: " << checks << " checks; 262144 TTP arithmetic cases, 108 type/role/marker cases, 30000 malformed/mutation inputs; byte transparency, timestamp, overflow and mapping bounds passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
