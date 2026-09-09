#include "swarmsync/protocol.hpp"

#include "swarmsync/common.hpp"
#include "swarmsync/crypto.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace swarmsync {
namespace {

[[noreturn]] void throw_socket_error(std::string_view action) {
  throw Error(std::string(action) + ": " + std::strerror(errno));
}

void close_fd(int fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
  }
}

void set_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    throw_socket_error("Unable to set close-on-exec");
  }
}

void set_non_blocking(int fd, bool enabled) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0) {
    throw_socket_error("Unable to inspect socket flags");
  }
  const int next_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd, F_SETFL, next_flags) < 0) {
    throw_socket_error("Unable to update socket flags");
  }
}

void suppress_sigpipe(const Socket& socket) {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
  const int enabled = 1;
  if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
    throw_socket_error("Unable to suppress SIGPIPE on TCP socket");
  }
#else
  static_cast<void>(socket);
#endif
}

void wait_for_connect(int fd, std::chrono::milliseconds timeout) {
  pollfd descriptor{fd, POLLOUT, 0};
  int result = 0;
  do {
    result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  } while (result < 0 && errno == EINTR);
  if (result == 0) {
    throw Error("TCP connection timed out");
  }
  if (result < 0) {
    throw_socket_error("Unable to poll connecting socket");
  }
  int socket_error = 0;
  socklen_t size = sizeof(socket_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) < 0) {
    throw_socket_error("Unable to read connection result");
  }
  if (socket_error != 0) {
    errno = socket_error;
    throw_socket_error("TCP connection failed");
  }
}

std::vector<std::string_view> split_header(std::string_view header) {
  if (header.empty() || header.size() > kMaxHeaderBytes) {
    throw Error("Protocol header has an invalid length");
  }
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (std::size_t index = 0; index < header.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(header[index]);
    if (character < 32U || character > 126U) {
      throw Error("Protocol header contains a control character");
    }
    if (header[index] == ' ') {
      if (index == start) {
        throw Error("Protocol header contains an empty field");
      }
      fields.push_back(header.substr(start, index - start));
      start = index + 1U;
    }
  }
  if (start == header.size()) {
    throw Error("Protocol header ends with a separator");
  }
  fields.push_back(header.substr(start));
  return fields;
}

bool safe_atom(std::string_view value, std::size_t maximum = 128U) {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) ||
           (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) ||
           (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9')) ||
           character == static_cast<unsigned char>('-') || character == static_cast<unsigned char>('_') ||
           character == static_cast<unsigned char>('.') || character == static_cast<unsigned char>(':');
  });
}

void require_atom(std::string_view value, std::string_view name) {
  if (!safe_atom(value)) {
    throw Error("Protocol " + std::string(name) + " contains invalid characters");
  }
}

template <typename Number>
Number parse_number(std::string_view value, std::string_view name) {
  if (value.empty()) {
    throw Error("Protocol " + std::string(name) + " is empty");
  }
  Number result{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw Error("Protocol " + std::string(name) + " is not an unsigned integer");
  }
  return result;
}

std::vector<std::uint32_t> parse_chunk_list(std::string_view value) {
  if (value == "all" || value == "none") {
    return {};
  }
  std::vector<std::uint32_t> chunks;
  std::size_t start = 0;
  while (start < value.size()) {
    const auto comma = value.find(',', start);
    const auto field = value.substr(start, comma == std::string_view::npos ? value.size() - start : comma - start);
    const auto chunk = parse_number<std::uint32_t>(field, "chunk list entry");
    if (!chunks.empty() && chunk <= chunks.back()) {
      throw Error("Protocol chunk advertisements must be sorted and unique");
    }
    chunks.push_back(chunk);
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1U;
    if (start == value.size()) {
      throw Error("Protocol chunk list has an empty entry");
    }
  }
  if (chunks.empty()) {
    throw Error("Protocol chunk list cannot be empty");
  }
  return chunks;
}

Endpoint endpoint_from_address(const sockaddr_storage& address, socklen_t size) {
  std::array<char, NI_MAXHOST> host{};
  std::array<char, NI_MAXSERV> service{};
  const int result = ::getnameinfo(reinterpret_cast<const sockaddr*>(&address), size,
                                   host.data(), host.size(), service.data(), service.size(),
                                   NI_NUMERICHOST | NI_NUMERICSERV);
  if (result != 0) {
    throw Error("Unable to read peer address: " + std::string(::gai_strerror(result)));
  }
  return {host.data(), parse_number<std::uint16_t>(service.data(), "remote port")};
}

}  // namespace

Socket::Socket(int fd) noexcept : fd_(fd) {}

Socket::~Socket() {
  close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.release();
  }
  return *this;
}

bool Socket::valid() const noexcept {
  return fd_ >= 0;
}

int Socket::native_handle() const noexcept {
  return fd_;
}

int Socket::release() noexcept {
  const int released = fd_;
  fd_ = -1;
  return released;
}

void Socket::close() noexcept {
  close_fd(release());
}

Socket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout) {
  if (endpoint.host.empty() || endpoint.port == 0U || timeout.count() <= 0) {
    throw Error("TCP endpoint and timeout must be valid");
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const auto port = std::to_string(endpoint.port);
  const int lookup = ::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results);
  if (lookup != 0) {
    throw Error("Unable to resolve TCP endpoint: " + std::string(::gai_strerror(lookup)));
  }

  std::string last_error = "No reachable address";
  for (addrinfo* item = results; item != nullptr; item = item->ai_next) {
    int fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    try {
      set_close_on_exec(fd);
      set_non_blocking(fd, true);
      const int connected = ::connect(fd, item->ai_addr, item->ai_addrlen);
      if (connected < 0 && errno != EINPROGRESS) {
        last_error = std::strerror(errno);
        close_fd(fd);
        continue;
      }
      if (connected < 0) {
        wait_for_connect(fd, timeout);
      }
      set_non_blocking(fd, false);
      Socket socket(fd);
      set_socket_timeout(socket, timeout);
      ::freeaddrinfo(results);
      return socket;
    } catch (const Error& error) {
      last_error = error.what();
      close_fd(fd);
    }
  }
  ::freeaddrinfo(results);
  throw Error("Unable to connect to " + endpoint.host + ":" + std::to_string(endpoint.port) + ": " + last_error);
}

Socket listen_tcp(std::uint16_t port, std::string_view bind_address, int backlog) {
  if (backlog <= 0) {
    throw Error("TCP listen backlog must be positive");
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string address(bind_address);
  const auto port_text = std::to_string(port);
  const int lookup = ::getaddrinfo(address.empty() ? nullptr : address.c_str(), port_text.c_str(), &hints, &results);
  if (lookup != 0) {
    throw Error("Unable to resolve listen address: " + std::string(::gai_strerror(lookup)));
  }

  std::string last_error = "No bindable address";
  for (addrinfo* item = results; item != nullptr; item = item->ai_next) {
    int fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    try {
      set_close_on_exec(fd);
      int reuse = 1;
      if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw_socket_error("Unable to set SO_REUSEADDR");
      }
      if (::bind(fd, item->ai_addr, item->ai_addrlen) < 0) {
        throw_socket_error("Unable to bind TCP listener");
      }
      if (::listen(fd, backlog) < 0) {
        throw_socket_error("Unable to listen on TCP socket");
      }
      ::freeaddrinfo(results);
      return Socket(fd);
    } catch (const Error& error) {
      last_error = error.what();
      close_fd(fd);
    }
  }
  ::freeaddrinfo(results);
  throw Error("Unable to listen on " + address + ":" + std::to_string(port) + ": " + last_error);
}

Socket accept_tcp(const Socket& listener, Endpoint* remote_endpoint) {
  sockaddr_storage address{};
  socklen_t size = sizeof(address);
  int accepted = -1;
  do {
    accepted = ::accept(listener.native_handle(), reinterpret_cast<sockaddr*>(&address), &size);
  } while (accepted < 0 && errno == EINTR);
  if (accepted < 0) {
    throw_socket_error("Unable to accept TCP connection");
  }
  try {
    set_close_on_exec(accepted);
    if (remote_endpoint != nullptr) {
      *remote_endpoint = endpoint_from_address(address, size);
    }
    return Socket(accepted);
  } catch (...) {
    close_fd(accepted);
    throw;
  }
}

std::uint16_t bound_port(const Socket& socket) {
  sockaddr_storage address{};
  socklen_t size = sizeof(address);
  if (::getsockname(socket.native_handle(), reinterpret_cast<sockaddr*>(&address), &size) < 0) {
    throw_socket_error("Unable to read TCP bound port");
  }
  if (address.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
  }
  if (address.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
  }
  throw Error("TCP socket has an unsupported address family");
}

void set_socket_timeout(const Socket& socket, std::chrono::milliseconds timeout) {
  if (!socket.valid() || timeout.count() <= 0) {
    throw Error("Socket timeout requires an open socket and positive duration");
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const auto remainder = timeout - seconds;
  timeval value{};
  value.tv_sec = seconds.count();
  value.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(remainder).count();
  if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) < 0 ||
      ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) < 0) {
    throw_socket_error("Unable to configure socket timeout");
  }
}

std::string read_line(const Socket& socket, std::size_t max_bytes) {
  if (!socket.valid() || max_bytes == 0U || max_bytes > kMaxHeaderBytes) {
    throw Error("Invalid bounded line read request");
  }
  std::string result;
  result.reserve(std::min<std::size_t>(max_bytes, 256U));
  while (true) {
    char character{};
    const auto read = ::recv(socket.native_handle(), &character, 1, 0);
    if (read == 0) {
      throw Error("Peer closed connection before completing a header");
    }
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_socket_error("Unable to read TCP header");
    }
    if (character == '\n') {
      return result;
    }
    if (character == '\r') {
      continue;
    }
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 32U || byte > 126U || result.size() >= max_bytes) {
      throw Error("TCP header is invalid or exceeds its limit");
    }
    result.push_back(character);
  }
}

void read_exact(const Socket& socket, std::span<std::byte> output) {
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto read = ::recv(socket.native_handle(), output.data() + offset, output.size() - offset, 0);
    if (read == 0) {
      throw Error("Peer closed connection before sending requested bytes");
    }
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_socket_error("Unable to read TCP bytes");
    }
    offset += static_cast<std::size_t>(read);
  }
}

void write_all(const Socket& socket, std::span<const std::byte> data) {
  suppress_sigpipe(socket);
  std::size_t offset = 0;
  while (offset < data.size()) {
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto written = ::send(socket.native_handle(), data.data() + offset, data.size() - offset, flags);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_socket_error("Unable to write TCP bytes");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void write_all(const Socket& socket, std::string_view data) {
  write_all(socket, std::as_bytes(std::span(data.data(), data.size())));
}

TrackerRequest parse_tracker_request(std::string_view header) {
  const auto fields = split_header(header);
  if (fields.front() == "ANNOUNCE") {
    if (fields.size() != 6U) {
      throw Error("ANNOUNCE requires token, swarm, peer, port, and chunks");
    }
    require_atom(fields[1], "token");
    require_atom(fields[2], "swarm id");
    require_atom(fields[3], "peer id");
    const auto port = parse_number<std::uint32_t>(fields[4], "listen port");
    if (port == 0U || port > 65535U) {
      throw Error("Protocol listen port is outside range");
    }
    TrackerRequest request;
    request.kind = TrackerRequestKind::announce;
    request.token = std::string(fields[1]);
    request.swarm_id = std::string(fields[2]);
    request.peer_id = std::string(fields[3]);
    request.listen_port = static_cast<std::uint16_t>(port);
    request.has_all_chunks = fields[5] == "all";
    request.chunks = parse_chunk_list(fields[5]);
    return request;
  }
  if (fields.front() == "PEERS") {
    if (fields.size() != 3U) {
      throw Error("PEERS requires token and swarm id");
    }
    require_atom(fields[1], "token");
    require_atom(fields[2], "swarm id");
    return {TrackerRequestKind::peers, std::string(fields[1]), std::string(fields[2]), {}, 0U, false, {}};
  }
  throw Error("Unsupported tracker command");
}

ChunkRequest parse_chunk_request(std::string_view header) {
  const auto fields = split_header(header);
  if (fields.size() != 4U || fields.front() != "GET") {
    throw Error("GET requires token, swarm id, and chunk index");
  }
  require_atom(fields[1], "token");
  require_atom(fields[2], "swarm id");
  return {std::string(fields[1]), std::string(fields[2]), parse_number<std::uint32_t>(fields[3], "chunk index")};
}

DataHeader parse_data_header(std::string_view header) {
  const auto fields = split_header(header);
  if (fields.size() != 3U || fields.front() != "DATA") {
    throw Error("DATA requires byte count and SHA-256 digest");
  }
  const auto byte_count = parse_number<std::uint32_t>(fields[1], "byte count");
  if (byte_count > kMaxChunkBytes || !is_sha256_hex(fields[2])) {
    throw Error("DATA header is outside protocol limits");
  }
  return {byte_count, std::string(fields[2])};
}

std::string encode_chunk_list(const std::vector<std::uint32_t>& chunks, bool has_all_chunks) {
  if (has_all_chunks) {
    return "all";
  }
  if (chunks.empty()) {
    return "none";
  }
  std::ostringstream output;
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    if (index > 0U && chunks[index] <= chunks[index - 1U]) {
      throw Error("Chunk list must be sorted and unique");
    }
    output << chunks[index];
  }
  return output.str();
}

}  // namespace swarmsync
