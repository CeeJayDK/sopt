#include <cstring>
#include <exception>

#include "test.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& c : sopt_test::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    std::printf("[ RUN  ] %s\n", c.name);
    const int before = sopt_test::failures();
    try {
      c.fn();
    } catch (const std::exception& e) {
      std::printf("  FAIL exception: %s\n", e.what());
      ++sopt_test::failures();
    }
    std::printf("[ %s ] %s\n", sopt_test::failures() == before ? " OK " : "FAIL", c.name);
    ++run;
  }
  std::printf("\n%d test(s), %d failure(s)\n", run, sopt_test::failures());
  return sopt_test::failures() ? 1 : 0;
}
