// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/provider.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <grp.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
using namespace shizuka;
volatile std::sig_atomic_t stopping=0;
void stop(int) { stopping=1; }
void require(bool value,const char* why) { if (!value) throw latency::Rejected(why); }
std::string bounded(int fd,std::size_t limit) {
    std::string out; char chunk[1024];
    while (true) {
        const auto size=read(fd,chunk,sizeof(chunk));
        if (size<0 && errno==EINTR) continue;
        require(size>=0,"cannot read session identity");
        if (!size) return out;
        out.append(chunk,static_cast<std::size_t>(size));
        require(out.size()<=limit,"oversized session identity");
    }
}
std::string readPath(const std::string& path,std::size_t limit=16384) {
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    require(fd>=0,"cannot open session identity");
    try { const auto out=bounded(fd,limit); close(fd); return out; }
    catch (...) { close(fd); throw; }
}
std::string readOwned(int directory,const char* name,std::size_t limit=4096,mode_t required_mode=0) {
    const int fd=openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    require(fd>=0,"module session record is unavailable");
    try {
        struct stat value{};
        require(fstat(fd,&value)==0 && S_ISREG(value.st_mode) && value.st_uid==0 &&
            (value.st_mode&0022)==0 && (!required_mode ||
            ((value.st_mode&07777)==required_mode && value.st_nlink==1)),"unsafe module session record");
        const auto out=bounded(fd,limit); close(fd); return out;
    } catch (...) { close(fd); throw; }
}
std::uint64_t decimal(std::string text) {
    if (!text.empty() && text.back()=='\n') text.pop_back();
    require(!text.empty() && !(text.size()>1 && text[0]=='0'),"invalid canonical process identity");
    std::uint64_t value=0;
    for (auto c:text) {
        require(c>='0' && c<='9',"invalid process identity");
        const auto digit=static_cast<unsigned>(c-'0');
        require(value<=(UINT64_MAX-digit)/10,"process identity overflow");
        value=value*10+digit;
    }
    return value;
}
std::uint64_t start(std::uint32_t pid) {
    return provider::processStart(readPath("/proc/"+std::to_string(pid)+"/stat"),pid);
}
std::uint64_t now() {
    struct timespec value{};
    require(clock_gettime(CLOCK_MONOTONIC,&value)==0 && value.tv_sec>=0,"monotonic clock unavailable");
    require(static_cast<std::uint64_t>(value.tv_sec)<=(UINT64_MAX-999999999ULL)/1000000000ULL,
            "monotonic clock overflow");
    return static_cast<std::uint64_t>(value.tv_sec)*1000000000ULL+static_cast<unsigned long>(value.tv_nsec);
}
bool same(const struct stat& a,const struct stat& b) { return a.st_dev==b.st_dev && a.st_ino==b.st_ino; }
class ModuleSession {
public:
    ModuleSession(const std::filesystem::path& path,std::uint32_t hal,bool recovery=false):path_(path),hal_(hal),recovery_(recovery) {
        require(path_.is_absolute() && path_==path_.lexically_normal() &&
            std::filesystem::canonical(path_)==path_ &&
            path_.parent_path()=="/data/adb/modules" && path_.filename()=="s25_aptx_adaptive",
            "module directory must be the canonical installed Shizuka module");
        module_=open(path_.c_str(),O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
        require(module_>=0,"module directory unavailable");
        try {
            require(fstat(module_,&module_identity_)==0 && module_identity_.st_uid==0 &&
                (module_identity_.st_mode&0022)==0,"unsafe module directory");
            state_=openat(module_,"state",O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
            require(state_>=0 && fstat(state_,&state_identity_)==0 && state_identity_.st_uid==0 &&
                (state_identity_.st_mode&07777)==0700,"unsafe module state directory");
            const auto properties=readOwned(module_,"module.prop");
            std::istringstream lines(properties); std::string line; unsigned matches=0;
            while (std::getline(lines,line)) if (line=="id=s25_aptx_adaptive") ++matches;
            require(matches==1,"module ID does not match Shizuka");
            require(fstatat(state_,"active",&active_identity_,AT_SYMLINK_NOFOLLOW)==0,
                    "module is not active");
            hal_start_=start(hal_);
            owner_=static_cast<std::uint32_t>(getpid()); owner_start_=start(owner_);
            boot_=provider::parseBootUuid(readPath("/proc/sys/kernel/random/boot_id",64));
            check();
        } catch (...) { if (state_>=0) close(state_); close(module_); throw; }
    }
    ~ModuleSession() { if (state_>=0) close(state_); if (module_>=0) close(module_); }
    void check() const {
        require(geteuid()==0 && provider::hasSysAdmin(readPath("/proc/self/status")),
                "temporary root or CAP_SYS_ADMIN is unavailable");
        struct stat current{};
        require(lstat(path_.c_str(),&current)==0 && S_ISDIR(current.st_mode) && same(current,module_identity_) &&
            current.st_uid==0 && (current.st_mode&0022)==0,"module directory was replaced");
        for (const auto* marker:{"disable","remove"}) {
            errno=0; const auto found=fstatat(module_,marker,&current,AT_SYMLINK_NOFOLLOW);
            if (!recovery_) require(found<0 && errno==ENOENT,"module is disabled, removed or unavailable");
        }
        require(fstatat(module_,"state",&current,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(current.st_mode) &&
            same(current,state_identity_) && current.st_uid==0 && (current.st_mode&07777)==0700,
            "module state directory was replaced");
        require(fstatat(state_,"active",&current,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(current.st_mode) &&
            current.st_uid==0 && (current.st_mode&0022)==0 && same(current,active_identity_),
            "module activation ended or changed");
        require(decimal(readOwned(state_,"hal.pid"))==hal_ &&
            decimal(readOwned(state_,"hal.start"))==hal_start_,"module HAL ownership does not match");
        require(start(hal_)==hal_start_ && start(owner_)==owner_start_,"HAL or publisher process changed");
        require(provider::parseBootUuid(readPath("/proc/sys/kernel/random/boot_id",64))==boot_,"boot session changed");
    }
    provider::Snapshot snapshot(const provider::Bytes16& cookie,std::uint64_t revision,
                                const latency::DelaySnapshot& config) const {
        provider::Snapshot result;
        result.boot=boot_; result.cookie=cookie; result.revision=revision;
        result.valid_until_ns=provider::checkedExpiry(now());
        result.hq_us=static_cast<std::uint32_t>(config.deltaFor(latency::Profile::StockR3Hq44100));
        result.ll_us=config.ll_ms*1000; result.hal_pid=hal_; result.owner_pid=owner_;
        result.hal_start_ticks=hal_start_; result.owner_start_ticks=owner_start_; return result;
    }
    void checkRuntimeLease(const provider::Snapshot& lease) const {
        require(lease.boot==boot_ && lease.hal_pid==hal_ && lease.hal_start_ticks==hal_start_,
            "The runtime lease does not belong to this module HAL session.");
        const auto owner=decimal(readOwned(state_,"latency-provider.pid"));
        require(owner>0 && owner<=INT32_MAX,"The saved runtime publisher PID is invalid.");
        provider::verifyRuntimeActivation(lease,readOwned(state_,"latency-runtime.active",4096,0600),
            static_cast<std::uint32_t>(owner),decimal(readOwned(state_,"latency-provider.start")));
    }
    provider::Snapshot recoveryIdentity(const provider::Bytes16& cookie,std::uint64_t expected_start) const {
        require(recovery_ && expected_start && hal_start_==expected_start,"The original HAL identity changed.");
        provider::Snapshot identity;
        identity.boot=boot_; identity.cookie=cookie; identity.hal_pid=hal_; identity.hal_start_ticks=hal_start_;
        provider::verifyRuntimeActivation(identity,readOwned(state_,"latency-runtime.active",4096,0600),0,0);
        return identity;
    }
private:
    std::filesystem::path path_;
    int module_=-1,state_=-1;
    struct stat module_identity_{},state_identity_{},active_identity_{};
    std::uint32_t hal_=0,owner_=0;
    std::uint64_t hal_start_=0,owner_start_=0;
    provider::Bytes16 boot_{};
    bool recovery_=false;
};
template<std::size_t N> std::array<std::uint8_t,N> readRecord(int directory,const char* name,uid_t uid,gid_t gid) {
    const int fd=openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    require(fd>=0,"Runtime record is missing or unavailable.");
    try {
        struct stat before{},after{};
        require(fstat(fd,&before)==0 && S_ISREG(before.st_mode) && before.st_uid==uid &&
            before.st_gid==gid && (before.st_mode&07777)==0640 && before.st_nlink==1 &&
            before.st_size==static_cast<off_t>(N),"Runtime record ownership, mode or size is invalid.");
        const auto bytes=bounded(fd,N);
        require(bytes.size()==N && fstat(fd,&after)==0 && same(before,after) &&
            after.st_uid==uid && after.st_gid==gid && (after.st_mode&07777)==0640 &&
            after.st_size==before.st_size,"Runtime record changed while reading.");
#ifdef __APPLE__
        require(before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec &&
            before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec &&
            before.st_ctimespec.tv_sec==after.st_ctimespec.tv_sec &&
            before.st_ctimespec.tv_nsec==after.st_ctimespec.tv_nsec,"Runtime record changed while reading.");
#else
        require(before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec==after.st_ctim.tv_sec && before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,
            "Runtime record changed while reading.");
#endif
        std::array<std::uint8_t,N> out{};
        std::copy(bytes.begin(),bytes.end(),out.begin()); close(fd); return out;
    } catch (...) { close(fd); throw; }
}
bool rootProcess(const std::string& text) {
    std::istringstream lines(text); std::string line;
    while (std::getline(lines,line)) {
        if (line.rfind("Uid:",0)!=0) continue;
        std::istringstream values(line.substr(4)); std::uint64_t real,effective,saved,fs;
        return (values>>real>>effective>>saved>>fs) && effective==0;
    }
    return false;
}
int statusCommand(const std::filesystem::path& module,bool health=false) {
    provider::Snapshot lease; provider::RuntimeStatus status;
    provider::StatusEvaluation evaluation;
    provider::HealthEvaluation health_evaluation;
    bool have_lease=false,have_status=false;
    int directory=-1;
    try {
        require(geteuid()==0,"Runtime status requires the current root session.");
        const auto* audio=getgrnam("audio"); require(audio!=nullptr,"The audio group is unavailable.");
        const gid_t audio_gid=audio->gr_gid;
        const auto* hal=getpwnam("audioserver"); require(hal!=nullptr,"The audioserver identity is unavailable.");
        const uid_t hal_uid=hal->pw_uid;
        directory=open("/data/vendor/audio/shizuka",O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
        require(directory>=0,"No runtime provider directory is present.");
        struct stat value{};
        require(fstat(directory,&value)==0 && S_ISDIR(value.st_mode) && value.st_uid==0 &&
            value.st_gid==audio_gid && (value.st_mode&07777)==0750,"Runtime provider directory ownership is invalid.");
        lease=provider::decode(readRecord<128>(directory,"provider.bin",0,audio_gid)); have_lease=true;
        ModuleSession session(module,lease.hal_pid);
        session.check();
        if (health) session.checkRuntimeLease(lease);
        require(provider::parseBootUuid(readPath("/proc/sys/kernel/random/boot_id",64))==lease.boot &&
            start(lease.hal_pid)==lease.hal_start_ticks && start(lease.owner_pid)==lease.owner_start_ticks,
            "The runtime provider belongs to an earlier boot or process.");
        const auto owner=readPath("/proc/"+std::to_string(lease.owner_pid)+"/status");
        require(rootProcess(owner) && provider::hasSysAdmin(owner),"The runtime provider no longer has root authority.");
        // The worker updates one fixed inode. Retry a raced read, never accept
        // a partial record or extend a stale observation's freshness deadline.
        for (unsigned attempt=0;attempt<3;++attempt) {
            try {
                status=provider::decodeStatus(readRecord<256>(directory,"status.bin",hal_uid,audio_gid));
                have_status=true; break;
            } catch (const std::exception&) { if (attempt==2) throw; }
        }
        evaluation=provider::evaluateStatus(status,lease,now());
        if (health) health_evaluation=provider::evaluateHealth(status,lease,now());
        session.check();
        if (health) session.checkRuntimeLease(lease);
    } catch (const std::exception& error) {
        evaluation=provider::StatusEvaluation{};
        evaluation.reason=error.what();
        health_evaluation=provider::HealthEvaluation{};
        health_evaluation.reason=error.what();
    }
    if (directory>=0) close(directory);
    if (health) {
        std::cout<<provider::healthJson(health_evaluation)<<'\n';
        return health_evaluation.healthy?0:2;
    }
    std::cout<<provider::statusJson(have_status?&status:nullptr,have_lease?&lease:nullptr,evaluation)<<'\n';
    return 0;
}
int recoveryCommand(const std::map<std::string,std::string>& options) {
    provider::RecoveryEvaluation evaluation;
    int directory=-1;
    try {
        require(options.size()==4,"Recovery check requires module directory, cookie and original HAL PID/start.");
        const auto cookie=provider::parseCookie(options.at("--cookie"));
        const auto hal=decimal(options.at("--hal-pid"));
        const auto ticks=decimal(options.at("--hal-start"));
        require(hal>0 && hal<=INT32_MAX && ticks,"Original HAL PID/start is invalid.");
        ModuleSession session(options.at("--module-dir"),static_cast<std::uint32_t>(hal),true);
        const auto identity=session.recoveryIdentity(cookie,ticks);
        const auto* audio=getgrnam("audio"); require(audio,"The audio group is unavailable.");
        const auto* hal_user=getpwnam("audioserver"); require(hal_user,"The audio HAL user is unavailable.");
        directory=open("/data/vendor/audio/shizuka",O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
        struct stat value{};
        require(directory>=0 && fstat(directory,&value)==0 && value.st_uid==0 &&
            value.st_gid==audio->gr_gid && (value.st_mode&07777)==0750,"Unsafe runtime status directory.");
        for (unsigned attempt=0;attempt<3;++attempt) {
            try {
                const auto status=provider::decodeStatus(readRecord<256>(directory,"status.bin",hal_user->pw_uid,audio->gr_gid));
                evaluation=provider::evaluateRecovery(status,identity,now()); break;
            } catch (const std::exception&) { if (attempt==2) throw; }
        }
        session.check();
    } catch (const std::exception& error) {
        evaluation=provider::RecoveryEvaluation{}; evaluation.reason=error.what();
    }
    if (directory>=0) close(directory);
    std::cout<<provider::recoveryJson(evaluation)<<'\n';
    return evaluation.safe_without_dsp_restart?0:2;
}
}  // namespace

int main(int argc,char** argv) {
    if (argc>=2 && std::string(argv[1])=="--recovery-check") {
        std::map<std::string,std::string> options;
        for (int i=2;i<argc;i+=2) {
            const std::string key=argv[i];
            if ((key!="--module-dir" && key!="--cookie" && key!="--hal-pid" && key!="--hal-start") ||
                i+1>=argc || !options.emplace(key,argv[i+1]).second) {
                std::cout<<shizuka::provider::recoveryJson(shizuka::provider::RecoveryEvaluation{})<<'\n'; return 2;
            }
        }
        return recoveryCommand(options);
    }
    if (argc>=2 && (std::string(argv[1])=="--status" || std::string(argv[1])=="--health")) {
        const bool health=std::string(argv[1])=="--health";
        if (argc==2) return statusCommand("/data/adb/modules/s25_aptx_adaptive",health);
        if (argc==4 && std::string(argv[2])=="--module-dir") return statusCommand(argv[3],health);
        if (health) {
            shizuka::provider::HealthEvaluation rejected;
            rejected.reason="Health accepts only --health [--module-dir PATH].";
            std::cout<<shizuka::provider::healthJson(rejected)<<'\n'; return 2;
        }
        shizuka::provider::StatusEvaluation unavailable;
        unavailable.reason="Status accepts only --status [--module-dir PATH].";
        std::cout<<shizuka::provider::statusJson(nullptr,nullptr,unavailable)<<'\n'; return 0;
    }
    std::unique_ptr<shizuka::provider::AtomicPublisher> output;
    try {
        std::map<std::string,std::string> options;
        for (int i=1;i<argc;i+=2) {
            const std::string key=argv[i];
            require((key=="--module-dir" || key=="--output-dir" || key=="--cookie" || key=="--hal-pid") &&
                i+1<argc && options.emplace(key,argv[i+1]).second,"unknown, duplicate or incomplete provider option");
        }
        require(options.size()==4,"required: --module-dir PATH --output-dir /data/vendor/audio/shizuka --cookie HEX32 --hal-pid PID");
        require(geteuid()==0,"provider requires the current root session");
        require(options.at("--output-dir")=="/data/vendor/audio/shizuka","provider output directory is fixed");
        const auto cookie=shizuka::provider::parseCookie(options.at("--cookie"));
        const auto hal=decimal(options.at("--hal-pid"));
        require(hal>0 && hal<=INT32_MAX,"HAL PID is out of range");
        ModuleSession session(options.at("--module-dir"),static_cast<std::uint32_t>(hal));
        const auto* audio=getgrnam("audio"); require(audio!=nullptr,"audio group is unavailable");
        output=std::make_unique<shizuka::provider::AtomicPublisher>(options.at("--output-dir"),0,audio->gr_gid);
        std::signal(SIGTERM,stop); std::signal(SIGINT,stop); std::signal(SIGHUP,stop);
        std::uint64_t revision=1;
        shizuka::latency::DelaySnapshot previous{}; bool first=true;
        while (!stopping) {
            session.check();
            const auto config=shizuka::latency::readDelayConfig("/data/adb/shizuka/config.conf");
            if (!first && (config.hq_ms!=previous.hq_ms || config.ll_ms!=previous.ll_ms)) {
                require(revision<UINT64_MAX,"configuration revision overflow"); ++revision;
            }
            session.check();
            output->publish(shizuka::provider::encode(session.snapshot(cookie,revision,config)));
            previous=config; first=false;
            struct timespec remaining{0,static_cast<long>(shizuka::provider::kRefreshNs)};
            while (!stopping && nanosleep(&remaining,&remaining)<0) require(errno==EINTR,"provider sleep failed");
        }
        require(output->removeOwned(),"provider stopped; lease unlink is unconfirmed and expiry must be enforced");
        std::cout<<"{\"provider_stopped\":true,\"lease_unlinked\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        const bool removed=!output || output->removeOwned();
        std::cerr<<"provider stopped: "<<error.what()<<"; lease_unlinked="<<(removed?"true":"false")<<'\n';
        return 2;
    }
}
