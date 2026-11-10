// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "shizuka/encoded_ttp.hpp"
#include <filesystem>
#include <string>

namespace shizuka::latency {
// Immutable per-buffer snapshot of the canonical schema-2 module file. LL is
// retained for diagnostics, but does not authorize an unreviewed LL transform.
struct DelaySnapshot {
    std::uint32_t hq_ms = 0;
    std::uint32_t ll_ms = 0;
    std::uint64_t deltaFor(Profile profile) const;
};
DelaySnapshot parseDelayConfig(const std::string& text);
// Requires a private directory and regular 0600 file owned by this process's
// effective UID. Opens the directory/file without following final symlinks.
// A missing or malformed file rejects; it never reuses a stale positive delta.
DelaySnapshot readDelayConfig(const std::filesystem::path& path);
}  // namespace shizuka::latency
