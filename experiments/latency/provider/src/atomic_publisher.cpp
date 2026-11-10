// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/provider.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shizuka::provider {
namespace {
void require(bool value, const char* reason) { if (!value) throw latency::Rejected(reason); }
void regular(const struct stat& value, uid_t uid, gid_t gid, mode_t mode) {
    require(S_ISREG(value.st_mode) && value.st_uid==uid && value.st_gid==gid &&
        (value.st_mode&07777)==mode && value.st_nlink==1,
        "provider output must be a private owned regular file");
}
void existingRecord(int directory, uid_t uid, gid_t gid) {
    struct stat value{};
    if (fstatat(directory,"provider.bin",&value,AT_SYMLINK_NOFOLLOW)) {
        require(errno==ENOENT,"cannot inspect existing provider record"); return;
    }
    regular(value,uid,gid,0640);
    require(value.st_size==128,"existing provider record has invalid size");
}
}  // namespace

AtomicPublisher::AtomicPublisher(const std::filesystem::path& path, uid_t uid, gid_t gid)
    :path_(path),uid_(uid),gid_(gid) {
    bool made=false;
    try {
        if (mkdir(path_.c_str(),0750)==0) made=true;
        else require(errno==EEXIST,"cannot create provider directory");
        directory_=open(path_.c_str(),O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
        require(directory_>=0,"cannot open provider directory");
        if (made) require(fchown(directory_,uid_,gid_)==0 && fchmod(directory_,0750)==0,
                          "cannot set provider directory ownership");
        struct stat value{};
        require(fstat(directory_,&value)==0 && S_ISDIR(value.st_mode) && value.st_uid==uid_ &&
            value.st_gid==gid_ && (value.st_mode&07777)==0750,"unsafe provider directory");
        directory_device_=value.st_dev; directory_inode_=value.st_ino;
        lock_=openat(directory_,"provider.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
        require(lock_>=0,"cannot open provider owner lock");
        require(fstat(lock_,&value)==0 && S_ISREG(value.st_mode) && value.st_uid==uid_ &&
            (value.st_mode&07777)==0600 && value.st_nlink==1,"unsafe provider owner lock");
        require(flock(lock_,LOCK_EX|LOCK_NB)==0,"another provider owns the output directory");
        existingRecord(directory_,uid_,gid_);
    } catch (...) {
        if (lock_>=0) close(lock_);
        if (directory_>=0) close(directory_);
        throw;
    }
}
AtomicPublisher::~AtomicPublisher() {
    removeOwned();
    if (published_fd_>=0) close(published_fd_);
    if (lock_>=0) close(lock_);
    if (directory_>=0) close(directory_);
}
void AtomicPublisher::verifyDirectory() const {
    struct stat fd{},path{};
    require(fstat(directory_,&fd)==0 && lstat(path_.c_str(),&path)==0 &&
        S_ISDIR(path.st_mode) && path.st_dev==directory_device_ && path.st_ino==directory_inode_ &&
        fd.st_dev==directory_device_ && fd.st_ino==directory_inode_ && fd.st_uid==uid_ &&
        fd.st_gid==gid_ && (fd.st_mode&07777)==0750,"provider directory identity or permissions changed");
}
void AtomicPublisher::publish(const Record& record) {
    decode(record);
    verifyDirectory();
    existingRecord(directory_,uid_,gid_);
    if (++counter_==0) throw latency::Rejected("provider temporary sequence overflow");
    const std::string temporary=".provider-"+std::to_string(getpid())+"-"+std::to_string(counter_);
    const int fd=openat(directory_,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    require(fd>=0,"cannot create provider temporary record");
    bool renamed=false;
    try {
        require(fchown(fd,uid_,gid_)==0 && fchmod(fd,0640)==0,"cannot protect provider record");
        std::size_t written=0;
        while (written<record.size()) {
            auto result=write(fd,record.data()+written,record.size()-written);
            if (result<0 && errno==EINTR) continue;
            require(result>0,"provider record write failed");
            written+=static_cast<std::size_t>(result);
        }
        struct stat value{};
        require(fstat(fd,&value)==0,"cannot inspect provider temporary record");
        regular(value,uid_,gid_,0640);
        require(fsync(fd)==0,"provider record fsync failed");
        verifyDirectory();
        existingRecord(directory_,uid_,gid_);
        require(renameat(directory_,temporary.c_str(),directory_,"provider.bin")==0,
                "provider record atomic replacement failed");
        renamed=true;
        // Pin the published inode until ownership ends. A saved dev/ino alone
        // can alias a replacement file after immediate inode reuse on Linux.
        if (published_fd_>=0) close(published_fd_);
        published_fd_=fd;
        published_=true; published_device_=value.st_dev; published_inode_=value.st_ino;
        require(fsync(directory_)==0,"provider directory fsync failed");
    } catch (...) {
        if (!renamed) close(fd);
        if (!renamed) unlinkat(directory_,temporary.c_str(),0);
        throw;
    }
}
bool AtomicPublisher::removeOwned() noexcept {
    if (!published_) return true;
    struct stat value{};
    if (fstatat(directory_,"provider.bin",&value,AT_SYMLINK_NOFOLLOW)) {
        if (errno==ENOENT) {
            published_=false;
            if (published_fd_>=0) close(published_fd_);
            published_fd_=-1;
            return true;
        }
        return false;
    }
    if (!S_ISREG(value.st_mode) || value.st_uid!=uid_ || value.st_dev!=published_device_ ||
        value.st_ino!=published_inode_) return false;
    if (unlinkat(directory_,"provider.bin",0)) return false;
    published_=false;
    if (published_fd_>=0) close(published_fd_);
    published_fd_=-1;
    return fsync(directory_)==0;
}
}  // namespace shizuka::provider
