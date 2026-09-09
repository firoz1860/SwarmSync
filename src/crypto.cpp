#include "swarmsync/crypto.hpp"

#include "swarmsync/common.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <vector>

namespace swarmsync {
namespace {

constexpr std::string_view kHex = "0123456789abcdef";

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

DigestContext new_digest_context() {
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context) {
    throw Error("Unable to allocate SHA-256 context");
  }
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    throw Error("Unable to initialize SHA-256 context");
  }
  return context;
}

void update_digest(EVP_MD_CTX* context, const void* bytes, std::size_t size) {
  if (size != 0U && EVP_DigestUpdate(context, bytes, size) != 1) {
    throw Error("Unable to update SHA-256 digest");
  }
}

std::string finish_digest(EVP_MD_CTX* context) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1 || digest_size != 32U) {
    throw Error("Unable to finalize SHA-256 digest");
  }

  std::string result;
  result.reserve(digest_size * 2U);
  for (unsigned int index = 0; index < digest_size; ++index) {
    result.push_back(kHex[digest[index] >> 4U]);
    result.push_back(kHex[digest[index] & 0x0FU]);
  }
  return result;
}

int nibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

}  // namespace

std::string sha256_hex(std::span<const std::byte> bytes) {
  auto context = new_digest_context();
  update_digest(context.get(), bytes.data(), bytes.size());
  return finish_digest(context.get());
}

std::string sha256_hex(std::string_view bytes) {
  return sha256_hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error("Unable to open file for hashing: " + path.string());
  }

  auto context = new_digest_context();
  std::vector<char> buffer(64U * 1024U);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = input.gcount();
    if (bytes_read > 0) {
      update_digest(context.get(), buffer.data(), static_cast<std::size_t>(bytes_read));
    }
  }
  if (!input.eof()) {
    throw Error("Unable to read file while hashing: " + path.string());
  }
  return finish_digest(context.get());
}

bool is_sha256_hex(std::string_view value) {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9')) ||
                  (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('f'));
         });
}

bool secure_token_equal(std::string_view expected, std::string_view supplied) {
  std::size_t difference = expected.size() ^ supplied.size();
  const auto longest = std::max(expected.size(), supplied.size());
  for (std::size_t index = 0; index < longest; ++index) {
    const unsigned char left = index < expected.size() ? static_cast<unsigned char>(expected[index]) : 0U;
    const unsigned char right = index < supplied.size() ? static_cast<unsigned char>(supplied[index]) : 0U;
    difference |= static_cast<std::size_t>(left ^ right);
  }
  return difference == 0U;
}

std::string hex_encode(std::string_view value) {
  std::string encoded;
  encoded.reserve(value.size() * 2U);
  for (const unsigned char character : value) {
    encoded.push_back(kHex[character >> 4U]);
    encoded.push_back(kHex[character & 0x0FU]);
  }
  return encoded;
}

std::string hex_decode(std::string_view value) {
  if (value.size() % 2U != 0U) {
    throw Error("Hex value must contain complete bytes");
  }
  std::string decoded;
  decoded.reserve(value.size() / 2U);
  for (std::size_t index = 0; index < value.size(); index += 2U) {
    const int high = nibble(value[index]);
    const int low = nibble(value[index + 1U]);
    if (high < 0 || low < 0) {
      throw Error("Hex value contains an invalid character");
    }
    decoded.push_back(static_cast<char>((high << 4U) | low));
  }
  return decoded;
}

}  // namespace swarmsync
