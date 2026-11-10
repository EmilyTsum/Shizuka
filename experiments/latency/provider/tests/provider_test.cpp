// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/provider.hpp"
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace shizuka;
namespace {
unsigned checks=0;
void check(bool value,unsigned line=__builtin_LINE()) { ++checks; if (!value) throw std::runtime_error("provider assertion failed at check "+std::to_string(checks)+", line "+std::to_string(line)); }
template<class F> void rejects(F f) {
    bool failed=false; try { f(); } catch (const latency::Rejected&) { failed=true; }
    check(failed);
}
struct Temporary {
    std::filesystem::path path;
    Temporary() { char name[]="/tmp/shizuka-provider-XXXXXX"; const auto* p=mkdtemp(name); if (!p) throw std::runtime_error("mkdtemp"); path=p; }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
};
provider::Record read(const std::filesystem::path& file) {
    std::ifstream source(file,std::ios::binary); provider::Record bytes{};
    source.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    check(source.gcount()==static_cast<std::streamsize>(bytes.size())); return bytes;
}
provider::Snapshot sample() {
    provider::Snapshot value;
    value.boot=provider::parseBootUuid("00112233-4455-6677-8899-aabbccddeeff\n");
    value.cookie=provider::parseCookie("fedcba98765432100123456789abcdef");
    value.revision=0x1234567800000001ULL; value.valid_until_ns=0x1122334455667788ULL;
    value.hq_us=150000; value.ll_us=100000; value.hal_pid=20437; value.owner_pid=12345;
    value.hal_start_ticks=0x200000007ULL; value.owner_start_ticks=0x300000009ULL;
    return value;
}
provider::StatusRecord statusRecord(const provider::Snapshot& lease,std::uint64_t now) {
    provider::StatusRecord out{};
    const char magic[]="SHZSTAT1"; std::memcpy(out.data(),magic,8);
    const auto put=[&](unsigned at,std::uint64_t value,unsigned size=4) {
        for(unsigned i=0;i<size;++i) out[at+i]=static_cast<unsigned char>(value>>(8*i));
    };
    put(8,1); put(12,256);
    std::memcpy(out.data()+16,lease.boot.data(),16); std::memcpy(out.data()+32,lease.cookie.data(),16);
    put(48,lease.hal_pid); put(52,5); put(60,1); put(64,lease.revision,8); put(72,123,8);
    put(80,150000,8); put(88,10,8); put(96,9,8); put(104,2); put(108,2); put(112,197000); put(116,347000);
    put(120,44100); put(124,1); put(128,0xb00000ab); put(132,0xe00000e6); put(136,0x6001);
    put(140,1); put(144,lease.owner_pid); put(148,1); put(152,50,8); put(160,3,8); put(168,25,8);
    put(176,4); put(180,1); put(200,5,8); put(208,now-50000000ULL,8); put(216,1); put(220,1); put(224,7,8);
    put(252,provider::crc32(out.data(),252)); return out;
}
std::string process(const std::string& name,const std::string& state="S") {
    std::string text="123 ("+name+") "+state;
    for (unsigned i=1;i<19;++i) text+=" 0";
    return text+" 4294967303 0 0\n";
}
void write(const std::filesystem::path& file,const std::string& text,mode_t mode=0600) {
    std::ofstream stream(file); stream<<text; stream.close(); chmod(file.c_str(),mode);
}
}  // namespace
int main() {
    try {
        const std::uint8_t known[]={'1','2','3','4','5','6','7','8','9'};
        check(provider::crc32(known,sizeof(known))==0xcbf43926U);
        auto value=sample(); const auto record=provider::encode(value);
        check(std::memcmp(record.data(),"SHZLAT1\0",8)==0 && record[8]==1 && record[12]==128);
        check(record[16]==0 && record[17]==0x11 && record[31]==0xff && record[32]==0xfe);
        check(record[48]==1 && record[52]==0x78 && record[55]==0x12);
        check(record[56]==0x88 && record[63]==0x11);
        check(record[64]==0xf0 && record[65]==0x49 && record[66]==2);
        for (unsigned i=96;i<124;++i) check(record[i]==0);
        const auto decoded=provider::decode(record);
        check(decoded.boot==value.boot && decoded.cookie==value.cookie && decoded.revision==value.revision &&
            decoded.valid_until_ns==value.valid_until_ns && decoded.hq_us==150000 && decoded.ll_us==100000 &&
            decoded.hal_pid==20437 && decoded.owner_pid==12345 && decoded.hal_start_ticks==value.hal_start_ticks &&
            decoded.owner_start_ticks==value.owner_start_ticks);
        for (unsigned i=0;i<128;++i) { auto bad=record; bad[i]^=1; rejects([&]{ provider::decode(bad); }); }
        for (auto text:{"", "0", "00000000000000000000000000000000", "fedcba98765432100123456789abcdeF"})
            rejects([&]{ provider::parseCookie(text); });
        rejects([] { provider::parseBootUuid("00112233445566778899aabbccddeeff"); });
        check(provider::checkedExpiry(100)==2000000100ULL);
        rejects([]{ provider::checkedExpiry(UINT64_MAX); });
        check(provider::processStart(process("name ) with spaces"),123)==4294967303ULL);
        rejects([] { provider::processStart(process("x","Z"),123); });
        rejects([] { provider::processStart(process("x"),124); });
        rejects([] { provider::processStart("123 (x) S 0",123); });
        check(provider::hasSysAdmin("Name:\tx\nCapEff:\t0000000000200000\n"));
        for (auto status:{"", "CapEff:\t0000000000000000\n", "CapEff: xyz", "CapEff: 200000 extra"})
            check(!provider::hasSysAdmin(status));
        value.hq_us=150001; rejects([&]{ provider::encode(value); });
        value=sample(); value.ll_us=100001; rejects([&]{ provider::encode(value); });
        value=sample(); value.revision=0; rejects([&]{ provider::encode(value); });

        Temporary temporary;
        const auto directory=temporary.path/"output";
        {
            provider::AtomicPublisher publisher(directory,geteuid(),getegid());
            publisher.publish(record);
            check(read(directory/"provider.bin")==record);
            struct stat mode{}; check(lstat((directory/"provider.bin").c_str(),&mode)==0 &&
                (mode.st_mode&07777)==0640 && mode.st_uid==geteuid() && mode.st_gid==getegid());
            rejects([&]{ provider::AtomicPublisher duplicate(directory,geteuid(),getegid()); });
            const int old=open((directory/"provider.bin").c_str(),O_RDONLY);
            check(old>=0);
            value=sample(); value.revision++; value.hq_us=50000;
            publisher.publish(provider::encode(value));
            provider::Record old_bytes{}; check(::read(old,old_bytes.data(),old_bytes.size())==128); close(old);
            check(old_bytes==record && provider::decode(read(directory/"provider.bin")).hq_us==50000);
            check(publisher.removeOwned() && !std::filesystem::exists(directory/"provider.bin"));
            publisher.publish(record);
        }
        check(!std::filesystem::exists(directory/"provider.bin"));
        const auto linked=temporary.path/"linked";
        std::filesystem::create_directory_symlink(directory,linked);
        rejects([&]{ provider::AtomicPublisher publisher(linked,geteuid(),getegid()); });
        chmod(directory.c_str(),0755);
        rejects([&]{ provider::AtomicPublisher publisher(directory,geteuid(),getegid()); });
        chmod(directory.c_str(),0750);
        write(directory/"provider.bin","wrong size",0640);
        rejects([&]{ provider::AtomicPublisher publisher(directory,geteuid(),getegid()); });
        std::filesystem::remove(directory/"provider.bin");
        const auto elsewhere=temporary.path/"elsewhere"; write(elsewhere,"owned other file");
        std::filesystem::create_symlink(elsewhere,directory/"provider.bin");
        rejects([&]{ provider::AtomicPublisher publisher(directory,geteuid(),getegid()); });
        std::filesystem::remove(directory/"provider.bin");
        check(mkfifo((directory/"provider.bin").c_str(),0640)==0);
        rejects([&]{ provider::AtomicPublisher publisher(directory,geteuid(),getegid()); });
        std::filesystem::remove(directory/"provider.bin");
        {
            provider::AtomicPublisher publisher(directory,geteuid(),getegid()); publisher.publish(record);
            std::filesystem::remove(directory/"provider.bin");
            write(directory/"provider.bin",std::string(128,'x'),0640);
            check(!publisher.removeOwned());
        }
        check(std::filesystem::exists(directory/"provider.bin"));
        const auto config_dir=temporary.path/"config";
        std::filesystem::create_directory(config_dir); chmod(config_dir.c_str(),0700);
        const auto config=config_dir/"config.conf";
        write(config,"schema=2\nadaptive_hq_ttp_delta_ms=150\nadaptive_ll_ttp_delta_ms=100\n");
        const auto snapshot=latency::readDelayConfig(config);
        check(snapshot.hq_ms==150 && snapshot.ll_ms==100);
        write(config,"schema=2\nadaptive_hq_ttp_delta_ms=invalid\n");
        rejects([&]{ latency::readDelayConfig(config); });
        const auto clock=10000000000ULL;
        auto lease=sample(); lease.valid_until_ns=provider::checkedExpiry(clock);
        const std::string activation="00112233-4455-6677-8899-aabbccddeeff fedcba98765432100123456789abcdef "+
            std::string(64,'a')+" relay\n";
        provider::verifyRuntimeActivation(lease,activation,lease.owner_pid,lease.owner_start_ticks);
        check(true);
        rejects([&]{ provider::verifyRuntimeActivation(lease,activation,lease.owner_pid+1,lease.owner_start_ticks); });
        rejects([&]{ provider::verifyRuntimeActivation(lease,activation,lease.owner_pid,lease.owner_start_ticks+1); });
        rejects([&]{ provider::verifyRuntimeActivation(lease,activation+"extra",lease.owner_pid,lease.owner_start_ticks); });
        for (const auto& malformed:{std::string(""),std::string(4097,'a'),activation.substr(0,activation.find(' ')),
                std::string("00112233-4455-6677-8899-aabbccddeeff ")+std::string(32,'0')+" "+std::string(64,'a')+" relay"})
            rejects([&]{ provider::verifyRuntimeActivation(lease,malformed,lease.owner_pid,lease.owner_start_ticks); });
        auto other_activation=activation; other_activation[0]='1';
        rejects([&]{ provider::verifyRuntimeActivation(lease,other_activation,lease.owner_pid,lease.owner_start_ticks); });
        other_activation=activation; other_activation[37]='a';
        rejects([&]{ provider::verifyRuntimeActivation(lease,other_activation,lease.owner_pid,lease.owner_start_ticks); });
        other_activation=activation; other_activation[70]='A';
        rejects([&]{ provider::verifyRuntimeActivation(lease,other_activation,lease.owner_pid,lease.owner_start_ticks); });
        other_activation=activation; other_activation.replace(other_activation.find("relay"),5,"unknown");
        rejects([&]{ provider::verifyRuntimeActivation(lease,other_activation,lease.owner_pid,lease.owner_start_ticks); });
        const auto status_bytes=statusRecord(lease,clock);
        const auto status=provider::decodeStatus(status_bytes);
        auto remote=status;
        remote.stage=2; remote.graph_epoch=remote.graph_source_port=0;
        remote.open_ack=remote.init_accepted=remote.start_ack=0;
        remote.rd_frames=remote.wr_frames=remote.confirmed_rewritten_wrappers=0;
        remote.owned_mappings=remote.in_flight_buffers=0;
        remote.lease_valid=0; remote.provider_owner_pid=0; remote.gpr_send_count=500;
        auto expired_identity=lease; expired_identity.valid_until_ns=0;
        check(provider::evaluateRecovery(remote,expired_identity,clock).safe_without_dsp_restart);
        auto not_revoked=remote; not_revoked.lease_valid=1;
        check(!provider::evaluateRecovery(not_revoked,expired_identity,clock).safe_without_dsp_restart);
        for (unsigned field=0;field<15;++field) {
            auto bad=remote;
            switch(field) {
                case 0: bad.boot[0]^=1; break;
                case 1: bad.cookie[0]^=1; break;
                case 2: bad.hal_pid++; break;
                case 3: bad.status_generation=0; break;
                case 4: bad.monotonic_now_ns=clock+1; break;
                case 5: bad.monotonic_now_ns=clock-provider::kLeaseNs; break;
                case 6: bad.error=-332; break;
                case 7: bad.owned_mappings=1; break;
                case 8: bad.in_flight_buffers=1; break;
                case 9: bad.graph_source_port=0x2012; break;
                case 10: bad.graph_epoch=1; break;
                case 11: bad.open_ack=1; break;
                case 12: bad.init_accepted=1; break;
                case 13: bad.start_ack=1; break;
                case 14: bad.stage=7; break;
            }
            check(!provider::evaluateRecovery(bad,lease,clock).safe_without_dsp_restart);
        }
        auto late_close=status;
        late_close.lease_valid=0; late_close.owned_mappings=late_close.in_flight_buffers=0;
        late_close.stage=8; late_close.last_ack_status=0x01001004;
        check(provider::evaluateRecovery(late_close,expired_identity,clock).safe_without_dsp_restart);
        check(provider::recoveryJson(provider::evaluateRecovery(late_close,expired_identity,clock)).find(
            "\"safe_without_dsp_restart\":true")!=std::string::npos);
        for (unsigned field=0;field<9;++field) {
            auto bad=late_close;
            switch(field) {
                case 0: bad.stage=6; break;
                case 1: bad.last_ack_status=0x01001005; break;
                case 2: bad.error=-5; break;
                case 3: bad.owned_mappings=1; break;
                case 4: bad.in_flight_buffers=1; break;
                case 5: bad.graph_epoch=0; break;
                case 6: bad.graph_source_port=0; break;
                case 7: bad.target_sg++; break;
                case 8: bad.container++; break;
            }
            check(!provider::evaluateRecovery(bad,lease,clock).safe_without_dsp_restart);
        }
        auto evaluation=provider::evaluateStatus(status,lease,clock);
        check(provider::evaluateHealth(status,lease,clock).healthy);
        // Health is owner liveness, not evidence that a packet was changed.
        // Paused, ready, closed and ordinary drain can retain live ownership.
        for (std::uint32_t stage=0;stage<=10;++stage) {
            auto idle=status; idle.stage=stage; idle.rd_frames=idle.wr_frames=0;
            idle.graph_epoch=0; idle.profile=idle.sample_rate=0;
            idle.init_accepted=idle.open_ack=idle.start_ack=idle.registration_ready=0;
            idle.owned_mappings=idle.in_flight_buffers=0;
            check(provider::evaluateHealth(idle,lease,clock).healthy==(stage!=7));
            idle.owned_mappings=2; idle.in_flight_buffers=2;
            check(provider::evaluateHealth(idle,lease,clock).healthy==(stage!=7));
        }
        for (std::uint32_t stage:{11U,UINT32_MAX}) {
            auto unknown=status; unknown.stage=stage;
            check(!provider::evaluateHealth(unknown,lease,clock).healthy);
        }
        for (std::int32_t error:{-1,1,INT32_MIN,INT32_MAX}) {
            auto failed=status; failed.error=error;
            check(!provider::evaluateHealth(failed,lease,clock).healthy);
        }
        auto unhealthy=status; unhealthy.worker_running=0;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.lease_valid=0;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.provider_revision=0;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.provider_revision=lease.revision+1;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.boot[0]^=1;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.cookie[0]^=1;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.hal_pid++;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.provider_owner_pid++;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.status_generation=0;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.monotonic_now_ns=clock-provider::kLeaseNs-1;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        unhealthy=status; unhealthy.monotonic_now_ns=clock+1;
        check(!provider::evaluateHealth(unhealthy,lease,clock).healthy);
        auto invalid_lease=lease; invalid_lease.valid_until_ns=clock;
        check(!provider::evaluateHealth(status,invalid_lease,clock).healthy);
        invalid_lease=lease; invalid_lease.valid_until_ns=clock+provider::kLeaseNs+1;
        check(!provider::evaluateHealth(status,invalid_lease,clock).healthy);
        check(provider::healthJson(provider::evaluateHealth(status,lease,clock)).find("\"healthy\":true")!=std::string::npos);
        check(provider::healthJson(provider::HealthEvaluation{false,"bad \"health\"\n"}).find("bad \\\"health\\\"\\u000a")!=std::string::npos);
        check(evaluation.available && evaluation.applied && !evaluation.pending_hq && evaluation.pending_ll);
        check(evaluation.age_ns==50000000 && evaluation.lease_remaining_ns==provider::kLeaseNs);
        const auto status_json=provider::statusJson(&status,&lease,evaluation);
        check(status_json.find("\"applied\":true")!=std::string::npos &&
            status_json.find("\"active_delta_us\":150000")!=std::string::npos);
        for(unsigned i=0;i<256;++i) { auto corrupt=status_bytes; corrupt[i]^=1; rejects([&]{ provider::decodeStatus(corrupt); }); }
        auto newer_lease=lease; newer_lease.revision++; newer_lease.hq_us=50000;
        auto pending=provider::evaluateStatus(status,newer_lease,clock);
        check(pending.available && pending.applied && pending.pending_hq && pending.state=="pending_next_session");
        auto bad=status; bad.wr_frames=0;
        check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.graph_epoch=0; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.open_ack=0; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.start_ack=0; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.init_accepted=0; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.profile=4096; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.sample_rate=48000; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.worker_running=0; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.in_flight_buffers=2; check(provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.stage=6; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.stage=7; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.stage=9; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.stage=10; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.error=-5;
        auto failed=provider::evaluateStatus(bad,lease,clock);
        check(failed.available && !failed.applied && failed.pending_hq && failed.state=="recovery_required");
        bad=status; bad.stage=7;
        check(provider::evaluateStatus(bad,lease,clock).state=="recovery_required");
        bad=status; bad.worker_running=0;
        check(provider::evaluateStatus(bad,lease,clock).state=="recovery_required");
        bad=status; bad.confirmed_rewritten_wrappers=0; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.active_delta_us=0; bad.after_ttp_us=bad.before_ttp_us; bad.confirmed_rewritten_wrappers=0;
        check(provider::evaluateStatus(bad,lease,clock).applied);
        // Diagnostic mode retains the normal fresh lease/ownership checks but
        // can never attest added TTP, including completed zero-delta writes.
        auto diagnostic=bad; diagnostic.diagnostic_passthrough=1;
        const auto diagnostic_evaluation=provider::evaluateStatus(diagnostic,lease,clock);
        check(diagnostic_evaluation.available && !diagnostic_evaluation.applied &&
            diagnostic_evaluation.state=="diagnostic_passthrough");
        check(provider::evaluateHealth(diagnostic,lease,clock).healthy);
        const auto diagnostic_json=provider::statusJson(&diagnostic,&lease,diagnostic_evaluation);
        check(diagnostic_json.find("\"diagnostic_passthrough\":true")!=std::string::npos &&
            diagnostic_json.find("\"runtime_apply_supported\":false")!=std::string::npos &&
            diagnostic_json.find("\"applied\":false")!=std::string::npos &&
            diagnostic_json.find("\"reported_delta_us\":0")!=std::string::npos &&
            diagnostic_json.find("\"active_delta_us\":null")!=std::string::npos);
        auto forged_evaluation=diagnostic_evaluation; forged_evaluation.applied=true;
        check(provider::statusJson(&diagnostic,&lease,forged_evaluation).find("\"applied\":true")==std::string::npos);
        auto zero_lease=lease; zero_lease.hq_us=0;
        check(!provider::evaluateStatus(diagnostic,zero_lease,clock).applied);
        for(unsigned field=0;field<10;++field) {
            auto invalid=diagnostic;
            switch(field) {
                case 0: invalid.diagnostic_passthrough=2; break;
                case 1: invalid.active_delta_us=1; break;
                case 2: invalid.confirmed_rewritten_wrappers=1; break;
                case 3: invalid.after_ttp_us++; break;
                case 4: invalid.before_ttp_us=invalid.after_ttp_us=524281; break;
                case 5: invalid.cookie[0]^=1; break;
                case 6: invalid.boot[0]^=1; break;
                case 7: invalid.hal_pid++; break;
                case 8: invalid.provider_owner_pid++; break;
                case 9: invalid.monotonic_now_ns=clock-provider::kLeaseNs-1; break;
            }
            check(!provider::evaluateStatus(invalid,lease,clock).available);
            check(!provider::evaluateHealth(invalid,lease,clock).healthy);
        }
        auto diagnostic_error=diagnostic; diagnostic_error.error=-5;
        check(provider::evaluateStatus(diagnostic_error,lease,clock).state=="recovery_required");
        diagnostic_error=diagnostic; diagnostic_error.stage=7;
        check(!provider::evaluateHealth(diagnostic_error,lease,clock).healthy);
        auto diagnostic_record=status_bytes;
        const auto put_status=[](provider::StatusRecord& bytes,unsigned at,std::uint64_t value,unsigned width=4) {
            for(unsigned i=0;i<width;++i) bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
        };
        const auto reseal_status=[&](provider::StatusRecord& bytes) {
            put_status(bytes,252,provider::crc32(bytes.data(),252));
        };
        put_status(diagnostic_record,80,0,8); put_status(diagnostic_record,116,status.before_ttp_us);
        put_status(diagnostic_record,224,0,8); put_status(diagnostic_record,232,1); reseal_status(diagnostic_record);
        check(provider::decodeStatus(diagnostic_record).diagnostic_passthrough==1);
        for(unsigned field=0;field<5;++field) {
            auto invalid=diagnostic_record;
            switch(field) {
                case 0: put_status(invalid,232,2); break;
                case 1: put_status(invalid,80,1,8); break;
                case 2: put_status(invalid,224,1,8); break;
                case 3: put_status(invalid,116,status.before_ttp_us+1); break;
                case 4: put_status(invalid,112,524281); put_status(invalid,116,524281); break;
            }
            reseal_status(invalid); rejects([&]{ provider::decodeStatus(invalid); });
        }
        for(unsigned at=240;at<252;++at) {
            auto invalid=diagnostic_record; invalid[at]=1; reseal_status(invalid);
            rejects([&]{ provider::decodeStatus(invalid); });
        }
        check(status_json.find("\"diagnostic_passthrough\":false")!=std::string::npos);
        auto raw_island=status; raw_island.experimental_raw_island=1;
        auto raw_evaluation=provider::evaluateStatus(raw_island,lease,clock);
        check(raw_evaluation.available && raw_evaluation.applied && raw_evaluation.state=="experimental_active");
        check(provider::evaluateHealth(raw_island,lease,clock).healthy);
        const auto raw_json=provider::statusJson(&raw_island,&lease,raw_evaluation);
        check(raw_json.find("\"experimental_raw_island\":true")!=std::string::npos &&
            raw_json.find("\"applied\":true")!=std::string::npos &&
            raw_json.find("\"runtime_apply_supported\":false")!=std::string::npos &&
            raw_json.find("\"active_delta_us\":150000")!=std::string::npos);
        for(unsigned field=0;field<7;++field) {
            auto invalid=raw_island;
            switch(field) {
                case 0: invalid.confirmed_rewritten_wrappers=0; break;
                case 1: invalid.wr_frames=0; break;
                case 2: invalid.init_accepted=0; break;
                case 3: invalid.sample_rate=48000; break;
                case 4: invalid.after_ttp_us--; break;
                case 5: invalid.container=0xe00000e5; break;
                case 6: invalid.active_delta_us=0; invalid.after_ttp_us=invalid.before_ttp_us;
                        invalid.confirmed_rewritten_wrappers=0; break;
            }
            check(!provider::evaluateStatus(invalid,lease,clock).applied);
        }
        raw_island.diagnostic_passthrough=1; raw_island.active_delta_us=0;
        raw_island.after_ttp_us=raw_island.before_ttp_us; raw_island.confirmed_rewritten_wrappers=0;
        check(!provider::evaluateStatus(raw_island,lease,clock).applied);
        auto raw_record=status_bytes; put_status(raw_record,236,1); reseal_status(raw_record);
        check(provider::decodeStatus(raw_record).experimental_raw_island==1);
        put_status(raw_record,232,1); put_status(raw_record,80,0,8);
        put_status(raw_record,116,status.before_ttp_us); put_status(raw_record,224,0,8); reseal_status(raw_record);
        check(!provider::evaluateStatus(provider::decodeStatus(raw_record),lease,clock).applied);
        put_status(raw_record,236,2); reseal_status(raw_record);
        rejects([&]{ provider::decodeStatus(raw_record); });
        raw_island.experimental_raw_island=2;
        check(!provider::evaluateStatus(raw_island,lease,clock).available);
        check(!provider::evaluateHealth(raw_island,lease,clock).healthy);
        check(status_json.find("\"experimental_raw_island\":false")!=std::string::npos);
        bad=status; bad.after_ttp_us--; check(!provider::evaluateStatus(bad,lease,clock).applied);
        bad=status; bad.monotonic_now_ns=clock+1; check(!provider::evaluateStatus(bad,lease,clock).available);
        bad=status; bad.monotonic_now_ns=clock-provider::kLeaseNs-1; check(!provider::evaluateStatus(bad,lease,clock).available);
        bad=status; bad.cookie[0]^=1; check(!provider::evaluateStatus(bad,lease,clock).available);
        bad=status; bad.hal_pid++; check(!provider::evaluateStatus(bad,lease,clock).available);
        bad=status; bad.provider_owner_pid++; check(!provider::evaluateStatus(bad,lease,clock).available);
        bad=status; bad.lease_valid=0; check(!provider::evaluateStatus(bad,lease,clock).available);
        auto expired=lease; expired.valid_until_ns=clock; check(!provider::evaluateStatus(status,expired,clock).available);
        provider::StatusEvaluation missing; missing.reason="bad \"record\"\n";
        check(provider::statusJson(nullptr,nullptr,missing).find("bad \\\"record\\\"\\u000a")!=std::string::npos);
        std::cout<<checks<<" provider checks passed; file/record tests only, no live HAL transport\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
