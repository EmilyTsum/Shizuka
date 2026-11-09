// SPDX-License-Identifier: GPL-2.0-only
#include "shizuka/bounded_files.hpp"
#include "shizuka/encoded_ttp.hpp"
#include "shizuka/external_config.hpp"

#include <iostream>
#include <limits>
#include <map>
#include <string>

namespace {
std::uint64_t number(const std::string& text) {
    if (text.empty() || (text.size() > 1 && text[0] == '0'))
        throw std::runtime_error("expected a canonical unsigned decimal integer");
    std::uint64_t value = 0;
    for (const auto c : text) {
        if (c < '0' || c > '9') throw std::runtime_error("expected an unsigned decimal integer");
        const auto digit = static_cast<unsigned>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            throw std::runtime_error("integer exceeds uint64 range");
        value = value * 10 + digit;
    }
    return value;
}
std::uint32_t handle(const std::string& text) {
    auto value = number(text);
    if (value > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("handle exceeds uint32 range");
    return static_cast<std::uint32_t>(value);
}
}  // namespace

int main(int argc, char** argv) {
    using namespace shizuka::latency;
    try {
        std::map<std::string, std::string> options;
        const std::vector<std::string> names{"--data", "--metadata", "--delta-us", "--config", "--profile", "--output-dir",
            "--address-mode", "--data-address", "--data-map-handle", "--data-capacity",
            "--metadata-address", "--metadata-map-handle", "--metadata-capacity"};
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "latency-transform --data FILE --metadata FILE --delta-us INTEGER --profile stock-r3-hq-44100 --output-dir NEW_DIRECTORY\n"
                "Use --config PRIVATE_CONFIG_FILE instead of --delta-us to snapshot the module HQ delay setting.\n"
                "Optional format-only 44B body: --address-mode physical|offset --data-address DECIMAL --data-map-handle INTEGER --data-capacity INTEGER --metadata-address DECIMAL --metadata-map-handle INTEGER --metadata-capacity INTEGER\n"
                "Offline transform only. No DSP image, graph, device, mapping or send operation.\n";
            return 0;
        }
        for (int i = 1; i < argc; i += 2) {
            const std::string key = argv[i];
            bool known = false; for (const auto& name : names) if (name == key) known = true;
            if (!known || i + 1 >= argc || !options.emplace(key, argv[i + 1]).second)
                throw std::runtime_error("unknown, duplicate or incomplete option; use --help");
        }
        for (const auto& name : std::vector<std::string>{"--data", "--metadata", "--profile", "--output-dir"})
            if (!options.count(name)) throw std::runtime_error("missing required option " + name);
        if (options.count("--delta-us") + options.count("--config") != 1)
            throw Rejected("provide exactly one of --delta-us and --config");
        std::size_t maps = 0;
        for (const auto& name : std::vector<std::string>{"--address-mode", "--data-address", "--data-map-handle", "--data-capacity", "--metadata-address", "--metadata-map-handle", "--metadata-capacity"}) maps += options.count(name);
        if (maps && maps != 7) throw std::runtime_error("provide explicit address mode and all six mapping fields, or none");
        const auto profile = parseProfile(options.at("--profile"));
        const auto delta = options.count("--config")
            ? readDelayConfig(options.at("--config")).deltaFor(profile)
            : number(options.at("--delta-us"));
        RDBuffer input{readBoundedFile(options.at("--data"), kMaximumEncodedBytes),
                       readBoundedFile(options.at("--metadata"), kMaximumMetadataBytes)};
        const auto output = validateAndRewrite(input, delta, profile);
        std::vector<std::uint8_t> command;
        if (maps) {
            const auto mode_name = options.at("--address-mode");
            if (mode_name != "physical" && mode_name != "offset") throw Rejected("unknown address mode");
            const auto mode = mode_name == "physical" ? AddressMode::Physical : AddressMode::Offset;
            const auto bytes = serializeWriteCommand(output,
                {mode, number(options.at("--data-address")), handle(options.at("--data-map-handle")), number(options.at("--data-capacity"))},
                {mode, number(options.at("--metadata-address")), handle(options.at("--metadata-map-handle")), number(options.at("--metadata-capacity"))});
            command.assign(bytes.begin(), bytes.end());
        }
        const auto report = reportJson(output, delta);
        AtomicDirectory directory(options.at("--output-dir"));
        directory.write("wr-data.bin", output.data);
        directory.write("wr-metadata.bin", output.metadata);
        directory.write("relay-report.json", {report.begin(), report.end()});
        if (maps) directory.write("wr-command.bin", command);
        directory.publish();
        std::cout << report;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "relay rejected buffer: " << error.what() << '\n';
        return 2;
    }
}
