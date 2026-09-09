#include "swarmsync/resume_store.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <vector>

namespace {

std::filesystem::path resume_test_path() {
  const auto directory = std::filesystem::temp_directory_path() / "swarmsync-resume-tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  return directory / "download.resume";
}

}  // namespace

SS_TEST(resume_state_persists_verified_chunks_across_a_restart) {
  const auto path = resume_test_path();
  auto state = swarmsync::ResumeStore::load_or_create(path, "swarm-a", 4U);
  state.mark_complete(1U);
  state.mark_complete(3U);
  state.flush();

  auto reopened = swarmsync::ResumeStore::load_or_create(path, "swarm-a", 4U);
  SS_CHECK(reopened.complete(1U));
  SS_CHECK(reopened.complete(3U));
  SS_CHECK(!reopened.complete(0U));
  SS_CHECK(reopened.completed_chunks() == std::vector<std::uint32_t>({1U, 3U}));
}

SS_TEST(resume_state_rejects_a_different_swarm_or_chunk_count) {
  const auto path = resume_test_path();
  auto state = swarmsync::ResumeStore::load_or_create(path, "swarm-a", 2U);
  state.flush();

  SS_CHECK_THROWS(swarmsync::ResumeStore::load_or_create(path, "swarm-b", 2U));
  SS_CHECK_THROWS(swarmsync::ResumeStore::load_or_create(path, "swarm-a", 3U));
}

SS_TEST(resume_state_can_clear_a_chunk_that_fails_revalidation) {
  const auto path = resume_test_path();
  auto state = swarmsync::ResumeStore::load_or_create(path, "swarm-a", 2U);
  state.mark_complete(0U);
  state.mark_complete(1U);
  state.flush();

  state.mark_incomplete(1U);
  state.flush();

  auto reopened = swarmsync::ResumeStore::load_or_create(path, "swarm-a", 2U);
  SS_CHECK(reopened.complete(0U));
  SS_CHECK(!reopened.complete(1U));
}
