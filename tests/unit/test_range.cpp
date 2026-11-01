#include <string>

#include "range.h"
#include "test_helpers.h"

using fsrv::ByteRange;
using fsrv::parse_range;
using fsrv::RangeOutcome;

namespace {

struct Parsed {
  RangeOutcome outcome;
  ByteRange range;
};

Parsed parse(const std::string& header, uint64_t size) {
  Parsed p{RangeOutcome::Ignore, ByteRange{}};
  p.outcome = parse_range(header, size, p.range);
  return p;
}

}  // namespace

TEST_CASE(range_absent_or_unusable_header_is_ignored) {
  CHECK(parse("", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("   ", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("items=0-10", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes=", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes=*", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes=abc-def", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes=0", 1000).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes=99999999999999999999999-", 1000).outcome ==
        RangeOutcome::Ignore);
}

TEST_CASE(range_closed_interval) {
  const Parsed p = parse("bytes=0-99", 1000);
  REQUIRE(p.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(p.range.start, static_cast<uint64_t>(0));
  CHECK_EQ(p.range.end, static_cast<uint64_t>(99));
  CHECK_EQ(p.range.length(), static_cast<uint64_t>(100));

  const Parsed mid = parse("bytes=500-500", 1000);
  REQUIRE(mid.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(mid.range.start, static_cast<uint64_t>(500));
  CHECK_EQ(mid.range.end, static_cast<uint64_t>(500));
  CHECK_EQ(mid.range.length(), static_cast<uint64_t>(1));
}

TEST_CASE(range_open_ended_interval) {
  const Parsed p = parse("bytes=900-", 1000);
  REQUIRE(p.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(p.range.start, static_cast<uint64_t>(900));
  CHECK_EQ(p.range.end, static_cast<uint64_t>(999));
  CHECK_EQ(p.range.length(), static_cast<uint64_t>(100));

  const Parsed all = parse("bytes=0-", 1000);
  REQUIRE(all.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(all.range.start, static_cast<uint64_t>(0));
  CHECK_EQ(all.range.end, static_cast<uint64_t>(999));
}

TEST_CASE(range_end_beyond_eof_is_clamped) {
  const Parsed p = parse("bytes=990-5000", 1000);
  REQUIRE(p.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(p.range.start, static_cast<uint64_t>(990));
  CHECK_EQ(p.range.end, static_cast<uint64_t>(999));
  CHECK_EQ(p.range.length(), static_cast<uint64_t>(10));
}

TEST_CASE(range_suffix_interval) {
  const Parsed p = parse("bytes=-100", 1000);
  REQUIRE(p.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(p.range.start, static_cast<uint64_t>(900));
  CHECK_EQ(p.range.end, static_cast<uint64_t>(999));
  CHECK_EQ(p.range.length(), static_cast<uint64_t>(100));

  const Parsed whole = parse("bytes=-5000", 1000);
  REQUIRE(whole.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(whole.range.start, static_cast<uint64_t>(0));
  CHECK_EQ(whole.range.end, static_cast<uint64_t>(999));

  const Parsed one = parse("bytes=-1", 1000);
  REQUIRE(one.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(one.range.start, static_cast<uint64_t>(999));
  CHECK_EQ(one.range.end, static_cast<uint64_t>(999));
}

TEST_CASE(range_unsatisfiable_cases) {
  CHECK(parse("bytes=1000-", 1000).outcome == RangeOutcome::Unsatisfiable);
  CHECK(parse("bytes=1000-2000", 1000).outcome == RangeOutcome::Unsatisfiable);
  CHECK(parse("bytes=50-49", 1000).outcome == RangeOutcome::Unsatisfiable);
  CHECK(parse("bytes=-0", 1000).outcome == RangeOutcome::Unsatisfiable);
  // Multiple ranges are not supported.
  CHECK(parse("bytes=0-1,5-6", 1000).outcome == RangeOutcome::Unsatisfiable);
}

TEST_CASE(range_empty_file) {
  CHECK(parse("", 0).outcome == RangeOutcome::Ignore);
  CHECK(parse("bytes=0-", 0).outcome == RangeOutcome::Unsatisfiable);
  CHECK(parse("bytes=-1", 0).outcome == RangeOutcome::Unsatisfiable);
  CHECK(parse("bytes=0-0", 0).outcome == RangeOutcome::Unsatisfiable);
}

TEST_CASE(range_is_lenient_about_whitespace_and_case) {
  const Parsed upper = parse("BYTES=0-9", 1000);
  REQUIRE(upper.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(upper.range.start, static_cast<uint64_t>(0));
  CHECK_EQ(upper.range.end, static_cast<uint64_t>(9));

  const Parsed spaced = parse("  bytes = 10 - 19  ", 1000);
  REQUIRE(spaced.outcome == RangeOutcome::Satisfiable);
  CHECK_EQ(spaced.range.start, static_cast<uint64_t>(10));
  CHECK_EQ(spaced.range.end, static_cast<uint64_t>(19));
}
