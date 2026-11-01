#pragma once

// Tiny zero-dependency unit test framework.
//
//   TEST_CASE(name) { CHECK(cond); REQUIRE(cond); CHECK_EQ(a, b); }
//
// `CHECK` records a failure and keeps going, `REQUIRE` aborts the current test.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline int& check_count() {
  static int count = 0;
  return count;
}

struct AbortTest {};

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <typename T>
std::string stringify(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_convertible_v<T, const char*>) {
    return std::string(static_cast<const char*>(value));
  } else {
    return "<value>";
  }
}

inline void record_failure(const char* file, int line, const std::string& text) {
  ++failure_count();
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, text.c_str());
}

}  // namespace testing

#define TEST_CASE(name)                                    \
  static void name();                                      \
  static ::testing::Registrar registrar_##name(#name, &name); \
  static void name()

#define CHECK(cond)                                                      \
  do {                                                                   \
    ++::testing::check_count();                                          \
    if (!(cond)) {                                                       \
      ::testing::record_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    }                                                                    \
  } while (0)

#define REQUIRE(cond)                                                    \
  do {                                                                   \
    ++::testing::check_count();                                          \
    if (!(cond)) {                                                       \
      ::testing::record_failure(__FILE__, __LINE__, "REQUIRE(" #cond ")"); \
      throw ::testing::AbortTest{};                                      \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b)                                                        \
  do {                                                                        \
    ++::testing::check_count();                                               \
    const auto& lhs_value = (a);                                              \
    const auto& rhs_value = (b);                                              \
    if (!(lhs_value == rhs_value)) {                                          \
      ::testing::record_failure(                                              \
          __FILE__, __LINE__,                                                 \
          std::string("CHECK_EQ(" #a ", " #b ") -> ") +                       \
              ::testing::stringify(lhs_value) + " vs " +                      \
              ::testing::stringify(rhs_value));                               \
    }                                                                         \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                 \
  do {                                                                        \
    ++::testing::check_count();                                               \
    const double lhs_value = static_cast<double>(a);                          \
    const double rhs_value = static_cast<double>(b);                          \
    if (!(std::abs(lhs_value - rhs_value) <= (eps))) {                        \
      ::testing::record_failure(                                              \
          __FILE__, __LINE__,                                                 \
          std::string("CHECK_NEAR(" #a ", " #b ") -> ") +                     \
              ::testing::stringify(lhs_value) + " vs " +                      \
              ::testing::stringify(rhs_value));                               \
    }                                                                         \
  } while (0)
