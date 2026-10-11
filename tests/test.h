#pragma once

// Minimal self-contained test harness (no external dependencies).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xft {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();
extern int g_failures;

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace xft

#define XFT_CAT2(a, b) a##b
#define XFT_CAT(a, b) XFT_CAT2(a, b)

#define TEST(name)                                                         \
  static void name();                                                      \
  static ::xft::Registrar XFT_CAT(reg_, name)(#name, name);                \
  static void name()

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      std::printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);         \
      ++::xft::g_failures;                                                             \
    }                                                                                  \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                              \
  do {                                                                                     \
    const double xa_ = (a), xb_ = (b);                                                     \
    if (!(std::fabs(xa_ - xb_) <= (tol))) {                                                \
      std::printf("    %s:%d: CHECK_NEAR(%s, %s) failed: %.9g vs %.9g (tol %g)\n", __FILE__, \
                  __LINE__, #a, #b, xa_, xb_, static_cast<double>(tol));                   \
      ++::xft::g_failures;                                                                 \
    }                                                                                      \
  } while (0)
