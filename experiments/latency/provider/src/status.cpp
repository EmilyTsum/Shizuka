// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/provider.hpp"
#include <algorithm>
#include <sstream>

namespace shizuka::provider {
namespace {
std::uint64_t get(const StatusRecord& record,unsigned at,unsigned size) {
    std::uint64_t out=0;
    for (unsigned i=0;i<size;++i) out|=std::uint64_t(record[at+i])<<(8*i);
    return out;
}
std::string jsonString(const std::string& text) {
    std::string out="\"";
    static const char hex[]="0123456789abcdef";
    for (unsigned char c:text) {
        if (c=='"' || c=='\\') { out+='\\'; out+=static_cast<char>(c); }
        else if (c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
bool bit(std::uint32_t value) { return value<=1; }
bool validDiagnostic(const RuntimeStatus& s) {
    return bit(s.diagnostic_passthrough) && bit(s.experimental_raw_island) && (!s.diagnostic_passthrough ||
        (!s.active_delta_us && !s.confirmed_rewritten_wrappers &&
         s.before_ttp_us==s.after_ttp_us && s.before_ttp_us<=524280));
}
}  // namespace
RuntimeStatus decodeStatus(const StatusRecord& record) {
    const std::uint8_t magic[8]={'S','H','Z','S','T','A','T','1'};
    if (!std::equal(magic,magic+8,record.begin()) || get(record,8,4)!=1 || get(record,12,4)!=256 ||
        get(record,252,4)!=crc32(record.data(),252) ||
        std::any_of(record.begin()+240,record.begin()+252,[](auto b){return b!=0;}))
        throw latency::Rejected("Runtime status has invalid header, reserved bytes or CRC.");
    RuntimeStatus v;
    std::copy(record.begin()+16,record.begin()+32,v.boot.begin());
    std::copy(record.begin()+32,record.begin()+48,v.cookie.begin());
    v.hal_pid=static_cast<std::uint32_t>(get(record,48,4));
    v.stage=static_cast<std::uint32_t>(get(record,52,4));
    const auto error=get(record,56,4);
    v.error=static_cast<std::int32_t>(error<0x80000000ULL?static_cast<std::int64_t>(error):static_cast<std::int64_t>(error)-0x100000000LL);
    v.lease_valid=static_cast<std::uint32_t>(get(record,60,4));
    v.provider_revision=get(record,64,8); v.graph_epoch=get(record,72,8);
    v.active_delta_us=get(record,80,8); v.rd_frames=get(record,88,8); v.wr_frames=get(record,96,8);
    v.owned_mappings=static_cast<std::uint32_t>(get(record,104,4));
    v.in_flight_buffers=static_cast<std::uint32_t>(get(record,108,4));
    v.before_ttp_us=static_cast<std::uint32_t>(get(record,112,4));
    v.after_ttp_us=static_cast<std::uint32_t>(get(record,116,4));
    v.sample_rate=static_cast<std::uint32_t>(get(record,120,4));
    v.profile=static_cast<std::uint32_t>(get(record,124,4));
    v.target_sg=static_cast<std::uint32_t>(get(record,128,4));
    v.container=static_cast<std::uint32_t>(get(record,132,4));
    v.graph_source_port=static_cast<std::uint32_t>(get(record,136,4));
    v.init_accepted=static_cast<std::uint32_t>(get(record,140,4));
    v.provider_owner_pid=static_cast<std::uint32_t>(get(record,144,4));
    v.registration_ready=static_cast<std::uint32_t>(get(record,148,4));
    v.callbacks_seen=get(record,152,8); v.acdb_target_calls=get(record,160,8); v.gpr_send_count=get(record,168,8);
    v.bootstrap_stage=static_cast<std::uint32_t>(get(record,176,4));
    v.worker_running=static_cast<std::uint32_t>(get(record,180,4));
    v.last_opcode=static_cast<std::uint32_t>(get(record,184,4));
    v.last_ack_status=static_cast<std::uint32_t>(get(record,188,4));
    v.last_event_opcode=static_cast<std::uint32_t>(get(record,192,4));
    v.media_format_bytes=static_cast<std::uint32_t>(get(record,196,4));
    v.status_generation=get(record,200,8); v.monotonic_now_ns=get(record,208,8);
    v.open_ack=static_cast<std::uint32_t>(get(record,216,4));
    v.start_ack=static_cast<std::uint32_t>(get(record,220,4));
    v.confirmed_rewritten_wrappers=get(record,224,8);
    v.diagnostic_passthrough=static_cast<std::uint32_t>(get(record,232,4));
    v.experimental_raw_island=static_cast<std::uint32_t>(get(record,236,4));
    if (!bit(v.lease_valid) || !bit(v.init_accepted) || !bit(v.registration_ready) ||
        !bit(v.worker_running) || !bit(v.open_ack) || !bit(v.start_ack))
        throw latency::Rejected("Runtime status has invalid boolean fields.");
    if (!validDiagnostic(v))
        throw latency::Rejected("Diagnostic passthrough status reports invalid mode, added TTP or rewrites.");
    return v;
}
StatusEvaluation evaluateStatus(const RuntimeStatus& s,const Snapshot& lease,std::uint64_t now_ns) {
    StatusEvaluation out;
    if (!validDiagnostic(s)) {
        out.reason="Runtime diagnostic mode or zero-addition evidence is invalid."; return out;
    }
    if (s.boot!=lease.boot || s.cookie!=lease.cookie || s.hal_pid!=lease.hal_pid ||
        s.provider_owner_pid!=lease.owner_pid) {
        out.reason="Runtime observation belongs to another boot, activation or process."; return out;
    }
    if (!s.status_generation || s.monotonic_now_ns>now_ns || now_ns-s.monotonic_now_ns>kLeaseNs) {
        out.reason="Runtime observation is missing, expired or from a future clock."; return out;
    }
    if (lease.valid_until_ns<=now_ns || lease.valid_until_ns-now_ns>kLeaseNs || !s.lease_valid ||
        !s.provider_revision || s.provider_revision>lease.revision) {
        out.reason="The current root provider lease is unavailable or expired."; return out;
    }
    out.available=true;
    out.age_ns=now_ns-s.monotonic_now_ns;
    out.lease_remaining_ns=lease.valid_until_ns-now_ns;
    // Phase 5 is SHIZUKA_TRANSPORT_ACTIVE. Live mappings and in-flight RD/WR
    // slots are normal ownership, not errors; pause/drain/uncertain phases are
    // excluded explicitly. Positive deltas need WR_DONE-confirmed rewrites.
    out.applied=!s.diagnostic_passthrough && s.stage==5 && s.graph_epoch && s.wr_frames && s.rd_frames>=s.wr_frames && !s.error &&
        s.worker_running && s.registration_ready && s.init_accepted &&
        s.open_ack && s.start_ack && s.profile==1 && s.sample_rate==44100 &&
        s.target_sg==0xb00000abU && s.container==0xe00000e6U && s.graph_source_port &&
        s.owned_mappings && s.active_delta_us<=150000 && s.active_delta_us%1000==0 &&
        (!s.experimental_raw_island || s.active_delta_us) &&
        (!s.active_delta_us || s.confirmed_rewritten_wrappers) &&
        s.before_ttp_us && s.before_ttp_us<=524280 && s.after_ttp_us<=524280 &&
        s.after_ttp_us>=s.before_ttp_us && s.after_ttp_us-s.before_ttp_us==s.active_delta_us;
    out.pending_hq=!out.applied || s.active_delta_us!=lease.hq_us;
    out.pending_ll=lease.ll_us!=0;
    if (s.error || s.stage==7 || s.stage>10 || !s.worker_running) {
        out.state="recovery_required";
        out.reason="The runtime reported an error or uncertain ownership. Owner recovery is required before another relay session.";
        return out;
    }
    if (s.diagnostic_passthrough) {
        out.state="diagnostic_passthrough";
        out.reason="No added TTP is active. This diagnostic relays encoded packets without changing their TTP.";
        return out;
    }
    if (s.experimental_raw_island) {
        out.state=out.applied?(out.pending_hq?"experimental_pending_next_session":"experimental_active"):"experimental_observing";
        out.reason=out.applied?
            "Experimental RAW-island RD/WR completions report the packet TTP change. This topology is not a qualified runtime capability; receiver latency remains unverified.":
            "The experimental RAW-island topology has not confirmed an active packet TTP change.";
        return out;
    }
    out.state=out.applied?(out.pending_hq?"pending_next_session":"active"):"observing";
    out.reason=out.applied?(out.pending_hq?
        "The current graph keeps its accepted delay. The new request is pending the next session.":
        "Matching RD/WR completions report the accepted packet TTP change. Receiver latency remains unverified."):
        "The current runtime has not confirmed an active packet TTP change.";
    return out;
}
HealthEvaluation evaluateHealth(const RuntimeStatus& s,const Snapshot& lease,std::uint64_t now_ns) {
    HealthEvaluation out;
    const auto identity=evaluateStatus(s,lease,now_ns);
    if (!identity.available) { out.reason=identity.reason; return out; }
    if (s.error) { out.reason="The runtime reported an error and requires owner recovery."; return out; }
    if (s.stage==7 || s.stage>10) {
        out.reason="The runtime ownership state is uncertain or unknown."; return out;
    }
    if (!s.worker_running) { out.reason="The runtime worker is no longer running."; return out; }
    out.healthy=true;
    out.reason="The current runtime lease, worker and ownership state are healthy.";
    return out;
}
std::string healthJson(const HealthEvaluation& e) {
    return std::string("{\"schema\":1,\"healthy\":")+(e.healthy?"true":"false")+
        ",\"reason\":"+jsonString(e.reason)+"}";
}
RecoveryEvaluation evaluateRecovery(const RuntimeStatus& s,const Snapshot& identity,std::uint64_t now_ns) {
    RecoveryEvaluation out;
    if (!validDiagnostic(s)) {
        out.reason="Runtime diagnostic mode or zero-addition evidence is invalid."; return out;
    }
    if (s.boot!=identity.boot || s.cookie!=identity.cookie || s.hal_pid!=identity.hal_pid) {
        out.reason="Recovery observation belongs to another boot, activation or HAL."; return out;
    }
    if (!s.status_generation || s.monotonic_now_ns>now_ns || now_ns-s.monotonic_now_ns>=kLeaseNs) {
        out.reason="Recovery observation is missing, stale or from a future clock."; return out;
    }
    if (s.lease_valid) {
        out.reason="The HAL has not yet observed root lease revocation."; return out;
    }
    if (s.error || s.owned_mappings || s.in_flight_buffers) return out;
    // Source is published at the first owned ADVERTISE before its caller can
    // continue. gpr_send_count includes unrelated normal GSL traffic.
    const bool untouched=s.stage<=2 && !s.graph_epoch && !s.graph_source_port &&
        !s.open_ack && !s.init_accepted && !s.start_ack && !s.rd_frames && !s.wr_frames &&
        !s.confirmed_rewritten_wrappers;
    // This legacy field stores the last acknowledged command opcode.
    const bool closed=s.stage==8 && s.graph_epoch && s.graph_source_port &&
        s.target_sg==0xb00000abU && s.container==0xe00000e6U && s.last_ack_status==0x01001004U;
    out.safe_without_dsp_restart=untouched || closed;
    if (out.safe_without_dsp_restart) out.reason=untouched?
        "The current owned HAL has not begun a remote relay graph.":
        "The current owned HAL confirmed target CLOSE and released all mappings and buffers.";
    return out;
}
std::string recoveryJson(const RecoveryEvaluation& e) {
    return std::string("{\"schema\":1,\"scope\":\"latency_recovery\",\"safe_without_dsp_restart\":")+
        (e.safe_without_dsp_restart?"true":"false")+",\"reason\":"+jsonString(e.reason)+"}";
}
std::string statusJson(const RuntimeStatus* s,const Snapshot* lease,const StatusEvaluation& e) {
    const bool diagnostic=s && s->diagnostic_passthrough==1;
    const bool experimental=s && s->experimental_raw_island==1;
    const bool applied=e.applied && !diagnostic;
    std::ostringstream out;
    out<<"{\"schema\":1,\"scope\":\"additional_ttp\",\"available\":"<<(e.available?"true":"false")
       <<",\"fresh\":"<<(e.available?"true":"false")<<",\"lease_valid\":"<<(e.available?"true":"false")
       <<",\"runtime_apply_supported\":"<<(applied && !experimental?"true":"false")<<",\"applied\":"<<(applied?"true":"false")
       <<",\"diagnostic_passthrough\":"<<(diagnostic?"true":"false")
       <<",\"experimental_raw_island\":"<<(experimental?"true":"false")
       <<",\"status\":"<<jsonString(e.state)<<",\"reason\":"<<jsonString(e.reason)
       <<",\"observation_age_ms\":"<<e.age_ns/1000000ULL<<",\"lease_remaining_ms\":"<<e.lease_remaining_ns/1000000ULL
       <<",\"pending_hq\":"<<(e.pending_hq?"true":"false")<<",\"pending_ll\":"<<(e.pending_ll?"true":"false");
    if (lease) out<<",\"requested_hq_us\":"<<lease->hq_us<<",\"requested_ll_us\":"<<lease->ll_us;
    if (s) {
        out<<",\"hal_pid\":"<<s->hal_pid<<",\"stage\":"<<s->stage<<",\"error\":"<<s->error
           <<",\"provider_revision\":\""<<s->provider_revision<<"\",\"graph_epoch\":\""<<s->graph_epoch
           <<"\",\"reported_delta_us\":"<<s->active_delta_us<<",\"active_delta_us\":";
        if(applied) out<<s->active_delta_us; else out<<"null";
        out<<",\"rd_frames\":"<<s->rd_frames<<",\"wr_frames\":"<<s->wr_frames
           <<",\"owned_mappings\":"<<s->owned_mappings<<",\"in_flight_buffers\":"<<s->in_flight_buffers
           <<",\"confirmed_rewritten_wrappers\":"<<s->confirmed_rewritten_wrappers
           <<",\"last_before_ttp_us\":"<<s->before_ttp_us<<",\"last_after_ttp_us\":"<<s->after_ttp_us
           <<",\"sample_rate_hz\":"<<s->sample_rate<<",\"profile\":"<<s->profile
           <<",\"target_sg\":"<<s->target_sg<<",\"container\":"<<s->container
           <<",\"graph_source_port\":"<<s->graph_source_port<<",\"init_accepted\":"<<s->init_accepted
           <<",\"provider_owner_pid\":"<<s->provider_owner_pid<<",\"registration_ready\":"<<s->registration_ready
           <<",\"callbacks_seen\":"<<s->callbacks_seen<<",\"acdb_target_calls\":"<<s->acdb_target_calls
           <<",\"gpr_send_count\":"<<s->gpr_send_count<<",\"bootstrap_stage\":"<<s->bootstrap_stage
           <<",\"worker_running\":"<<s->worker_running<<",\"last_opcode\":"<<s->last_opcode
           <<",\"last_ack_status\":"<<s->last_ack_status<<",\"last_event_opcode\":"<<s->last_event_opcode
           <<",\"media_format_bytes\":"<<s->media_format_bytes<<",\"status_generation\":\""<<s->status_generation
           <<"\",\"open_ack\":"<<s->open_ack<<",\"start_ack\":"<<s->start_ack;
    } else out<<",\"active_delta_us\":null";
    return out.str()+"}";
}
}  // namespace shizuka::provider
