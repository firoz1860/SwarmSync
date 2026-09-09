#include "swarmsync/common.hpp"
#include "swarmsync/crypto.hpp"
#include "swarmsync/download.hpp"
#include "swarmsync/manifest.hpp"
#include "swarmsync/peer_server.hpp"
#include "swarmsync/tracker.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_stop_signal(int) {
  stop_requested = 1;
}

struct Options {
  std::unordered_map<std::string, std::string> values;
  bool help{};
};

Options parse_options(int argc, char** argv, int start) {
  Options options;
  for (int index = start; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help") {
      options.help = true;
      continue;
    }
    if (!name.starts_with("--") || index + 1 >= argc) {
      throw swarmsync::Error("Expected an option followed by a value: " + name);
    }
    const std::string value = argv[++index];
    if (value.starts_with("--")) {
      throw swarmsync::Error("Option is missing a value: " + name);
    }
    if (!options.values.emplace(name, value).second) {
      throw swarmsync::Error("Option appears more than once: " + name);
    }
  }
  return options;
}

void allow_only(const Options& options, const std::vector<std::string_view>& allowed) {
  for (const auto& [name, value] : options.values) {
    static_cast<void>(value);
    bool found = false;
    for (const auto candidate : allowed) {
      if (name == candidate) {
        found = true;
        break;
      }
    }
    if (!found) {
      throw swarmsync::Error("Unsupported option: " + name);
    }
  }
}

std::string require(const Options& options, std::string_view name) {
  const auto item = options.values.find(std::string(name));
  if (item == options.values.end() || item->second.empty()) {
    throw swarmsync::Error("Required option is missing: " + std::string(name));
  }
  return item->second;
}

std::string optional(const Options& options, std::string_view name, std::string fallback) {
  const auto item = options.values.find(std::string(name));
  return item == options.values.end() ? std::move(fallback) : item->second;
}

std::uint32_t number(std::string_view value, std::string_view name) {
  std::uint32_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
    throw swarmsync::Error("Option must be an unsigned integer: " + std::string(name));
  }
  return parsed;
}

std::uint16_t port(const Options& options, std::string_view name) {
  const auto parsed = number(require(options, name), name);
  if (parsed == 0U || parsed > 65535U) {
    throw swarmsync::Error("Option must be a TCP port from 1 to 65535: " + std::string(name));
  }
  return static_cast<std::uint16_t>(parsed);
}

std::uint16_t listen_port(const Options& options, std::string_view name) {
  const auto parsed = number(require(options, name), name);
  if (parsed > 65535U) {
    throw swarmsync::Error("Option must be a TCP port from 0 to 65535: " + std::string(name));
  }
  return static_cast<std::uint16_t>(parsed);
}

std::string token(const Options& options) {
  const auto supplied = options.values.find("--token");
  if (supplied != options.values.end()) {
    return supplied->second;
  }
  if (const char* environment_token = std::getenv("SWARMSYNC_TOKEN"); environment_token != nullptr && *environment_token != '\0') {
    return environment_token;
  }
  throw swarmsync::Error("Set SWARMSYNC_TOKEN or pass --token for this private swarm");
}

void print_usage() {
  std::cout
      << "SwarmSync private peer-to-peer file sharing\n\n"
      << "Commands:\n"
      << "  manifest create --file <path> --out <path> [--chunk-size <bytes>]\n"
      << "  seed --manifest <path> --file <path> --tracker-host <host> --tracker-port <port>\n"
      << "       --listen-port <0-65535> --peer-id <id> [--bind <address>] [--heartbeat-ms <ms>]\n"
      << "  download --manifest <path> --out-dir <directory> --tracker-host <host> --tracker-port <port>\n"
      << "       --listen-port <0-65535> --peer-id <id> [--workers <count>] [--retries <count>]\n\n"
      << "Read the token from SWARMSYNC_TOKEN by default. --token is an explicit override.\n";
}

void create_manifest(const Options& options) {
  allow_only(options, {"--file", "--out", "--chunk-size"});
  const auto source = std::filesystem::path(require(options, "--file"));
  const auto output = std::filesystem::path(require(options, "--out"));
  const auto chunk_size = number(optional(options, "--chunk-size", "262144"), "--chunk-size");
  const auto manifest = swarmsync::Manifest::from_file(source, chunk_size);
  manifest.save(output);
  std::cout << "Created manifest: " << output << "\nSwarm ID: " << manifest.swarm_id()
            << "\nChunks: " << manifest.chunk_hashes.size() << '\n';
}

void seed(const Options& options) {
  allow_only(options, {"--manifest", "--file", "--tracker-host", "--tracker-port", "--listen-port", "--peer-id",
                       "--bind", "--heartbeat-ms", "--token"});
  const auto manifest = swarmsync::Manifest::load(require(options, "--manifest"));
  const auto source = std::filesystem::path(require(options, "--file"));
  if (!std::filesystem::is_regular_file(source) || std::filesystem::file_size(source) != manifest.file_size_bytes ||
      swarmsync::sha256_file(source) != manifest.file_sha256) {
    throw swarmsync::Error("Seed file does not match the selected manifest");
  }

  const auto shared_token = token(options);
  const auto tracker_endpoint = swarmsync::Endpoint{require(options, "--tracker-host"), port(options, "--tracker-port")};
  const auto peer_id = require(options, "--peer-id");
  const auto heartbeat = std::chrono::milliseconds(number(optional(options, "--heartbeat-ms", "1000"), "--heartbeat-ms"));
  if (heartbeat.count() <= 0) {
    throw swarmsync::Error("--heartbeat-ms must be positive");
  }

  swarmsync::PeerServer server({manifest, source, shared_token, listen_port(options, "--listen-port"),
                                optional(options, "--bind", "127.0.0.1"), 32U, {}});
  server.start();
  const swarmsync::TrackerClient tracker(tracker_endpoint, shared_token);
  const auto announce = [&] {
    tracker.announce({manifest.swarm_id(), peer_id, server.endpoint().port, {}, true});
  };
  announce();
  std::cout << "Seeder peer " << peer_id << " is serving swarm " << manifest.swarm_id() << " on "
            << server.endpoint().host << ':' << server.endpoint().port << "\nPress Ctrl+C to stop.\n" << std::flush;

  std::signal(SIGINT, handle_stop_signal);
  std::signal(SIGTERM, handle_stop_signal);
  while (stop_requested == 0) {
    std::this_thread::sleep_for(heartbeat);
    if (stop_requested != 0) {
      break;
    }
    try {
      announce();
    } catch (const swarmsync::Error&) {
      swarmsync::log_error("Seeder heartbeat could not reach the tracker");
    }
  }
  server.stop();
}

void download(const Options& options) {
  allow_only(options, {"--manifest", "--out-dir", "--tracker-host", "--tracker-port", "--listen-port", "--peer-id",
                       "--workers", "--retries", "--bind", "--heartbeat-ms", "--token"});
  const auto manifest = swarmsync::Manifest::load(require(options, "--manifest"));
  swarmsync::DownloadConfig config{manifest,
                                    require(options, "--out-dir"),
                                    {require(options, "--tracker-host"), port(options, "--tracker-port")},
                                    token(options),
                                    listen_port(options, "--listen-port"),
                                    require(options, "--peer-id"),
                                    number(optional(options, "--workers", "4"), "--workers"),
                                    number(optional(options, "--retries", "3"), "--retries"),
                                    std::chrono::seconds(3),
                                    std::chrono::milliseconds(number(optional(options, "--heartbeat-ms", "1000"), "--heartbeat-ms")),
                                    optional(options, "--bind", "127.0.0.1"),
                                    32U};
  const auto result = swarmsync::DownloadCoordinator(std::move(config)).run();
  std::cout << "Verified download completed: " << result.final_path << "\nChunks: " << result.completed_chunks
            << "  Retry events: " << result.retry_events << "  Elapsed: " << result.elapsed.count() << " ms\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
      print_usage();
      return 0;
    }
    const std::string command = argv[1];
    if (command == "manifest") {
      if (argc < 3 || std::string_view(argv[2]) != "create") {
        throw swarmsync::Error("Use: swarmsync manifest create --file <path> --out <path>");
      }
      const auto options = parse_options(argc, argv, 3);
      if (options.help) {
        print_usage();
        return 0;
      }
      create_manifest(options);
      return 0;
    }

    const auto options = parse_options(argc, argv, 2);
    if (options.help) {
      print_usage();
      return 0;
    }
    if (command == "seed") {
      seed(options);
      return 0;
    }
    if (command == "download") {
      download(options);
      return 0;
    }
    throw swarmsync::Error("Unknown command: " + command);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
