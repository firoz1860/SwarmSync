#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace swarmsync {

std::string sha256_hex(std::span<const std::byte> bytes);
std::string sha256_hex(std::string_view bytes);
std::string sha256_file(const std::filesystem::path& path);
bool is_sha256_hex(std::string_view value);
bool secure_token_equal(std::string_view expected, std::string_view supplied);
std::string hex_encode(std::string_view value);
std::string hex_decode(std::string_view value);

}  // namespace swarmsync
