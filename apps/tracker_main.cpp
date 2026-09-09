#include "swarmsync/common.hpp"
#include "swarmsync/tracker.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void handle_stop_signal(int) {
  stop_requested = 1;
}

std::unordered_map<std::string, std::string> parse_options(int argc, char** argv) {
  std::unordered_map<std::string, std::string> options;
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (!name.starts_with("--") || index + 1 >= argc) {
      throw swarmsync::Error("Expected an option followed by a value: " + name);
    }
    const std::string value = argv[++index];
    if (value.starts_with("--") || !options.emplace(name, value).second) {
      throw swarmsync::Error("Invalid or duplicate option: " + name);
    }
  }
  return options;
}

std::string require(const std::unordered_map<std::string, std::string>& options, std::string_view name) {
  const auto item = options.find(std::string(name));
  if (item == options.end() || item->second.empty()) {
    throw swarmsync::Error("Required option is missing: " + std::string(name));
  }
  return item->second;
}

std::string optional(const std::unordered_map<std::string, std::string>& options, std::string_view name,
                     std::string fallback) {
  const auto item = options.find(std::string(name));
  return item == options.end() ? std::move(fallback) : item->second;
}

std::uint32_t number(std::string_view value, std::string_view name) {
  std::uint32_t parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
    throw swarmsync::Error("Option must be an unsigned integer: " + std::string(name));
  }
  return parsed;
}

std::string token(const std::unordered_map<std::string, std::string>& options) {
  const auto supplied = options.find("--token");
  if (supplied != options.end()) {
    return supplied->second;
  }
  if (const char* environment_token = std::getenv("SWARMSYNC_TOKEN"); environment_token != nullptr && *environment_token != '\0') {
    return environment_token;
  }
  throw swarmsync::Error("Set SWARMSYNC_TOKEN or pass --token for this private tracker");
}

void usage() {
  std::cout << "Usage: swarmsync-tracker --port <0-65535> [--bind <address>] [--ttl-seconds <seconds>]\n"
               "       [--max-connections <count>] [--workers <count>] [--max-swarms <count>]\n"
               "       [--max-peers-per-swarm <count>] [--max-response-peers <count>] [--token <token>]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
      usage();
      return 0;
    }
    const auto options = parse_options(argc, argv);
    for (const auto& [name, value] : options) {
      static_cast<void>(value);
      if (name != "--port" && name != "--bind" && name != "--ttl-seconds" && name != "--max-connections" &&
          name != "--workers" && name != "--max-swarms" && name != "--max-peers-per-swarm" &&
          name != "--max-response-peers" && name != "--token") {
        throw swarmsync::Error("Unsupported option: " + name);
      }
    }
    const auto requested_port = number(require(options, "--port"), "--port");
    const auto ttl_seconds = number(optional(options, "--ttl-seconds", "30"), "--ttl-seconds");
    if (requested_port > 65535U || ttl_seconds == 0U) {
      throw swarmsync::Error("--port must be from 0 to 65535 and --ttl-seconds must be positive");
    }

    swarmsync::TrackerLimits limits;
    limits.max_connections = number(optional(options, "--max-connections", "32"), "--max-connections");
    limits.worker_count = number(optional(options, "--workers", "4"), "--workers");
    limits.max_swarms = number(optional(options, "--max-swarms", "1024"), "--max-swarms");
    limits.max_peers_per_swarm =
        number(optional(options, "--max-peers-per-swarm", "256"), "--max-peers-per-swarm");
    limits.max_response_peers =
        number(optional(options, "--max-response-peers", "128"), "--max-response-peers");

    swarmsync::TrackerServer tracker(static_cast<std::uint16_t>(requested_port), token(options),
                                     std::chrono::seconds(ttl_seconds), optional(options, "--bind", "127.0.0.1"),
                                     limits);
    tracker.start();
    std::cout << "SwarmSync tracker listening on " << tracker.endpoint().host << ':' << tracker.endpoint().port
              << " with presence TTL " << ttl_seconds << " seconds\nPress Ctrl+C to stop.\n" << std::flush;
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);
    while (stop_requested == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    tracker.stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
