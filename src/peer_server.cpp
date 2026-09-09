#include "swarmsync/peer_server.hpp"

#include "swarmsync/common.hpp"
#include "swarmsync/crypto.hpp"

#include <algorithm>
#include <fstream>
#include <span>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace swarmsync {
namespace {

void send_error(const Socket& client, std::string_view code) noexcept {
  try {
    write_all(client, "ERROR " + std::string(code) + "\n");
  } catch (const Error&) {
  }
}

}  // namespace

PeerServer::PeerServer(PeerServerConfig config) : config_(std::move(config)) {
  config_.manifest.validate();
  if (config_.file_path.empty() || config_.token.empty() || config_.bind_address.empty() ||
      config_.max_connections == 0U) {
    throw Error("Peer server requires file, token, bind address, and connection limit");
  }
}

PeerServer::~PeerServer() {
  stop();
}

void PeerServer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    throw Error("Peer server is already running");
  }
  try {
    listener_ = listen_tcp(config_.port, config_.bind_address);
    endpoint_ = {config_.bind_address, bound_port(listener_)};
    const auto worker_count = std::min<std::uint32_t>(config_.max_connections, 4U);
    worker_threads_.reserve(worker_count);
    for (std::uint32_t index = 0; index < worker_count; ++index) {
      worker_threads_.emplace_back(&PeerServer::worker_loop, this);
    }
    accept_thread_ = std::thread(&PeerServer::accept_loop, this);
  } catch (...) {
    stop();
    throw;
  }
}

void PeerServer::stop() {
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
}

Endpoint PeerServer::endpoint() const {
  if (endpoint_.host.empty() || endpoint_.port == 0U) {
    throw Error("Peer server has not started");
  }
  return endpoint_;
}

void PeerServer::accept_loop() {
  while (running_.load()) {
    try {
      auto client = accept_tcp(listener_);
      set_socket_timeout(client, std::chrono::seconds(3));
      const auto current = outstanding_connections_.fetch_add(1U);
      if (current >= config_.max_connections) {
        outstanding_connections_.fetch_sub(1U);
        send_error(client, "busy");
        continue;
      }

      bool accepted = false;
      {
        std::lock_guard lock(queue_mutex_);
        if (running_.load()) {
          queue_.push_back(std::move(client));
          accepted = true;
        }
      }
      if (accepted) {
        queue_condition_.notify_one();
      } else {
        outstanding_connections_.fetch_sub(1U);
      }
    } catch (const Error&) {
      if (running_.load()) {
        log_error("Peer server recovered from a connection error");
      }
    }
  }
}

void PeerServer::worker_loop() {
  while (true) {
    Socket client;
    {
      std::unique_lock lock(queue_mutex_);
      queue_condition_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
      if (!running_.load() || queue_.empty()) {
        return;
      }
      client = std::move(queue_.front());
      queue_.pop_front();
    }
    serve_connection(std::move(client));
    outstanding_connections_.fetch_sub(1U);
  }
}

void PeerServer::serve_connection(Socket client) {
  try {
    const auto request = parse_chunk_request(read_line(client));
    if (!secure_token_equal(config_.token, request.token)) {
      send_error(client, "unauthorized");
      return;
    }
    if (request.swarm_id != config_.manifest.swarm_id()) {
      send_error(client, "wrong_swarm");
      return;
    }
    if (request.chunk_index >= config_.manifest.chunk_hashes.size() || !has_chunk(request.chunk_index)) {
      send_error(client, "unavailable");
      return;
    }

    const auto expected_size = config_.manifest.chunk_size(request.chunk_index);
    std::ifstream input(config_.file_path, std::ios::binary);
    if (!input) {
      send_error(client, "io_failure");
      return;
    }
    input.seekg(static_cast<std::streamoff>(config_.manifest.chunk_offset(request.chunk_index)));
    std::vector<char> bytes(expected_size);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        sha256_hex(std::string_view(bytes.data(), bytes.size())) != config_.manifest.chunk_hashes[request.chunk_index]) {
      send_error(client, "corrupt");
      return;
    }

    write_all(client, "DATA " + std::to_string(bytes.size()) + " " +
                          config_.manifest.chunk_hashes[request.chunk_index] + "\n");
    write_all(client, std::as_bytes(std::span(bytes.data(), bytes.size())));
  } catch (const Error&) {
    send_error(client, "bad_request");
  }
}

bool PeerServer::has_chunk(std::uint32_t index) const {
  return !config_.has_chunk || config_.has_chunk(index);
}

}  // namespace swarmsync
