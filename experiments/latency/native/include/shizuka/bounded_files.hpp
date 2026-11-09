// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shizuka::latency {
std::vector<std::uint8_t> readBoundedFile(const std::filesystem::path& path, std::size_t maximum);

// Owns a private sibling directory; publication atomically refuses an existing
// destination. A failed transform leaves no partial result directory.
class AtomicDirectory {
public:
    explicit AtomicDirectory(std::filesystem::path destination);
    ~AtomicDirectory();
    AtomicDirectory(const AtomicDirectory&) = delete;
    AtomicDirectory& operator=(const AtomicDirectory&) = delete;
    void write(const std::string& name, const std::vector<std::uint8_t>& bytes);
    void publish();
private:
    std::filesystem::path destination_, temporary_;
    bool published_ = false;
};
}  // namespace shizuka::latency
