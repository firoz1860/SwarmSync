#include "swarmsync/download.hpp"

#include "swarmsync/common.hpp"
#include "swarmsync/crypto.hpp"
#include "swarmsync/peer_server.hpp"
#include "swarmsync/resume_store.hpp"
#include "swarmsync/tracker.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace swarmsync {
namespace {

void validate_config(const DownloadConfig& config) {
  config.manifest.validate();
  if (config.output_directory.empty() || config.tracker_endpoint.host.empty() || config.tracker_endpoint.port == 0U ||
      config.token.empty() || config.peer_id.empty() || config.worker_count == 0U || config.max_retries == 0U ||
      config.network_timeout.count() <= 0 || config.heartbeat_interval.count() <= 0 || config.bind_address.empty() ||
      config.peer_max_connections == 0U) {
    throw Error("Download configuration contains an invalid required value");
  }
}

void prepare_partial_file(const std::filesystem::path& path, std::uint64_t expected_size) {
  if (std::filesystem::exists(path)) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != expected_size) {
      throw Error("Existing partial file does not match manifest size: " + path.string());
    }
    return;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw Error("Unable to create partial file: " + path.string());
  }
  if (expected_size > 0U) {
    const char zero = 0;
    output.seekp(static_cast<std::streamoff>(expected_size - 1U));
    output.write(&zero, 1);
  }
  if (!output) {
    throw Error("Unable to size partial file: " + path.string());
  }
}

bool peer_has_chunk(const DiscoveredPeer& peer, std::uint32_t index) {
  return peer.has_all_chunks ||
         std::binary_search(peer.chunks.begin(), peer.chunks.end(), index);
}

std::vector<std::byte> fetch_chunk(const DiscoveredPeer& peer, const Manifest& manifest,
                                   std::string_view token, std::uint32_t index,
                                   std::chrono::milliseconds timeout) {
  const auto connection = connect_tcp(peer.endpoint, timeout);
  write_all(connection, "GET " + std::string(token) + " " + manifest.swarm_id() + " " +
                            std::to_string(index) + "\n");
  const auto response = read_line(connection);
  if (response.starts_with("ERROR ")) {
    throw Error("Peer declined requested chunk");
  }
  const auto header = parse_data_header(response);
  if (header.byte_count != manifest.chunk_size(index) || header.sha256 != manifest.chunk_hashes[index]) {
    throw Error("Peer returned chunk metadata that disagrees with the manifest");
  }
  std::vector<std::byte> bytes(header.byte_count);
  read_exact(connection, bytes);
  if (sha256_hex(std::span<const std::byte>(bytes.data(), bytes.size())) != manifest.chunk_hashes[index]) {
    throw Error("Peer returned bytes that failed chunk integrity verification");
  }
  return bytes;
}

std::vector<std::byte> fetch_with_retry(const TrackerClient& tracker, const DownloadConfig& config,
                                        std::string_view swarm_id, std::uint32_t index,
                                        std::atomic_size_t& retry_events) {
  std::string last_failure = "no active peer advertised this chunk";
  for (std::uint32_t round = 0; round < config.max_retries; ++round) {
    try {
      auto peers = tracker.peers(swarm_id);
      peers.erase(std::remove_if(peers.begin(), peers.end(), [&](const DiscoveredPeer& peer) {
                    return peer.peer_id == config.peer_id || !peer_has_chunk(peer, index);
                  }),
                  peers.end());
      for (const auto& peer : peers) {
        try {
          return fetch_chunk(peer, config.manifest, config.token, index, config.network_timeout);
        } catch (const Error& error) {
          last_failure = error.what();
        }
      }
    } catch (const Error& error) {
      last_failure = error.what();
    }
    if (round + 1U < config.max_retries) {
      retry_events.fetch_add(1U);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  throw Error("Unable to fetch chunk " + std::to_string(index) + ": " + last_failure);
}

bool partial_chunk_matches(std::ifstream& input, const Manifest& manifest, std::uint32_t index) {
  const auto offset = manifest.chunk_offset(index);
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    return false;
  }
  std::vector<std::byte> bytes(manifest.chunk_size(index));
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return input.gcount() == static_cast<std::streamsize>(bytes.size()) &&
         sha256_hex(std::span<const std::byte>(bytes.data(), bytes.size())) == manifest.chunk_hashes[index];
}

void revalidate_completed_chunks(const std::filesystem::path& partial_path, const Manifest& manifest,
                                 ResumeStore& resume) {
  const auto completed = resume.completed_chunks();
  if (completed.empty()) {
    return;
  }
  std::ifstream input(partial_path, std::ios::binary);
  if (!input) {
    throw Error("Unable to read the partial file while revalidating resume state");
  }

  bool changed = false;
  for (const auto index : completed) {
    if (!partial_chunk_matches(input, manifest, index)) {
      resume.mark_incomplete(index);
      changed = true;
    }
  }
  if (changed) {
    resume.flush();
  }
}

void write_verified_chunk(const std::filesystem::path& partial_path, const Manifest& manifest,
                          std::uint32_t index, std::span<const std::byte> bytes,
                          ResumeStore& resume, std::mutex& write_mutex) {
  std::lock_guard lock(write_mutex);
  const auto offset = manifest.chunk_offset(index);
  const auto maximum_offset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (offset > maximum_offset || bytes.size() > maximum_offset - offset) {
    throw Error("Verified chunk offset is outside the platform file range");
  }

  int flags = O_WRONLY;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  int fd = ::open(partial_path.c_str(), flags);
  if (fd < 0) {
    throw Error("Unable to open partial file for a verified chunk write");
  }

  try {
    std::size_t written = 0U;
    while (written < bytes.size()) {
      const auto result = ::pwrite(fd, bytes.data() + written, bytes.size() - written,
                                   static_cast<off_t>(offset + written));
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw Error("Unable to write verified chunk: " + std::string(std::strerror(errno)));
      }
      if (result == 0) {
        throw Error("Unable to write verified chunk: zero-byte write");
      }
      written += static_cast<std::size_t>(result);
    }
    if (::fsync(fd) < 0) {
      throw Error("Unable to durably flush verified chunk: " + std::string(std::strerror(errno)));
    }
    const int closing_fd = fd;
    fd = -1;
    if (::close(closing_fd) < 0) {
      throw Error("Unable to close partial file after a verified chunk write: " +
                  std::string(std::strerror(errno)));
    }
  } catch (...) {
    if (fd >= 0) {
      ::close(fd);
    }
    throw;
  }

  resume.mark_complete(index);
  resume.flush();
}

}  // namespace

DownloadCoordinator::DownloadCoordinator(DownloadConfig config) : config_(std::move(config)) {
  validate_config(config_);
}

DownloadResult DownloadCoordinator::run() {
  const auto started = std::chrono::steady_clock::now();
  const auto swarm_id = config_.manifest.swarm_id();
  std::filesystem::create_directories(config_.output_directory);
  const auto final_path = config_.output_directory / config_.manifest.file_name;
  const auto partial_path = config_.output_directory / (config_.manifest.file_name + ".part");
  const auto resume_path = config_.output_directory / (swarm_id + ".resume");

  if (std::filesystem::exists(final_path)) {
    if (sha256_file(final_path) != config_.manifest.file_sha256) {
      throw Error("Existing final file does not match the selected manifest");
    }
    return {final_path, config_.manifest.chunk_hashes.size(), 0U,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)};
  }
  if (std::filesystem::exists(resume_path) && !std::filesystem::exists(partial_path)) {
    throw Error("Resume state exists but its partial file is missing");
  }
  prepare_partial_file(partial_path, config_.manifest.file_size_bytes);
  auto resume = ResumeStore::load_or_create(resume_path, swarm_id,
                                              static_cast<std::uint32_t>(config_.manifest.chunk_hashes.size()));
  revalidate_completed_chunks(partial_path, config_.manifest, resume);

  PeerServer peer_server({config_.manifest, partial_path, config_.token, config_.listen_port,
                          config_.bind_address, config_.peer_max_connections,
                          [&resume](std::uint32_t index) { return resume.complete(index); }});
  peer_server.start();

  std::atomic_bool heartbeat_running{false};
  std::mutex heartbeat_mutex;
  std::condition_variable heartbeat_condition;
  std::thread heartbeat_thread;
  auto stop_heartbeat = [&] {
    heartbeat_running.store(false);
    heartbeat_condition.notify_all();
    if (heartbeat_thread.joinable()) {
      heartbeat_thread.join();
    }
  };

  try {
    const TrackerClient tracker(config_.tracker_endpoint, config_.token, config_.network_timeout);
    const auto announce_current = [&] {
      tracker.announce({swarm_id, config_.peer_id, peer_server.endpoint().port,
                        resume.completed_chunks(), resume.all_complete()});
    };
    announce_current();

    heartbeat_running.store(true);
    heartbeat_thread = std::thread([&] {
      std::unique_lock lock(heartbeat_mutex);
      while (heartbeat_running.load()) {
        if (heartbeat_condition.wait_for(lock, config_.heartbeat_interval,
                                         [&] { return !heartbeat_running.load(); })) {
          break;
        }
        lock.unlock();
        try {
          announce_current();
        } catch (const Error&) {
          log_error("Downloader heartbeat could not reach the tracker");
        }
        lock.lock();
      }
    });

    std::atomic_uint32_t next_index{0U};
    std::atomic_bool failed{false};
    std::atomic_size_t retry_events{0U};
    std::mutex write_mutex;
    std::mutex failure_mutex;
    std::exception_ptr failure;
    std::vector<std::thread> workers;
    workers.reserve(config_.worker_count);
    for (std::uint32_t worker_index = 0; worker_index < config_.worker_count; ++worker_index) {
      workers.emplace_back([&] {
        while (!failed.load()) {
          const auto index = next_index.fetch_add(1U);
          if (index >= config_.manifest.chunk_hashes.size()) {
            return;
          }
          if (resume.complete(index)) {
            continue;
          }
          try {
            const auto bytes = fetch_with_retry(tracker, config_, swarm_id, index, retry_events);
            write_verified_chunk(partial_path, config_.manifest, index, bytes, resume, write_mutex);
            try {
              announce_current();
            } catch (const Error&) {
              log_error("Downloader deferred a chunk availability announcement");
            }
          } catch (...) {
            {
              std::lock_guard lock(failure_mutex);
              if (failure == nullptr) {
                failure = std::current_exception();
              }
            }
            failed.store(true);
            return;
          }
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
    if (!resume.all_complete()) {
      throw Error("Download ended before every manifest chunk was verified");
    }

    stop_heartbeat();
    peer_server.stop();
    resume.flush();
    if (sha256_file(partial_path) != config_.manifest.file_sha256) {
      throw Error("Final full-file SHA-256 verification failed");
    }
    std::filesystem::rename(partial_path, final_path);
    std::error_code remove_error;
    std::filesystem::remove(resume_path, remove_error);
    if (remove_error) {
      log_error("Completed download left a stale resume state for later cleanup");
    }
    return {final_path, config_.manifest.chunk_hashes.size(), retry_events.load(),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)};
  } catch (...) {
    stop_heartbeat();
    peer_server.stop();
    try {
      resume.flush();
    } catch (const Error&) {
      log_error("Download could not flush resume state during error handling");
    }
    throw;
  }
}

}  // namespace swarmsync
