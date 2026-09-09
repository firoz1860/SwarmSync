#include "swarmsync/tracker.hpp"

#include "swarmsync/common.hpp"
#include "swarmsync/crypto.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <utility>

namespace swarmsync {
namespace {

std::vector<std::string_view> split_response(std::string_view line) {
  if (line.empty() || line.size() > kMaxHeaderBytes) {
    throw Error("Tracker response has an invalid length");
  }
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(line[index]);
    if (character < 32U || character > 126U) {
      throw Error("Tracker response contains an invalid character");
    }
    if (line[index] == ' ') {
      if (start == index) {
        throw Error("Tracker response contains an empty field");
      }
      fields.push_back(line.substr(start, index - start));
      start = index + 1U;
    }
  }
  if (start == line.size()) {
    throw Error("Tracker response ends with a separator");
  }
  fields.push_back(line.substr(start));
  return fields;
}

DiscoveredPeer parse_peer_response(std::string_view line) {
  const auto fields = split_response(line);
  if (fields.size() != 5U || fields.front() != "PEER") {
    throw Error("Tracker returned an invalid peer record");
  }

  const auto validated = parse_tracker_request(
      "ANNOUNCE check-token check-swarm " + std::string(fields[1]) + " " +
      std::string(fields[3]) + " " + std::string(fields[4]));
  static_cast<void>(parse_tracker_request("PEERS check-token " + std::string(fields[2])));

  return {std::string(fields[1]), {std::string(fields[2]), validated.listen_port},
          validated.chunks, validated.has_all_chunks};
}

void expect_ok(const Socket& connection) {
  const auto response = read_line(connection);
  if (response != "OK") {
    throw Error("Tracker rejected request: " + response);
  }
}

void send_tracker_error(const Socket& connection, std::string_view code) noexcept {
  try {
    write_all(connection, "ERROR " + std::string(code) + "\n");
  } catch (const Error&) {
  }
}

}  // namespace

TrackerClient::TrackerClient(Endpoint endpoint, std::string token, std::chrono::milliseconds timeout)
    : endpoint_(std::move(endpoint)), token_(std::move(token)), timeout_(timeout) {
  if (endpoint_.host.empty() || endpoint_.port == 0U || token_.empty() || timeout_.count() <= 0) {
    throw Error("Tracker client requires endpoint, token, and positive timeout");
  }
}

void TrackerClient::announce(const PeerAdvertisement& advertisement) const {
  const auto chunks = encode_chunk_list(advertisement.chunks, advertisement.has_all_chunks);
  const std::string header = "ANNOUNCE " + token_ + " " + advertisement.swarm_id + " " +
                             advertisement.peer_id + " " + std::to_string(advertisement.listen_port) + " " + chunks;
  static_cast<void>(parse_tracker_request(header));

  const auto connection = connect_tcp(endpoint_, timeout_);
  write_all(connection, header + "\n");
  expect_ok(connection);
}

std::vector<DiscoveredPeer> TrackerClient::peers(std::string_view swarm_id) const {
  const std::string header = "PEERS " + token_ + " " + std::string(swarm_id);
  static_cast<void>(parse_tracker_request(header));

  const auto connection = connect_tcp(endpoint_, timeout_);
  write_all(connection, header + "\n");

  std::vector<DiscoveredPeer> discovered;
  while (true) {
    const auto response = read_line(connection);
    if (response == "END") {
      return discovered;
    }
    if (response.starts_with("ERROR ")) {
      throw Error("Tracker rejected peer query: " + response);
    }
    discovered.push_back(parse_peer_response(response));
  }
}

TrackerServer::TrackerServer(std::uint16_t port, std::string token, std::chrono::seconds ttl,
                             std::string bind_address, TrackerLimits limits)
    : requested_port_(port), token_(std::move(token)), ttl_(ttl), bind_address_(std::move(bind_address)),
      limits_(limits) {
  if (token_.empty() || ttl_.count() <= 0 || bind_address_.empty()) {
    throw Error("Tracker server requires a token, positive TTL, and bind address");
  }
  if (limits_.max_connections == 0U || limits_.worker_count == 0U ||
      limits_.worker_count > limits_.max_connections || limits_.max_swarms == 0U ||
      limits_.max_peers_per_swarm == 0U || limits_.max_response_peers == 0U ||
      limits_.max_response_peers > limits_.max_peers_per_swarm) {
    throw Error("Tracker limits must be positive and internally consistent");
  }
}

TrackerServer::~TrackerServer() {
  stop();
}

void TrackerServer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    throw Error("Tracker server is already running");
  }
  try {
    listener_ = listen_tcp(requested_port_, bind_address_);
    endpoint_ = {bind_address_, bound_port(listener_)};
    worker_threads_.reserve(limits_.worker_count);
    for (std::uint32_t index = 0U; index < limits_.worker_count; ++index) {
      worker_threads_.emplace_back(&TrackerServer::worker_loop, this);
    }
    accept_thread_ = std::thread(&TrackerServer::accept_loop, this);
  } catch (...) {
    stop();
    throw;
  }
}

void TrackerServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (listener_.valid()) {
    ::shutdown(listener_.native_handle(), SHUT_RDWR);
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  listener_.close();
  {
    std::lock_guard lock(queue_mutex_);
    const auto pending = static_cast<std::uint32_t>(queue_.size());
    queue_.clear();
    if (pending > 0U) {
      outstanding_connections_.fetch_sub(pending);
    }
  }
  queue_condition_.notify_all();
  for (auto& worker : worker_threads_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  worker_threads_.clear();
  outstanding_connections_.store(0U);
}

Endpoint TrackerServer::endpoint() const {
  if (endpoint_.host.empty() || endpoint_.port == 0U) {
    throw Error("Tracker server has not started");
  }
  return endpoint_;
}

void TrackerServer::accept_loop() {
  while (running_.load()) {
    try {
      Endpoint remote;
      auto client = accept_tcp(listener_, &remote);
      set_socket_timeout(client, std::chrono::seconds(3));
      const auto current = outstanding_connections_.fetch_add(1U);
      if (current >= limits_.max_connections) {
        outstanding_connections_.fetch_sub(1U);
        send_tracker_error(client, "busy");
        continue;
      }

      bool queued = false;
      {
        std::lock_guard lock(queue_mutex_);
        if (running_.load()) {
          queue_.push_back({std::move(client), std::move(remote)});
          queued = true;
        }
      }
      if (queued) {
        queue_condition_.notify_one();
      } else {
        outstanding_connections_.fetch_sub(1U);
      }
    } catch (const Error& error) {
      if (running_.load()) {
        log_error("Tracker accept loop recovered from a connection error");
      }
    }
  }
}

void TrackerServer::worker_loop() {
  while (true) {
    QueuedConnection connection;
    {
      std::unique_lock lock(queue_mutex_);
      queue_condition_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
      if (!running_.load() || queue_.empty()) {
        return;
      }
      connection = std::move(queue_.front());
      queue_.pop_front();
    }
    handle_connection(std::move(connection.client), std::move(connection.remote_endpoint));
    outstanding_connections_.fetch_sub(1U);
  }
}

void TrackerServer::handle_connection(Socket client, Endpoint remote_endpoint) {
  try {
    const auto request = parse_tracker_request(read_line(client));
    if (!secure_token_equal(token_, request.token)) {
      write_all(client, "ERROR unauthorized\n");
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (request.kind == TrackerRequestKind::announce) {
      const PeerRecord record{{request.peer_id, {remote_endpoint.host, request.listen_port}, request.chunks,
                               request.has_all_chunks}, now};
      bool accepted = false;
      {
        std::lock_guard lock(peers_mutex_);
        prune_expired_locked(now);
        auto swarm = swarms_.find(request.swarm_id);
        if (swarm == swarms_.end()) {
          if (swarms_.size() < limits_.max_swarms) {
            swarm = swarms_.emplace(request.swarm_id,
                                    std::unordered_map<std::string, PeerRecord>{}).first;
          }
        }
        if (swarm != swarms_.end()) {
          const auto peer = swarm->second.find(request.peer_id);
          if (peer != swarm->second.end() || swarm->second.size() < limits_.max_peers_per_swarm) {
            swarm->second[request.peer_id] = record;
            accepted = true;
          }
        }
      }
      if (!accepted) {
        write_all(client, "ERROR capacity\n");
        return;
      }
      write_all(client, "OK\n");
      return;
    }

    std::vector<DiscoveredPeer> peers;
    {
      std::lock_guard lock(peers_mutex_);
      prune_expired_locked(now);
      const auto swarm = swarms_.find(request.swarm_id);
      if (swarm != swarms_.end()) {
        peers.reserve(std::min<std::size_t>(swarm->second.size(), limits_.max_response_peers));
        for (const auto& [peer_id, record] : swarm->second) {
          static_cast<void>(peer_id);
          peers.push_back(record.peer);
          if (peers.size() == limits_.max_response_peers) {
            break;
          }
        }
      }
    }
    for (const auto& peer : peers) {
      write_all(client, "PEER " + peer.peer_id + " " + peer.endpoint.host + " " +
                            std::to_string(peer.endpoint.port) + " " +
                            encode_chunk_list(peer.chunks, peer.has_all_chunks) + "\n");
    }
    write_all(client, "END\n");
  } catch (const Error&) {
    try {
      write_all(client, "ERROR bad_request\n");
    } catch (const Error&) {
    }
  }
}

void TrackerServer::prune_expired_locked(std::chrono::steady_clock::time_point now) {
  for (auto swarm = swarms_.begin(); swarm != swarms_.end();) {
    auto& peers = swarm->second;
    for (auto peer = peers.begin(); peer != peers.end();) {
      if (now - peer->second.last_seen >= ttl_) {
        peer = peers.erase(peer);
      } else {
        ++peer;
      }
    }
    if (peers.empty()) {
      swarm = swarms_.erase(swarm);
    } else {
      ++swarm;
    }
  }
}

}  // namespace swarmsync
