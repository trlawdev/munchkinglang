#pragma once
#include <exception>

namespace munx
{

  class compilation_error : public std::exception
  {
    const char *message_;

  public:
    // Not constexpr: std::exception's constructor is not constexpr.
    explicit compilation_error(const char *message) : message_(message) {}

    constexpr const char *what() const noexcept override { return message_; }
  };

} // namespace munx
