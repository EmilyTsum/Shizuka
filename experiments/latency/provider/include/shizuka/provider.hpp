// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "shizuka/external_config.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <sys/types.h>

namespace shizuka::provider {
using Bytes16 = std::array<std::uint8_t, 16>;
using Record = std::array<std::uint8_t, 128>;
constexpr std::uint64_t kLeaseNs = 2000000000ULL;
constexpr std::uint64_t kRefreshNs = 500000000ULL;
struct Snapshot {
    Bytes16 boot{}, cookie{};
    std::uint64_t revision = 0, valid_until_ns = 0;
    std::uint32_t hq_us = 0, ll_us = 0, hal_pid = 0, owner_pid = 0;
    std::uint64_t hal_start_ticks = 0, owner_start_ticks = 0;
};
Bytes16 parseCookie(const std::string& text);
Bytes16 parseBootUuid(const std::string& text);
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);
Record encode(const Snapshot& value);
Snapshot decode(const Record& record);
std::uint64_t processStart(const std::string& stat, std::uint32_t expected_pid);
bool hasSysAdmin(const std::string& status);
std::uint64_t checkedExpiry(std::uint64_t now_ns);
void verifyRuntimeActivation(const Snapshot& lease, const std::string& activation,
    std::uint32_t saved_owner_pid, std::uint64_t saved_owner_start);

using StatusRecord = std::array<std::uint8_t, 256>;
struct RuntimeStatus {
    Bytes16 boot{}, cookie{};
    std::uint32_t hal_pid=0, stage=0;
    std::int32_t error=0;
    std::uint32_t lease_valid=0;
    std::uint64_t provider_revision=0, graph_epoch=0, active_delta_us=0, rd_frames=0, wr_frames=0;
    std::uint32_t owned_mappings=0, in_flight_buffers=0, before_ttp_us=0, after_ttp_us=0;
    std::uint32_t sample_rate=0, profile=0, target_sg=0, container=0, graph_source_port=0;
    std::uint32_t init_accepted=0, provider_owner_pid=0, registration_ready=0;
    std::uint64_t callbacks_seen=0, acdb_target_calls=0, gpr_send_count=0;
    std::uint32_t bootstrap_stage=0, worker_running=0, last_opcode=0, last_ack_status=0;
    std::uint32_t last_event_opcode=0, media_format_bytes=0;
    std::uint64_t status_generation=0, monotonic_now_ns=0;
    std::uint32_t open_ack=0, start_ack=0;
    std::uint64_t confirmed_rewritten_wrappers=0;
    // Raw status ABI1 offset232. Diagnostic relay never attests added TTP.
    std::uint32_t diagnostic_passthrough=0;
    // Raw status ABI1 offset236. This topology is never a factory capability.
    std::uint32_t experimental_raw_island=0;
};
struct StatusEvaluation {
    bool available=false, applied=false, pending_hq=false, pending_ll=false;
    std::uint64_t age_ns=0, lease_remaining_ns=0;
    std::string state="unavailable", reason="No current runtime observation is available.";
};
struct HealthEvaluation {
    bool healthy=false;
    std::string reason="No current runtime health observation is available.";
};
struct RecoveryEvaluation {
    bool safe_without_dsp_restart=false;
    std::string reason="Remote ownership is not yet confirmed clean.";
};
RuntimeStatus decodeStatus(const StatusRecord& record);
StatusEvaluation evaluateStatus(const RuntimeStatus& status, const Snapshot& lease, std::uint64_t now_ns);
HealthEvaluation evaluateHealth(const RuntimeStatus& status, const Snapshot& lease, std::uint64_t now_ns);
RecoveryEvaluation evaluateRecovery(const RuntimeStatus& status, const Snapshot& identity, std::uint64_t now_ns);
std::string recoveryJson(const RecoveryEvaluation& evaluation);
std::string healthJson(const HealthEvaluation& evaluation);
std::string statusJson(const RuntimeStatus* status, const Snapshot* lease,
                       const StatusEvaluation& evaluation);

// This policy is explicit so filesystem tests need neither root nor an audio
// group. The executable always supplies uid=0 and the system's audio gid.
class AtomicPublisher {
public:
    AtomicPublisher(const std::filesystem::path& directory, uid_t uid, gid_t gid);
    ~AtomicPublisher();
    AtomicPublisher(const AtomicPublisher&) = delete;
    AtomicPublisher& operator=(const AtomicPublisher&) = delete;
    void publish(const Record& record);
    bool removeOwned() noexcept;
private:
    std::filesystem::path path_;
    uid_t uid_;
    gid_t gid_;
    int directory_ = -1, lock_ = -1, published_fd_ = -1;
    dev_t directory_device_ = 0, published_device_ = 0;
    ino_t directory_inode_ = 0, published_inode_ = 0;
    std::uint64_t counter_ = 0;
    bool published_ = false;
    void verifyDirectory() const;
};
}  // namespace shizuka::provider
