#pragma once
#include <cstdio>
#include <vector>

namespace sopt_test {
struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Reg {
  Reg(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
}  // namespace sopt_test

#define TEST(name)                                                  \
  static void name();                                               \
  static sopt_test::Reg reg_##name(#name, name);                    \
  static void name()

#define CHECK(cond)                                                               \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      ++sopt_test::failures();                                                    \
    }                                                                             \
  } while (0)
