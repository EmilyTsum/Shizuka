// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/external_config.hpp"
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace shizuka::latency {
namespace {
constexpr std::size_t kConfigLimit = 16 * 1024;
std::string trim(const std::string& value) {
    auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}
std::uint32_t integer(const std::string& text, std::uint32_t maximum) {
    if (text.empty() || (text.size() > 1 && text[0] == '0'))
        throw Rejected("delay must be a canonical unsigned decimal integer");
    std::uint32_t number = 0;
    for (auto c : text) {
        if (c < '0' || c > '9') throw Rejected("delay must be an unsigned decimal integer");
        auto digit = static_cast<unsigned>(c - '0');
        if (number > maximum / 10 || (number == maximum / 10 && digit > maximum % 10))
            throw Rejected("delay exceeds the reviewed module control range");
        number = number * 10 + digit;
    }
    return number;
}
struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) { if (fd < 0) throw Rejected("cannot open private delay configuration"); }
    ~Descriptor() { close(fd); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
void privateOwner(int fd, bool directory) {
    struct stat value{};
    if (fstat(fd, &value) || value.st_uid != geteuid() ||
        (directory ? !S_ISDIR(value.st_mode) : !S_ISREG(value.st_mode)) ||
        (value.st_mode & 07777) != (directory ? 0700 : 0600))
        throw Rejected("delay configuration requires private owner-only directory/file");
}
bool sameModificationTimes(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
    return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
        before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
        before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
        before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}
}  // namespace

DelaySnapshot parseDelayConfig(const std::string& text) {
    if (text.size() > kConfigLimit || text.find('\0') != std::string::npos)
        throw Rejected("invalid or oversized delay configuration");
    const std::set<std::string> allowed{"schema", "adaptive_hq_ttp_delta_ms", "adaptive_ll_ttp_delta_ms",
        "classic_qhs_host_aware", "aptx_voice_enabled", "adaptive_priority", "aptx_hd_priority"};
    std::map<std::string, std::string> values;
    for (std::size_t cursor = 0; cursor < text.size();) {
        auto end = text.find_first_of("\r\n", cursor);
        if (end == std::string::npos) end = text.size();
        auto line = trim(text.substr(cursor, end - cursor));
        cursor = end + 1;
        if (line.empty() || line[0] == '#') continue;
        auto separator = line.find('=');
        if (separator == std::string::npos) throw Rejected("expected configuration key=value");
        auto key = trim(line.substr(0, separator));
        auto value = trim(line.substr(separator + 1));
        if (!allowed.count(key) || !values.emplace(key, value).second)
            throw Rejected("unknown, noncanonical or duplicate configuration key");
    }
    if (!values.count("schema") || values.at("schema") != "2")
        throw Rejected("canonical schema-2 module configuration is required");
    for (const auto& key : {"classic_qhs_host_aware", "aptx_voice_enabled"}) {
        if (!values.count(key)) continue;
        const auto& value = values.at(key);
        if (value != "true" && value != "false" &&
            !(std::string(key) == "aptx_voice_enabled" && value == "system"))
            throw Rejected("invalid module configuration choice");
    }
    for (const auto& key : {"adaptive_priority", "aptx_hd_priority"}) {
        if (!values.count(key) || values.at(key) == "system") continue;
        const auto& value = values.at(key);
        if (value.empty() || value.size() > 6 || value.find_first_not_of("0123456789") != std::string::npos)
            throw Rejected("invalid module priority choice");
    }
    DelaySnapshot snapshot;
    if (values.count("adaptive_hq_ttp_delta_ms")) snapshot.hq_ms = integer(values.at("adaptive_hq_ttp_delta_ms"), 150);
    if (values.count("adaptive_ll_ttp_delta_ms")) snapshot.ll_ms = integer(values.at("adaptive_ll_ttp_delta_ms"), 100);
    // The remaining module controls are validated, never executed or applied.
    return snapshot;
}

std::uint64_t DelaySnapshot::deltaFor(Profile profile) const {
    if (profile != Profile::StockR3Hq44100) throw Rejected("unreviewed configuration playback context");
    if (hq_ms > 150 || ll_ms > 100) throw Rejected("invalid delay snapshot");
    return static_cast<std::uint64_t>(hq_ms) * 1000;
}

DelaySnapshot readDelayConfig(const std::filesystem::path& path) {
    if (path.filename().empty() || path.filename() == "." || path.filename() == "..")
        throw Rejected("invalid configuration filename");
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    Descriptor directory(open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    privateOwner(directory.fd, true);
    Descriptor file(openat(directory.fd, path.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    privateOwner(file.fd, false);
    struct stat before{}, after{};
    if (fstat(file.fd, &before) || before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > kConfigLimit)
        throw Rejected("oversized configuration file");
    std::string text;
    char chunk[1024];
    while (true) {
        const auto size = read(file.fd, chunk, sizeof(chunk));
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) throw Rejected("configuration read failed");
        if (!size) break;
        text.append(chunk, static_cast<std::size_t>(size));
        if (text.size() > kConfigLimit) throw Rejected("oversized configuration file");
    }
    privateOwner(file.fd, false);
    if (fstat(file.fd, &after) || before.st_size != after.st_size ||
        text.size() != static_cast<std::uintmax_t>(before.st_size) ||
        !sameModificationTimes(before, after))
        throw Rejected("configuration changed during read; retry next owned buffer");
    return parseDelayConfig(text);
}
}  // namespace shizuka::latency
