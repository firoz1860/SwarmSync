#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace swarmsync {

struct Manifest {
  std::string file_name;
  std::uint64_t file_size_bytes{};
  std::uint32_t chunk_size_bytes{};
  std::string file_sha256;
  std::vector<std::string> chunk_hashes;

  static Manifest from_file(const std::filesystem::path& path, std::uint32_t chunk_size_bytes);
  static Manifest load(const std::filesystem::path& path);

  void save(const std::filesystem::path& path) const;
  void validate() const;
  std::string serialize() const;
  std::string swarm_id() const;
  std::uint32_t chunk_size(std::uint32_t index) const;
  std::uint64_t chunk_offset(std::uint32_t index) const;
};

}  // namespace swarmsync
