#pragma once

#include "swarmsync/manifest.hpp"
#include "swarmsync/protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace swarmsync {

struct PeerServerConfig {
  Manifest manifest;
  std::filesystem::path file_path;
  std::string token;
  std::uint16_t port{};
  std::string bind_address{"127.0.0.1"};
  std::uint32_t max_connections{32U};
  std::function<bool(std::uint32_t)> has_chunk;
};

class PeerServer {
 public:
  explicit PeerServer(PeerServerConfig config);
  ~PeerServer();

  PeerServer(const PeerServer&) = delete;
  PeerServer& operator=(const PeerServer&) = delete;

  void start();
  void stop();
  [[nodiscard]] Endpoint endpoint() const;

 private:
  void accept_loop();
  void worker_loop();
  void serve_connection(Socket client);
  [[nodiscard]] bool has_chunk(std::uint32_t index) const;

  PeerServerConfig config_;
  Endpoint endpoint_;
  Socket listener_;
  std::atomic_bool running_{false};
  std::atomic_uint32_t outstanding_connections_{0U};
  std::thread accept_thread_;
  std::vector<std::thread> worker_threads_;
  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::deque<Socket> queue_;
};

}  // namespace swarmsync
