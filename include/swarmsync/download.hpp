#pragma once

#include "swarmsync/manifest.hpp"
#include "swarmsync/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace swarmsync {

struct DownloadConfig {
  Manifest manifest;
  std::filesystem::path output_directory;
  Endpoint tracker_endpoint;
  std::string token;
  std::uint16_t listen_port{};
  std::string peer_id;
  std::uint32_t worker_count{4U};
  std::uint32_t max_retries{3U};
  std::chrono::milliseconds network_timeout{std::chrono::seconds(3)};
  std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(1)};
  std::string bind_address{"127.0.0.1"};
  std::uint32_t peer_max_connections{32U};
};

struct DownloadResult {
  std::filesystem::path final_path;
  std::size_t completed_chunks{};
  std::size_t retry_events{};
  std::chrono::milliseconds elapsed{};
};

class DownloadCoordinator {
 public:
  explicit DownloadCoordinator(DownloadConfig config);
  DownloadResult run();

 private:
  DownloadConfig config_;
};

}  // namespace swarmsync
