#include <cmath>
#include <cstring>
#include <string>

#include "test_framework.h"

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg.rfind("--filter=", 0) == 0) {
      filter = arg.substr(9);
    } else if (arg == "--list") {
      for (const auto& test : testing::registry()) {
        std::printf("%s\n", test.name);
      }
      return 0;
    }
  }

  int executed = 0;
  int failed_tests = 0;

  for (const auto& test : testing::registry()) {
    if (!filter.empty() && std::strstr(test.name, filter.c_str()) == nullptr) {
      continue;
    }
    const int before = testing::failure_count();
    ++executed;
    std::printf("[ RUN      ] %s\n", test.name);
    try {
      test.fn();
    } catch (const testing::AbortTest&) {
      // REQUIRE already recorded the failure.
    } catch (const std::exception& e) {
      testing::record_failure(__FILE__, __LINE__,
                              std::string("uncaught exception: ") + e.what());
    } catch (...) {
      testing::record_failure(__FILE__, __LINE__, "uncaught unknown exception");
    }
    const bool ok = testing::failure_count() == before;
    if (!ok) ++failed_tests;
    std::printf("[ %s ] %s\n", ok ? "     OK " : "  FAILED", test.name);
  }

  std::printf("\n%d test(s) executed, %d failed, %d assertion(s)\n", executed,
              failed_tests, testing::check_count());
  return failed_tests == 0 ? 0 : 1;
}
