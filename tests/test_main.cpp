#include "test_support.hpp"

SS_TEST(test_harness_runs_a_passing_assertion) {
  SS_CHECK(2 + 2 == 4);
}

int main(int argc, char** argv) {
  return swarmsync::test::run_all(argc, argv);
}
