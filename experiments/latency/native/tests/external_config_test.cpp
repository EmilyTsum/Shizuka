// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/external_config.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace shizuka::latency;
namespace {
unsigned checks = 0;
void check(bool condition) { ++checks; if (!condition) throw std::runtime_error("config contract assertion failed"); }
template<class F> void rejects(F f) {
    bool failed = false;
    try { f(); } catch (const Rejected&) { failed = true; }
    check(failed);
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path); file << text; file.close();
    if (chmod(path.c_str(), 0600)) throw std::runtime_error("chmod failed");
}
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        char pattern[] = "/tmp/shizuka-config-XXXXXX";
        auto created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}
int main() {
    try {
        auto config = parseDelayConfig("# module file\r\nschema = 2\radaptive_hq_ttp_delta_ms=150\nadaptive_ll_ttp_delta_ms=100\n");
        check(config.hq_ms == 150 && config.ll_ms == 100);
        check(config.deltaFor(Profile::StockR3Hq44100) == 150000);
        check(parseDelayConfig("schema=2\n").deltaFor(Profile::StockR3Hq44100) == 0);
        check(parseDelayConfig("schema=2\nclassic_qhs_host_aware=true\naptx_voice_enabled=system\nadaptive_priority=000001\naptx_hd_priority=0\n").hq_ms == 0);
        for (unsigned i = 0; i <= 150; ++i)
            check(parseDelayConfig("schema=2\nadaptive_hq_ttp_delta_ms=" + std::to_string(i)).hq_ms == i);
        for (const std::string text : {"", "schema=1", "schema=3", "schema=2\nschema=2", "schema=2\nx=0",
            "schema=2\nadaptive.hq.ttp_delta_ms=150", "schema=2\nadaptive_hq_ttp_delta_ms=151",
            "schema=2\nadaptive_ll_ttp_delta_ms=101", "schema=2\nadaptive_hq_ttp_delta_ms=-1",
            "schema=2\nadaptive_hq_ttp_delta_ms=+1", "schema=2\nadaptive_hq_ttp_delta_ms=1.5",
            "schema=2\nadaptive_hq_ttp_delta_ms=00", "schema=2\nadaptive_hq_ttp_delta_ms=0150",
            "schema=2\nclassic_qhs_host_aware=garbage", "schema=2\nclassic_qhs_host_aware=system",
            "schema=2\naptx_voice_enabled=", "schema=2\naptx_voice_enabled=yes",
            "schema=2\nadaptive_priority=1000000", "schema=2\nadaptive_priority=-1",
            "schema=2\naptx_hd_priority=x", "schema=2\naptx_hd_priority=",
            "schema=2\nadaptive_hq_ttp_delta_ms=1 #comment", "schema=2\nadaptive_hq_ttp_delta_ms=",
            "schema=2\nadaptive_hq_ttp_delta_ms=9999999999999999999999999999999999999",
            "schema=2\nadaptive_hq_ttp_delta_ms=1\nadaptive_hq_ttp_delta_ms=2", "schema=2\nbroken"})
            rejects([&] { parseDelayConfig(text); });
        rejects([] { parseDelayConfig(std::string("schema=2\0evil", 13)); });
        rejects([] { parseDelayConfig(std::string(16385, '#')); });
        rejects([] { DelaySnapshot{}.deltaFor(static_cast<Profile>(1)); });
        rejects([] { DelaySnapshot{151, 0}.deltaFor(Profile::StockR3Hq44100); });
        rejects([] { DelaySnapshot{0, 101}.deltaFor(Profile::StockR3Hq44100); });

        Temporary temp;
        auto path = temp.path / "config.conf";
        write(path, "schema=2\nadaptive_hq_ttp_delta_ms=150\n");
        const auto first = readDelayConfig(path);
        check(first.hq_ms == 150);
        // Same atomic replace used by ExtensionConfigStore. An in-flight
        // snapshot remains immutable; the next complete buffer sees new value.
        auto replacement = temp.path / "next";
        write(replacement, "schema=2\nadaptive_hq_ttp_delta_ms=50\n");
        std::filesystem::rename(replacement, path);
        check(first.hq_ms == 150 && readDelayConfig(path).hq_ms == 50);
        chmod(path.c_str(), 0644); rejects([&] { readDelayConfig(path); });
        chmod(path.c_str(), 0600); chmod(temp.path.c_str(), 0755); rejects([&] { readDelayConfig(path); });
        chmod(temp.path.c_str(), 0700);
        auto link = temp.path / "link";
        std::filesystem::create_symlink(path, link); rejects([&] { readDelayConfig(link); });
        auto dirlink = temp.path / "dirlink";
        std::filesystem::create_directory_symlink(temp.path, dirlink); rejects([&] { readDelayConfig(dirlink / "config.conf"); });
        auto fifo = temp.path / "fifo";
        if (mkfifo(fifo.c_str(), 0600)) throw std::runtime_error("mkfifo failed");
        rejects([&] { readDelayConfig(fifo); });
        std::filesystem::remove(path); rejects([&] { readDelayConfig(path); });
        write(path, std::string(16385, '#')); rejects([&] { readDelayConfig(path); });
        std::cout << checks << " external config checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
