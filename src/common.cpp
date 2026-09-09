#include "swarmsync/common.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace swarmsync {
namespace {

std::mutex log_mutex;

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::ostringstream out;
  out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
  return out.str();
}

void log(std::string_view level, std::string_view message) {
  std::lock_guard lock(log_mutex);
  std::cerr << timestamp() << " [" << level << "] " << message << '\n';
}

}  // namespace

void log_info(std::string_view message) {
  log("INFO", message);
}

void log_error(std::string_view message) {
  log("ERROR", message);
}

}  // namespace swarmsync
