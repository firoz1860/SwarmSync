#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace swarmsync {

class ResumeStore {
 public:
  static ResumeStore load_or_create(const std::filesystem::path& path, std::string swarm_id,
                                    std::uint32_t chunk_count);

  ResumeStore(const ResumeStore&) = delete;
  ResumeStore& operator=(const ResumeStore&) = delete;
  ResumeStore(ResumeStore&& other) noexcept;
  ResumeStore& operator=(ResumeStore&& other) noexcept;

  void mark_complete(std::uint32_t index);
  void mark_incomplete(std::uint32_t index);
  [[nodiscard]] bool complete(std::uint32_t index) const;
  [[nodiscard]] bool all_complete() const;
  [[nodiscard]] std::vector<std::uint32_t> completed_chunks() const;
  void flush();

 private:
  ResumeStore(std::filesystem::path path, std::string swarm_id, std::uint32_t chunk_count);

  void load_existing();
  [[nodiscard]] std::string serialize_locked() const;
  void check_index(std::uint32_t index) const;

  std::filesystem::path path_;
  std::string swarm_id_;
  std::uint32_t chunk_count_{};
  mutable std::mutex mutex_;
  std::vector<bool> completed_;
  std::uint64_t revision_{};
  bool dirty_{true};
};

}  // namespace swarmsync
