#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swarmsync {

constexpr std::size_t kMaxHeaderBytes = 8192U;
constexpr std::uint32_t kMaxChunkBytes = 4U * 1024U * 1024U;

struct Endpoint {
  std::string host;
  std::uint16_t port{};
};

class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) noexcept;
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] int native_handle() const noexcept;
  int release() noexcept;
  void close() noexcept;

 private:
  int fd_{-1};
};

enum class TrackerRequestKind { announce, peers };

struct TrackerRequest {
  TrackerRequestKind kind{};
  std::string token;
  std::string swarm_id;
  std::string peer_id;
  std::uint16_t listen_port{};
  bool has_all_chunks{};
  std::vector<std::uint32_t> chunks;
};

struct ChunkRequest {
  std::string token;
  std::string swarm_id;
  std::uint32_t chunk_index{};
};

struct DataHeader {
  std::uint32_t byte_count{};
  std::string sha256;
};

Socket connect_tcp(const Endpoint& endpoint,
                   std::chrono::milliseconds timeout = std::chrono::seconds(5));
Socket listen_tcp(std::uint16_t port, std::string_view bind_address = "127.0.0.1", int backlog = 64);
Socket accept_tcp(const Socket& listener, Endpoint* remote_endpoint = nullptr);
std::uint16_t bound_port(const Socket& socket);
void set_socket_timeout(const Socket& socket, std::chrono::milliseconds timeout);
std::string read_line(const Socket& socket, std::size_t max_bytes = kMaxHeaderBytes);
void read_exact(const Socket& socket, std::span<std::byte> output);
void write_all(const Socket& socket, std::span<const std::byte> data);
void write_all(const Socket& socket, std::string_view data);

TrackerRequest parse_tracker_request(std::string_view header);
ChunkRequest parse_chunk_request(std::string_view header);
DataHeader parse_data_header(std::string_view header);
std::string encode_chunk_list(const std::vector<std::uint32_t>& chunks, bool has_all_chunks);

}  // namespace swarmsync
