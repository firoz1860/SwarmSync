#include "swarmsync/manifest.hpp"

#include "swarmsync/common.hpp"
#include "swarmsync/crypto.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

namespace swarmsync {
namespace {

constexpr std::uint32_t kMaxChunkSize = 4U * 1024U * 1024U;
constexpr std::uint32_t kMaxChunkCount = 1'000'000U;

template <typename Number>
Number parse_unsigned(std::string_view value, std::string_view field) {
  if (value.empty()) {
    throw Error("Manifest field is empty: " + std::string(field));
  }
  Number parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw Error("Manifest field is not an unsigned integer: " + std::string(field));
  }
  return parsed;
}

std::uint64_t expected_chunk_count(std::uint64_t file_size, std::uint32_t chunk_size) {
  return file_size == 0U ? 0U : ((file_size - 1U) / chunk_size) + 1U;
}

bool safe_file_name(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
         name.find(static_cast<char>(0)) == std::string_view::npos;
}

std::string read_line(std::istream& input, const std::filesystem::path& path) {
  std::string line;
  if (!std::getline(input, line)) {
    throw Error("Unexpected end of manifest: " + path.string());
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::string require_field(std::string_view line, std::string_view field) {
  const std::string prefix = std::string(field) + "=";
  if (!line.starts_with(prefix)) {
    throw Error("Expected manifest field: " + std::string(field));
  }
  return std::string(line.substr(prefix.size()));
}

}  // namespace

Manifest Manifest::from_file(const std::filesystem::path& path, std::uint32_t chunk_size) {
  if (chunk_size == 0U || chunk_size > kMaxChunkSize) {
    throw Error("Chunk size must be between 1 and 4194304 bytes");
  }
  if (!std::filesystem::is_regular_file(path)) {
    throw Error("Manifest source must be a regular file: " + path.string());
  }

  const auto file_size = std::filesystem::file_size(path);
  const auto count = expected_chunk_count(file_size, chunk_size);
  if (count > kMaxChunkCount) {
    throw Error("File creates more chunks than the configured manifest limit");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error("Unable to open manifest source: " + path.string());
  }

  Manifest manifest;
  manifest.file_name = path.filename().string();
  manifest.file_size_bytes = file_size;
  manifest.chunk_size_bytes = chunk_size;
  manifest.file_sha256 = sha256_file(path);

  std::vector<char> buffer(chunk_size);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = input.gcount();
    if (bytes_read > 0) {
      manifest.chunk_hashes.push_back(
          sha256_hex(std::string_view(buffer.data(), static_cast<std::size_t>(bytes_read))));
    }
  }
  if (!input.eof()) {
    throw Error("Unable to read manifest source: " + path.string());
  }
  manifest.validate();
  return manifest;
}

Manifest Manifest::load(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error("Unable to open manifest: " + path.string());
  }
  if (read_line(input, path) != "SWARMSYNC_MANIFEST_V1") {
    throw Error("Unsupported manifest format");
  }

  Manifest manifest;
  manifest.file_name = hex_decode(require_field(read_line(input, path), "file_name"));
  manifest.file_size_bytes = parse_unsigned<std::uint64_t>(
      require_field(read_line(input, path), "file_size"), "file_size");
  manifest.chunk_size_bytes = parse_unsigned<std::uint32_t>(
      require_field(read_line(input, path), "chunk_size"), "chunk_size");
  manifest.file_sha256 = require_field(read_line(input, path), "file_sha256");
  const auto declared_count = parse_unsigned<std::uint32_t>(
      require_field(read_line(input, path), "chunk_count"), "chunk_count");
  if (declared_count > kMaxChunkCount) {
    throw Error("Manifest declares too many chunks");
  }

  std::map<std::uint32_t, std::string> chunks;
  for (std::uint32_t line_number = 0; line_number < declared_count; ++line_number) {
    const auto entry = require_field(read_line(input, path), "chunk");
    const auto comma = entry.find(',');
    if (comma == std::string::npos) {
      throw Error("Manifest chunk entry is missing a comma");
    }
    const auto index = parse_unsigned<std::uint32_t>(std::string_view(entry).substr(0U, comma), "chunk index");
    const auto hash = entry.substr(comma + 1U);
    if (!chunks.emplace(index, hash).second) {
      throw Error("Manifest contains a duplicate chunk index");
    }
  }
  std::string trailing;
  if (std::getline(input, trailing)) {
    throw Error("Manifest contains unexpected trailing data");
  }

  manifest.chunk_hashes.reserve(chunks.size());
  for (std::uint32_t index = 0; index < declared_count; ++index) {
    const auto item = chunks.find(index);
    if (item == chunks.end()) {
      throw Error("Manifest chunk indexes are not contiguous");
    }
    manifest.chunk_hashes.push_back(item->second);
  }
  manifest.validate();
  return manifest;
}

void Manifest::save(const std::filesystem::path& path) const {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw Error("Unable to write manifest: " + path.string());
  }
  output << serialize();
  if (!output) {
    throw Error("Unable to finish manifest write: " + path.string());
  }
}

void Manifest::validate() const {
  if (!safe_file_name(file_name)) {
    throw Error("Manifest file name is not a safe basename");
  }
  if (chunk_size_bytes == 0U || chunk_size_bytes > kMaxChunkSize) {
    throw Error("Manifest chunk size is outside the allowed range");
  }
  if (!is_sha256_hex(file_sha256)) {
    throw Error("Manifest file hash is not a lowercase SHA-256 digest");
  }
  const auto expected = expected_chunk_count(file_size_bytes, chunk_size_bytes);
  if (expected > kMaxChunkCount || chunk_hashes.size() != expected) {
    throw Error("Manifest chunk count does not match file size");
  }
  for (const auto& hash : chunk_hashes) {
    if (!is_sha256_hex(hash)) {
      throw Error("Manifest chunk hash is not a lowercase SHA-256 digest");
    }
  }
}

std::string Manifest::serialize() const {
  validate();
  std::ostringstream output;
  output << "SWARMSYNC_MANIFEST_V1\n";
  output << "file_name=" << hex_encode(file_name) << '\n';
  output << "file_size=" << file_size_bytes << '\n';
  output << "chunk_size=" << chunk_size_bytes << '\n';
  output << "file_sha256=" << file_sha256 << '\n';
  output << "chunk_count=" << chunk_hashes.size() << '\n';
  for (std::size_t index = 0; index < chunk_hashes.size(); ++index) {
    output << "chunk=" << index << ',' << chunk_hashes[index] << '\n';
  }
  return output.str();
}

std::string Manifest::swarm_id() const {
  return sha256_hex(serialize());
}

std::uint32_t Manifest::chunk_size(std::uint32_t index) const {
  validate();
  if (index >= chunk_hashes.size()) {
    throw Error("Chunk index is outside the manifest");
  }
  const auto offset = static_cast<std::uint64_t>(index) * chunk_size_bytes;
  const auto remaining = file_size_bytes - offset;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, chunk_size_bytes));
}

std::uint64_t Manifest::chunk_offset(std::uint32_t index) const {
  if (index >= chunk_hashes.size()) {
    throw Error("Chunk index is outside the manifest");
  }
  return static_cast<std::uint64_t>(index) * chunk_size_bytes;
}

}  // namespace swarmsync
