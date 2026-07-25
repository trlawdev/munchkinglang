#include <iostream>
#include <string>
#include "../include/logger.hpp"

int main() {
  munx::logger logger{std::cout, 0, 0};
  logger.log_line<munx::log_level::warn>("Hello world");
  // Format is an NTTP; "\\{" is a literal '{' for the formatter.
  logger.log_fmt<munx::log_level::error,
                 "Hello {}. I am {} years old. {} \\{\n">("world", 20,
                                                          std::string("test"));
  std::cout.flush();
}
