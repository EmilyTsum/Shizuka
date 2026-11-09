// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/bounded_files.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

namespace shizuka::latency {
namespace {
class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) { if (fd < 0) fail("open"); }
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    int get() const { return fd_; }
    static void fail(const char* operation) { throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }
private:
    int fd_;
};
void syncDirectory(const std::filesystem::path& path) {
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (::fsync(fd.get()) != 0) Descriptor::fail("directory fsync");
}
}  // namespace

std::vector<std::uint8_t> readBoundedFile(const std::filesystem::path& path, std::size_t maximum) {
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat before{}, after{};
    if (::fstat(fd.get(), &before) != 0) Descriptor::fail("input fstat");
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uintmax_t>(before.st_size) > maximum)
        throw std::runtime_error("input must be a bounded regular file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(before.st_size));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::read(fd.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) Descriptor::fail("input read");
        if (!count) throw std::runtime_error("input changed or ended during read");
        done += static_cast<std::size_t>(count);
    }
    std::uint8_t extra;
    ssize_t tail;
    do { tail = ::read(fd.get(), &extra, 1); } while (tail < 0 && errno == EINTR);
    if (tail < 0) Descriptor::fail("input tail read");
    if (::fstat(fd.get(), &after) != 0) Descriptor::fail("input final fstat");
    if (tail || before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
        before.st_ctime != after.st_ctime)
        throw std::runtime_error("input changed during read");
    return bytes;
}

AtomicDirectory::AtomicDirectory(std::filesystem::path destination)
    : destination_(std::filesystem::absolute(std::move(destination)).lexically_normal()) {
    const auto status = std::filesystem::symlink_status(destination_);
    if (status.type() != std::filesystem::file_type::not_found)
        throw std::runtime_error("refusing to overwrite an existing output path");
    if (!std::filesystem::is_directory(destination_.parent_path()))
        throw std::runtime_error("output parent directory must already exist");
    auto pattern = (destination_.parent_path() / ".shizuka-latency-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    if (!::mkdtemp(name.data())) Descriptor::fail("output mkdtemp");
    temporary_ = name.data();
}
AtomicDirectory::~AtomicDirectory() {
    if (!published_ && !temporary_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(temporary_, ignored);
    }
}
void AtomicDirectory::write(const std::string& name, const std::vector<std::uint8_t>& bytes) {
    if (published_ || name.empty() || name.find('/') != std::string::npos || name == "." || name == "..")
        throw std::runtime_error("invalid unpublished output filename");
    Descriptor fd(::open((temporary_ / name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::write(fd.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) Descriptor::fail("output write");
        if (!count) throw std::runtime_error("short output write");
        done += static_cast<std::size_t>(count);
    }
    if (::fsync(fd.get()) != 0) Descriptor::fail("output fsync");
}
void AtomicDirectory::publish() {
    if (published_) throw std::runtime_error("output already published");
    syncDirectory(temporary_);
#ifdef __APPLE__
    if (::renamex_np(temporary_.c_str(), destination_.c_str(), RENAME_EXCL) != 0)
        Descriptor::fail("exclusive output rename");
#elif defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary_.c_str(), AT_FDCWD,
                  destination_.c_str(), 1U /* RENAME_NOREPLACE */) != 0)
        Descriptor::fail("exclusive output rename");
#else
    throw std::runtime_error("this platform lacks atomic no-replace directory publication");
#endif
    published_ = true;
    syncDirectory(destination_.parent_path());
}
}  // namespace shizuka::latency
