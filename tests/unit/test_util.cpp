#include <set>
#include <string>

#include "test_helpers.h"
#include "util.h"

using fsrv::ascii_iequals;
using fsrv::ascii_istarts_with;
using fsrv::ascii_lower;
using fsrv::format_time_iso;
using fsrv::human_size;
using fsrv::is_valid_utf8;
using fsrv::percent_decode;
using fsrv::trim;
using fsrv::uuid_v4;

TEST_CASE(util_uuid_v4_has_canonical_shape) {
  const std::string id = uuid_v4();
  CHECK_EQ(id.size(), static_cast<size_t>(36));
  CHECK_EQ(id[8], '-');
  CHECK_EQ(id[13], '-');
  CHECK_EQ(id[18], '-');
  CHECK_EQ(id[23], '-');
  CHECK_EQ(id[14], '4');
  CHECK(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    const char c = id[i];
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}

TEST_CASE(util_uuid_v4_is_unique) {
  std::set<std::string> seen;
  for (int i = 0; i < 500; ++i) seen.insert(uuid_v4());
  CHECK_EQ(seen.size(), static_cast<size_t>(500));
}

TEST_CASE(util_percent_decode_basics) {
  std::string out;
  REQUIRE(percent_decode("a%20b", out));
  CHECK_EQ(out, std::string("a b"));

  REQUIRE(percent_decode("plain", out));
  CHECK_EQ(out, std::string("plain"));

  REQUIRE(percent_decode("", out));
  CHECK_EQ(out, std::string(""));

  // '+' is a literal plus, not a space (this is a path, not a form body).
  REQUIRE(percent_decode("a+b", out));
  CHECK_EQ(out, std::string("a+b"));

  REQUIRE(percent_decode("%E4%B8%AD", out));
  CHECK_EQ(out, std::string("\xE4\xB8\xAD"));

  REQUIRE(percent_decode("%2f%2F", out));
  CHECK_EQ(out, std::string("//"));

  REQUIRE(percent_decode("%00", out));
  CHECK_EQ(out.size(), static_cast<size_t>(1));
  CHECK_EQ(out[0], '\0');
}

TEST_CASE(util_percent_decode_rejects_malformed) {
  std::string out;
  CHECK(!percent_decode("%zz", out));
  CHECK(!percent_decode("%2", out));
  CHECK(!percent_decode("abc%", out));
  CHECK(!percent_decode("%g0", out));
}

TEST_CASE(util_ascii_case_helpers) {
  CHECK_EQ(ascii_lower("AbC-123"), std::string("abc-123"));
  CHECK_EQ(ascii_lower(""), std::string(""));

  CHECK(ascii_iequals("AbC", "aBc"));
  CHECK(!ascii_iequals("abc", "abd"));
  CHECK(!ascii_iequals("abc", "ab"));
  CHECK(!ascii_iequals("", "a"));

  CHECK(ascii_istarts_with("/Api/Files", "/api"));
  CHECK(!ascii_istarts_with("/api", "/admin"));
  CHECK(ascii_istarts_with("anything", ""));
  CHECK(!ascii_istarts_with("ab", "abc"));
}

TEST_CASE(util_trim) {
  CHECK_EQ(trim("  x \t\r\n"), std::string("x"));
  CHECK_EQ(trim("x"), std::string("x"));
  CHECK_EQ(trim("   "), std::string(""));
  CHECK_EQ(trim(""), std::string(""));
  CHECK_EQ(trim("\ta b\t"), std::string("a b"));
}

TEST_CASE(util_utf8_validation) {
  CHECK(is_valid_utf8(""));
  CHECK(is_valid_utf8("plain ascii"));
  CHECK(is_valid_utf8("\xE4\xB8\xAD\xE6\x96\x87"));
  CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));  // U+1F600

  CHECK(!is_valid_utf8("\xFF"));
  CHECK(!is_valid_utf8("\x80"));
  CHECK(!is_valid_utf8("\xC0\xAF"));        // overlong '/'
  CHECK(!is_valid_utf8("\xE0\x80\xAF"));    // overlong
  CHECK(!is_valid_utf8("\xED\xA0\x80"));    // UTF-16 surrogate
  CHECK(!is_valid_utf8("\xE4\xB8"));        // truncated
  CHECK(!is_valid_utf8("\xF5\x80\x80\x80"));  // > U+10FFFF
  CHECK(!is_valid_utf8("ok\xC3"));          // truncated at end
}

TEST_CASE(util_format_time_iso) {
  const std::string text = format_time_iso(1700000000);
  CHECK_EQ(text.size(), static_cast<size_t>(19));
  CHECK_EQ(text[4], '-');
  CHECK_EQ(text[7], '-');
  CHECK_EQ(text[10], ' ');
  CHECK_EQ(text[13], ':');
  CHECK_EQ(text[16], ':');
  for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u,
                   18u}) {
    CHECK(text[i] >= '0' && text[i] <= '9');
  }
}

TEST_CASE(util_human_size) {
  CHECK_EQ(human_size(0), std::string("0 B"));
  CHECK_EQ(human_size(1023), std::string("1023 B"));
  CHECK_EQ(human_size(1024), std::string("1.0 KB"));
  CHECK_EQ(human_size(1536), std::string("1.5 KB"));
  CHECK_EQ(human_size(1024ull * 1024), std::string("1.0 MB"));
  CHECK_EQ(human_size(1024ull * 1024 * 1024), std::string("1.0 GB"));
  CHECK_EQ(human_size(1024ull * 1024 * 1024 * 1024), std::string("1.0 TB"));
}

TEST_CASE(util_time_helpers_are_sane) {
  const int64_t sec = fsrv::now_sec();
  const int64_t ms = fsrv::now_ms();
  CHECK(sec > 1600000000);
  CHECK(ms / 1000 >= sec - 5);
  CHECK(ms / 1000 <= sec + 5);
}
