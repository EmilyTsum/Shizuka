// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/provider.hpp"
#include <algorithm>
#include <limits>
#include <sstream>

namespace shizuka::provider {
namespace {
unsigned nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    throw latency::Rejected("expected lowercase hexadecimal identity");
}
void put(Record& out, unsigned offset, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t get(const Record& in, unsigned offset, unsigned size) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(in[offset + i]) << (8 * i);
    return value;
}
std::uint64_t decimal(const std::string& text) {
    if (text.empty()) throw latency::Rejected("empty process field");
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10)
            throw latency::Rejected("invalid process field");
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
}
void validate(const Snapshot& v) {
    if (!v.revision || !v.valid_until_ns || !v.hal_pid || !v.owner_pid ||
        !v.hal_start_ticks || !v.owner_start_ticks || v.hq_us > 150000 || v.ll_us > 100000 ||
        v.hq_us % 1000 || v.ll_us % 1000 ||
        std::all_of(v.cookie.begin(), v.cookie.end(), [](auto b) { return b == 0; }) ||
        std::all_of(v.boot.begin(), v.boot.end(), [](auto b) { return b == 0; }))
        throw latency::Rejected("invalid provider snapshot");
}
}  // namespace
Bytes16 parseCookie(const std::string& text) {
    if (text.size() != 32) throw latency::Rejected("cookie requires 32 lowercase hex characters");
    Bytes16 result{};
    for (unsigned i = 0; i < 16; ++i) result[i] = static_cast<std::uint8_t>((nibble(text[2*i]) << 4) | nibble(text[2*i+1]));
    if (std::all_of(result.begin(), result.end(), [](auto b) { return b == 0; }))
        throw latency::Rejected("zero activation cookie is not allowed");
    return result;
}
Bytes16 parseBootUuid(const std::string& text) {
    std::string value = text;
    if (!value.empty() && value.back() == '\n') value.pop_back();
    if (value.size() != 36) throw latency::Rejected("invalid boot UUID length");
    std::string hex;
    for (unsigned i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') throw latency::Rejected("invalid boot UUID separator");
        } else { nibble(value[i]); hex.push_back(value[i]); }
    }
    return parseCookie(hex);
}
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t value = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        value ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return value ^ 0xffffffffU;
}
Record encode(const Snapshot& v) {
    validate(v);
    Record out{};
    const std::uint8_t magic[8] = {'S','H','Z','L','A','T','1',0};
    std::copy(magic, magic + 8, out.begin());
    put(out, 8, 1, 4); put(out, 12, out.size(), 4);
    std::copy(v.boot.begin(), v.boot.end(), out.begin()+16);
    std::copy(v.cookie.begin(), v.cookie.end(), out.begin()+32);
    put(out,48,v.revision,8); put(out,56,v.valid_until_ns,8);
    put(out,64,v.hq_us,4); put(out,68,v.ll_us,4);
    put(out,72,v.hal_pid,4); put(out,76,v.owner_pid,4);
    put(out,80,v.hal_start_ticks,8); put(out,88,v.owner_start_ticks,8);
    put(out,124,crc32(out.data(),124),4);
    return out;
}
Snapshot decode(const Record& in) {
    const std::uint8_t magic[8] = {'S','H','Z','L','A','T','1',0};
    if (!std::equal(magic,magic+8,in.begin()) || get(in,8,4)!=1 || get(in,12,4)!=128 ||
        get(in,124,4)!=crc32(in.data(),124) ||
        std::any_of(in.begin()+96,in.begin()+124,[](auto b){return b!=0;}))
        throw latency::Rejected("invalid provider record header, reserved bytes or CRC");
    Snapshot v;
    std::copy(in.begin()+16,in.begin()+32,v.boot.begin());
    std::copy(in.begin()+32,in.begin()+48,v.cookie.begin());
    v.revision=get(in,48,8); v.valid_until_ns=get(in,56,8);
    v.hq_us=static_cast<std::uint32_t>(get(in,64,4)); v.ll_us=static_cast<std::uint32_t>(get(in,68,4));
    v.hal_pid=static_cast<std::uint32_t>(get(in,72,4)); v.owner_pid=static_cast<std::uint32_t>(get(in,76,4));
    v.hal_start_ticks=get(in,80,8); v.owner_start_ticks=get(in,88,8);
    validate(v); return v;
}
std::uint64_t processStart(const std::string& stat, std::uint32_t expected_pid) {
    const auto split=stat.rfind(") ");
    const auto comm=stat.find(" (");
    if (comm==std::string::npos || split==std::string::npos || split<comm || decimal(stat.substr(0,comm))!=expected_pid)
        throw latency::Rejected("invalid process stat identity");
    std::istringstream fields(stat.substr(split+2));
    std::string field;
    for (unsigned i=0;i<=19;++i) {
        if (!(fields>>field)) throw latency::Rejected("truncated process stat");
        if (i==0 && (field=="Z" || field=="X" || field=="x")) throw latency::Rejected("process has exited");
    }
    const auto value=decimal(field);
    if (!value) throw latency::Rejected("zero process start identity");
    return value;
}
bool hasSysAdmin(const std::string& status) {
    std::istringstream lines(status); std::string line;
    while (std::getline(lines,line)) {
        if (line.rfind("CapEff:",0)!=0) continue;
        std::istringstream stream(line.substr(7)); std::string hex,extra;
        if (!(stream>>hex) || (stream>>extra) || hex.empty() || hex.size()>16) return false;
        std::uint64_t value=0;
        try { for (char c:hex) value=(value<<4)|nibble(c); } catch (...) { return false; }
        return (value & (std::uint64_t(1)<<21))!=0;
    }
    return false;
}
std::uint64_t checkedExpiry(std::uint64_t now_ns) {
    if (now_ns>std::numeric_limits<std::uint64_t>::max()-kLeaseNs)
        throw latency::Rejected("monotonic lease expiry overflow");
    return now_ns+kLeaseNs;
}
void verifyRuntimeActivation(const Snapshot& lease,const std::string& activation,
    std::uint32_t saved_owner_pid,std::uint64_t saved_owner_start) {
    if (saved_owner_pid!=lease.owner_pid || saved_owner_start!=lease.owner_start_ticks)
        throw latency::Rejected("The runtime lease publisher is not owned by this module session.");
    if (activation.size()>4096)
        throw latency::Rejected("The latency activation record is oversized.");
    std::istringstream values(activation);
    std::string boot,cookie,digest,mode,extra;
    if (!(values>>boot>>cookie>>digest>>mode) || (values>>extra))
        throw latency::Rejected("The latency activation record is incomplete or malformed.");
    if (parseBootUuid(boot)!=lease.boot || parseCookie(cookie)!=lease.cookie || digest.size()!=64 ||
        digest.find_first_not_of("0123456789abcdef")!=std::string::npos ||
        (mode!="observer" && mode!="relay"))
        throw latency::Rejected("The runtime lease belongs to a different latency activation.");
}
}  // namespace shizuka::provider
