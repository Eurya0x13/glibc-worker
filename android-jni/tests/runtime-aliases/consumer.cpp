#include <cstring>
#include <stdexcept>
#include <string>

extern "C" int guest_cxx_probe() {
  constexpr char message[] = "C++ string storage and exception unwinding";
  try {
    std::string text(message);
    throw std::runtime_error(text);
  } catch (const std::runtime_error &error) {
    return std::strcmp(error.what(), message) == 0 ? 42 : -1;
  }
}
