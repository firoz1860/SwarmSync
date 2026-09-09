#include "swarmsync/crypto.hpp"
#include "swarmsync/download.hpp"
#include "swarmsync/manifest.hpp"
#include "swarmsync/peer_server.hpp"
#include "swarmsync/resume_store.hpp"
#include "swarmsync/tracker.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path integration_directory() {
  const auto path = std::filesystem::temp_directory_path() / "swarmsync-integration-tests";
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void write_fixture(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary);
  for (std::size_t index = 0; index < 900U * 1024U; ++index) {
    output.put(static_cast<char>((index * 31U) % 251U));
  }
}

void write_corrupt_partial(const std::filesystem::path& path, std::uint64_t size) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  const std::vector<char> zeros(static_cast<std::size_t>(size), '\0');
  output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
}

}  // namespace

SS_TEST(two_local_peers_transfer_a_verified_multi_chunk_file) {
  const auto directory = integration_directory();
  const auto source = directory / "source.bin";
  write_fixture(source);
  const auto manifest = swarmsync::Manifest::from_file(source, 64U * 1024U);

  swarmsync::TrackerServer tracker(0U, "test-token", std::chrono::seconds(10));
  tracker.start();

  swarmsync::PeerServer seeder({manifest, source, "test-token", 0U, "127.0.0.1", 8U, {}});
  seeder.start();
  swarmsync::TrackerClient tracker_client(tracker.endpoint(), "test-token");
  tracker_client.announce({manifest.swarm_id(), "seed-a", seeder.endpoint().port, {}, true});

  swarmsync::DownloadConfig config{manifest,
                                    directory / "download",
                                    tracker.endpoint(),
                                    "test-token",
                                    0U,
                                    "downloader-a",
                                    3U,
                                    3U,
                                    std::chrono::milliseconds(1500),
                                    std::chrono::milliseconds(100)};
  const auto result = swarmsync::DownloadCoordinator(config).run();

  SS_CHECK(result.completed_chunks == manifest.chunk_hashes.size());
  SS_CHECK(swarmsync::sha256_file(result.final_path) == swarmsync::sha256_file(source));
  seeder.stop();
  tracker.stop();
}

SS_TEST(download_rechecks_completed_resume_chunks_before_trusting_them) {
  const auto directory = integration_directory();
  const auto source = directory / "source.bin";
  write_fixture(source);
  const auto manifest = swarmsync::Manifest::from_file(source, 64U * 1024U);

  swarmsync::TrackerServer tracker(0U, "test-token", std::chrono::seconds(10));
  tracker.start();
  swarmsync::PeerServer seeder({manifest, source, "test-token", 0U, "127.0.0.1", 8U, {}});
  seeder.start();
  swarmsync::TrackerClient tracker_client(tracker.endpoint(), "test-token");
  tracker_client.announce({manifest.swarm_id(), "seed-a", seeder.endpoint().port, {}, true});

  const auto output_directory = directory / "download";
  std::filesystem::create_directories(output_directory);
  const auto partial_path = output_directory / (manifest.file_name + ".part");
  write_corrupt_partial(partial_path, manifest.file_size_bytes);
  const auto resume_path = output_directory / (manifest.swarm_id() + ".resume");
  auto resume = swarmsync::ResumeStore::load_or_create(
      resume_path, manifest.swarm_id(), static_cast<std::uint32_t>(manifest.chunk_hashes.size()));
  for (std::uint32_t index = 0U; index < manifest.chunk_hashes.size(); ++index) {
    resume.mark_complete(index);
  }
  resume.flush();

  swarmsync::DownloadConfig config{manifest,
                                    output_directory,
                                    tracker.endpoint(),
                                    "test-token",
                                    0U,
                                    "downloader-a",
                                    3U,
                                    3U,
                                    std::chrono::milliseconds(1500),
                                    std::chrono::milliseconds(100)};
  const auto result = swarmsync::DownloadCoordinator(config).run();

  SS_CHECK(result.completed_chunks == manifest.chunk_hashes.size());
  SS_CHECK(swarmsync::sha256_file(result.final_path) == swarmsync::sha256_file(source));
  seeder.stop();
  tracker.stop();
}

SS_TEST(peer_server_can_stop_an_idle_accept_loop) {
  const auto directory = integration_directory();
  const auto source = directory / "source.bin";
  write_fixture(source);
  const auto manifest = swarmsync::Manifest::from_file(source, 64U * 1024U);

  swarmsync::PeerServer server({manifest, source, "test-token", 0U, "127.0.0.1", 4U, {}});
  server.start();
  server.stop();
}
