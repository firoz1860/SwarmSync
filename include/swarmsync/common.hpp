#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace swarmsync {

class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& message) : std::runtime_error(message) {}
};

void log_info(std::string_view message);
void log_error(std::string_view message);

}  // namespace swarmsync
