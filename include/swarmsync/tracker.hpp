#pragma once

#include "swarmsync/protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace swarmsync {

struct TrackerLimits {
  std::uint32_t max_connections{32U};
  std::uint32_t worker_count{4U};
  std::uint32_t max_swarms{1024U};
  std::uint32_t max_peers_per_swarm{256U};
  std::uint32_t max_response_peers{128U};
};

struct PeerAdvertisement {
  std::string swarm_id;
  std::string peer_id;
  std::uint16_t listen_port{};
  std::vector<std::uint32_t> chunks;
  bool has_all_chunks{};
};

struct DiscoveredPeer {
  std::string peer_id;
  Endpoint endpoint;
  std::vector<std::uint32_t> chunks;
  bool has_all_chunks{};
};

class TrackerClient {
 public:
  TrackerClient(Endpoint endpoint, std::string token,
                std::chrono::milliseconds timeout = std::chrono::seconds(3));

  void announce(const PeerAdvertisement& advertisement) const;
  std::vector<DiscoveredPeer> peers(std::string_view swarm_id) const;

 private:
  Endpoint endpoint_;
  std::string token_;
  std::chrono::milliseconds timeout_;
};

class TrackerServer {
 public:
  TrackerServer(std::uint16_t port, std::string token, std::chrono::seconds ttl,
                std::string bind_address = "127.0.0.1", TrackerLimits limits = {});
  ~TrackerServer();

  TrackerServer(const TrackerServer&) = delete;
  TrackerServer& operator=(const TrackerServer&) = delete;

  void start();
  void stop();
  [[nodiscard]] Endpoint endpoint() const;

 private:
  struct PeerRecord {
    DiscoveredPeer peer;
    std::chrono::steady_clock::time_point last_seen;
  };

  struct QueuedConnection {
    Socket client;
    Endpoint remote_endpoint;
  };

  void accept_loop();
  void worker_loop();
  void handle_connection(Socket client, Endpoint remote_endpoint);
  void prune_expired_locked(std::chrono::steady_clock::time_point now);

  std::uint16_t requested_port_;
  std::string token_;
  std::chrono::seconds ttl_;
  std::string bind_address_;
  TrackerLimits limits_;
  Endpoint endpoint_;
  Socket listener_;
  std::atomic_bool running_{false};
  std::atomic_uint32_t outstanding_connections_{0U};
  std::thread accept_thread_;
  std::vector<std::thread> worker_threads_;
  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::deque<QueuedConnection> queue_;
  std::mutex peers_mutex_;
  std::unordered_map<std::string, std::unordered_map<std::string, PeerRecord>> swarms_;
};

}  // namespace swarmsync
