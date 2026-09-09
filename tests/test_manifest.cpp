#include "swarmsync/crypto.hpp"
#include "swarmsync/manifest.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path manifest_test_dir() {
  const auto path = std::filesystem::temp_directory_path() / "swarmsync-manifest-tests";
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void write_text_file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary);
  out << contents;
}

}  // namespace

SS_TEST(sha256_and_token_comparison_have_stable_security_contracts) {
  SS_CHECK(swarmsync::sha256_hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  SS_CHECK(swarmsync::secure_token_equal("private-token", "private-token"));
  SS_CHECK(!swarmsync::secure_token_equal("private-token", "private-token-2"));
}

SS_TEST(manifest_hashes_chunks_and_round_trips_canonical_data) {
  const auto directory = manifest_test_dir();
  const auto source = directory / "sample.bin";
  const auto saved_manifest = directory / "sample.swarmsync";
  write_text_file(source, "abcdefghi");

  const auto manifest = swarmsync::Manifest::from_file(source, 3);
  SS_CHECK(manifest.file_size_bytes == 9U);
  SS_CHECK(manifest.chunk_size_bytes == 3U);
  SS_CHECK(manifest.chunk_hashes.size() == 3U);
  SS_CHECK(manifest.chunk_hashes.at(0) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  manifest.save(saved_manifest);
  const auto loaded = swarmsync::Manifest::load(saved_manifest);
  SS_CHECK(loaded.swarm_id() == manifest.swarm_id());
  SS_CHECK(loaded.file_name == "sample.bin");
}

SS_TEST(manifest_rejects_a_non_sha256_chunk_hash) {
  const auto directory = manifest_test_dir();
  const auto invalid_manifest = directory / "invalid.swarmsync";
  write_text_file(invalid_manifest,
                  "SWARMSYNC_MANIFEST_V1\n"
                  "file_name=73616d706c652e62696e\n"
                  "file_size=3\n"
                  "chunk_size=3\n"
                  "file_sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
                  "chunk_count=1\n"
                  "chunk=0,not-a-sha256\n");

  SS_CHECK_THROWS(swarmsync::Manifest::load(invalid_manifest));
}
