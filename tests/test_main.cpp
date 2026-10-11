#include <cstring>

#include "test.h"

namespace xft {
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
int g_failures = 0;
}  // namespace xft

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failedTests = 0;
  for (auto& t : xft::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    const int before = xft::g_failures;
    std::printf("[ RUN  ] %s\n", t.name);
    t.fn();
    ++run;
    if (xft::g_failures != before) {
      ++failedTests;
      std::printf("[ FAIL ] %s\n", t.name);
    }
  }
  std::printf("\n%d test(s) run, %d failed\n", run, failedTests);
  return failedTests ? 1 : 0;
}
