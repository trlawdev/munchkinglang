#pragma once
#include <exception>
#include <string>

namespace munx
{

  /// Fatal front-end failure (lex, parse, or compile).
  /// Carries a human-readable message, typically `file:line:col: error: …`.
  class compilation_error : public std::exception
  {
    std::string message_;

  public:
    /// Construct with a fully formatted diagnostic message.
    explicit compilation_error(std::string message) : message_(std::move(message)) {}

    /// @return Null-terminated diagnostic text (owned by this object).
    const char *what() const noexcept override { return message_.c_str(); }
  };

} // namespace munx
