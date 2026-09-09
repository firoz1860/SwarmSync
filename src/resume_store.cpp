#include "swarmsync/resume_store.hpp"

#include "swarmsync/common.hpp"

#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace swarmsync {
namespace {

template <typename Number>
Number parse_unsigned(std::string_view value, std::string_view field) {
  if (value.empty()) {
    throw Error("Resume field is empty: " + std::string(field));
  }
  Number parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw Error("Resume field is not an unsigned integer: " + std::string(field));
  }
  return parsed;
}

std::string read_required_line(std::istream& input, const std::filesystem::path& path) {
  std::string line;
  if (!std::getline(input, line)) {
    throw Error("Unexpected end of resume state: " + path.string());
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::string require_field(std::string_view line, std::string_view field) {
  const std::string prefix = std::string(field) + "=";
  if (!line.starts_with(prefix)) {
    throw Error("Expected resume field: " + std::string(field));
  }
  return std::string(line.substr(prefix.size()));
}

void write_atomic(const std::filesystem::path& path, std::string_view content) {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  const auto temporary = path.string() + ".tmp";
  int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    throw Error("Unable to open temporary resume state: " + std::string(std::strerror(errno)));
  }

  try {
    std::size_t offset = 0;
    while (offset < content.size()) {
      const auto written = ::write(fd, content.data() + offset, content.size() - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw Error("Unable to write temporary resume state: " + std::string(std::strerror(errno)));
      }
      offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) < 0) {
      throw Error("Unable to flush temporary resume state: " + std::string(std::strerror(errno)));
    }
    const int closing_fd = fd;
    fd = -1;
    if (::close(closing_fd) < 0) {
      throw Error("Unable to close temporary resume state: " + std::string(std::strerror(errno)));
    }
    if (::rename(temporary.c_str(), path.c_str()) < 0) {
      throw Error("Unable to publish resume state: " + std::string(std::strerror(errno)));
    }
  } catch (...) {
    if (fd >= 0) {
      ::close(fd);
    }
    ::unlink(temporary.c_str());
    throw;
  }
}

}  // namespace

ResumeStore::ResumeStore(std::filesystem::path path, std::string swarm_id, std::uint32_t chunk_count)
    : path_(std::move(path)), swarm_id_(std::move(swarm_id)), chunk_count_(chunk_count),
      completed_(chunk_count, false) {
  if (path_.empty() || swarm_id_.empty()) {
    throw Error("Resume state requires a path and swarm identifier");
  }
}

ResumeStore ResumeStore::load_or_create(const std::filesystem::path& path, std::string swarm_id,
                                        std::uint32_t chunk_count) {
  ResumeStore store(path, std::move(swarm_id), chunk_count);
  if (std::filesystem::exists(path)) {
    store.load_existing();
    store.dirty_ = false;
  }
  return store;
}

ResumeStore::ResumeStore(ResumeStore&& other) noexcept {
  std::lock_guard lock(other.mutex_);
  path_ = std::move(other.path_);
  swarm_id_ = std::move(other.swarm_id_);
  chunk_count_ = other.chunk_count_;
  completed_ = std::move(other.completed_);
  revision_ = other.revision_;
  dirty_ = other.dirty_;
}

ResumeStore& ResumeStore::operator=(ResumeStore&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    path_ = std::move(other.path_);
    swarm_id_ = std::move(other.swarm_id_);
    chunk_count_ = other.chunk_count_;
    completed_ = std::move(other.completed_);
    revision_ = other.revision_;
    dirty_ = other.dirty_;
  }
  return *this;
}

void ResumeStore::mark_complete(std::uint32_t index) {
  std::lock_guard lock(mutex_);
  check_index(index);
  if (!completed_[index]) {
    completed_[index] = true;
    ++revision_;
    dirty_ = true;
  }
}

void ResumeStore::mark_incomplete(std::uint32_t index) {
  std::lock_guard lock(mutex_);
  check_index(index);
  if (completed_[index]) {
    completed_[index] = false;
    ++revision_;
    dirty_ = true;
  }
}

bool ResumeStore::complete(std::uint32_t index) const {
  std::lock_guard lock(mutex_);
  check_index(index);
  return completed_[index];
}

bool ResumeStore::all_complete() const {
  std::lock_guard lock(mutex_);
  for (const bool complete : completed_) {
    if (!complete) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint32_t> ResumeStore::completed_chunks() const {
  std::lock_guard lock(mutex_);
  std::vector<std::uint32_t> result;
  for (std::uint32_t index = 0; index < chunk_count_; ++index) {
    if (completed_[index]) {
      result.push_back(index);
    }
  }
  return result;
}

void ResumeStore::flush() {
  std::string snapshot;
  std::uint64_t revision = 0;
  {
    std::lock_guard lock(mutex_);
    if (!dirty_) {
      return;
    }
    snapshot = serialize_locked();
    revision = revision_;
  }
  write_atomic(path_, snapshot);
  std::lock_guard lock(mutex_);
  if (revision_ == revision) {
    dirty_ = false;
  }
}

void ResumeStore::load_existing() {
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    throw Error("Unable to open resume state: " + path_.string());
  }
  if (read_required_line(input, path_) != "SWARMSYNC_RESUME_V1") {
    throw Error("Unsupported resume state format");
  }
  const auto stored_swarm = require_field(read_required_line(input, path_), "swarm_id");
  const auto stored_count = parse_unsigned<std::uint32_t>(
      require_field(read_required_line(input, path_), "chunk_count"), "chunk_count");
  const auto completed = require_field(read_required_line(input, path_), "complete");
  std::string trailing;
  if (std::getline(input, trailing)) {
    throw Error("Resume state contains unexpected trailing data");
  }
  if (stored_swarm != swarm_id_ || stored_count != chunk_count_) {
    throw Error("Resume state does not match the selected manifest");
  }
  if (completed == "none") {
    return;
  }
  std::size_t start = 0;
  std::uint32_t previous = 0;
  bool have_previous = false;
  while (start < completed.size()) {
    const auto comma = completed.find(',', start);
    const auto value = std::string_view(completed).substr(
        start, comma == std::string::npos ? completed.size() - start : comma - start);
    const auto index = parse_unsigned<std::uint32_t>(value, "completed chunk");
    if (index >= chunk_count_ || (have_previous && index <= previous)) {
      throw Error("Resume state contains an invalid completed chunk list");
    }
    completed_[index] = true;
    previous = index;
    have_previous = true;
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1U;
    if (start == completed.size()) {
      throw Error("Resume state contains an empty completed chunk entry");
    }
  }
}

std::string ResumeStore::serialize_locked() const {
  std::ostringstream output;
  output << "SWARMSYNC_RESUME_V1\n";
  output << "swarm_id=" << swarm_id_ << '\n';
  output << "chunk_count=" << chunk_count_ << '\n';
  output << "complete=";
  bool first = true;
  for (std::uint32_t index = 0; index < chunk_count_; ++index) {
    if (completed_[index]) {
      if (!first) {
        output << ',';
      }
      output << index;
      first = false;
    }
  }
  if (first) {
    output << "none";
  }
  output << '\n';
  return output.str();
}

void ResumeStore::check_index(std::uint32_t index) const {
  if (index >= chunk_count_) {
    throw Error("Resume chunk index is outside the manifest");
  }
}

}  // namespace swarmsync
