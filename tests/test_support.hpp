#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace swarmsync::test {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

class Registrar {
 public:
  Registrar(std::string_view name, TestFunction function) {
    registry().push_back({std::string(name), function});
  }
};

class TestFailure : public std::runtime_error {
 public:
  explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

inline int run_all(int argc, char** argv) {
  std::string filter;
  if (const char* configured_filter = std::getenv("TEST_FILTER"); configured_filter != nullptr) {
    filter = configured_filter;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--filter") {
    filter = argv[2];
  }

  int passed = 0;
  int failed = 0;
  for (const auto& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    try {
      test.function();
      ++passed;
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "FAIL " << test.name << ": unknown exception\n";
    }
  }
  std::cout << "Tests: " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace swarmsync::test

#define SS_TEST(name) \
  void name(); \
  static ::swarmsync::test::Registrar registrar_##name(#name, &name); \
  void name()

#define SS_CHECK(expression) \
  do { \
    if (!(expression)) { \
      throw ::swarmsync::test::TestFailure("Check failed: " #expression); \
    } \
  } while (false)

#define SS_CHECK_THROWS(expression) \
  do { \
    bool threw = false; \
    try { \
      static_cast<void>(expression); \
    } catch (const std::exception&) { \
      threw = true; \
    } \
    if (!threw) { \
      throw ::swarmsync::test::TestFailure("Expected exception: " #expression); \
    } \
  } while (false)
