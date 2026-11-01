#include <cstdint>
#include <vector>

#include "bitmap.h"
#include "test_helpers.h"

using fsrv::ChunkBitmap;

TEST_CASE(bitmap_empty_session) {
  ChunkBitmap bm(0);
  CHECK_EQ(bm.chunks(), static_cast<uint32_t>(0));
  CHECK_EQ(bm.count(), static_cast<uint32_t>(0));
  CHECK(bm.full());
  CHECK(bm.indices().empty());
  CHECK(bm.missing().empty());
  CHECK(bm.blob().empty());
  CHECK(!bm.test(0));
}

TEST_CASE(bitmap_set_test_and_lists) {
  ChunkBitmap bm(10);
  CHECK_EQ(bm.missing().size(), static_cast<size_t>(10));
  CHECK(!bm.full());

  bm.set(0);
  bm.set(9);
  CHECK(bm.test(0));
  CHECK(bm.test(9));
  CHECK(!bm.test(5));
  CHECK_EQ(bm.count(), static_cast<uint32_t>(2));
  CHECK(bm.indices() == (std::vector<uint32_t>{0, 9}));

  const std::vector<uint32_t> missing = bm.missing();
  REQUIRE(missing.size() == static_cast<size_t>(8));
  CHECK_EQ(missing.front(), static_cast<uint32_t>(1));
  CHECK_EQ(missing.back(), static_cast<uint32_t>(8));

  bm.clear(0);
  CHECK(!bm.test(0));
  CHECK_EQ(bm.count(), static_cast<uint32_t>(1));
}

TEST_CASE(bitmap_set_all_is_full) {
  ChunkBitmap bm(37);
  bm.set_all();
  CHECK(bm.full());
  CHECK_EQ(bm.count(), static_cast<uint32_t>(37));
  CHECK(bm.missing().empty());
  CHECK_EQ(bm.blob().size(), static_cast<size_t>((37 + 7) / 8));
}

TEST_CASE(bitmap_out_of_range_set_throws) {
  ChunkBitmap bm(8);
  CHECK_STATUS(bm.set(8), 400);
  CHECK_STATUS(bm.set(1000), 400);
  // Clearing or testing out of range is a harmless no-op.
  bm.clear(8);
  CHECK(!bm.test(8));
  CHECK_EQ(bm.count(), static_cast<uint32_t>(0));
}

TEST_CASE(bitmap_blob_round_trip) {
  ChunkBitmap original(23);
  original.set(0);
  original.set(5);
  original.set(22);

  const ChunkBitmap restored = ChunkBitmap::from_blob(original.blob(), 23);
  CHECK_EQ(restored.chunks(), static_cast<uint32_t>(23));
  CHECK_EQ(restored.count(), static_cast<uint32_t>(3));
  CHECK(restored.test(0));
  CHECK(restored.test(5));
  CHECK(restored.test(22));
  CHECK(!restored.test(6));
}

TEST_CASE(bitmap_from_blob_tolerates_wrong_sizes) {
  const std::vector<uint8_t> short_blob{0xFF};
  const ChunkBitmap short_bm = ChunkBitmap::from_blob(short_blob, 16);
  CHECK_EQ(short_bm.count(), static_cast<uint32_t>(8));
  CHECK(short_bm.test(7));
  CHECK(!short_bm.test(8));

  const std::vector<uint8_t> long_blob{0x01, 0x00, 0x01, 0x00};
  const ChunkBitmap long_bm = ChunkBitmap::from_blob(long_blob, 8);
  CHECK_EQ(long_bm.count(), static_cast<uint32_t>(1));
  CHECK(long_bm.test(0));

  const ChunkBitmap empty_bm = ChunkBitmap::from_blob({}, 8);
  CHECK_EQ(empty_bm.count(), static_cast<uint32_t>(0));
}

TEST_CASE(bitmap_partial_last_byte) {
  // 9 chunks -> second byte holds a single bit.
  ChunkBitmap bm(9);
  bm.set(8);
  CHECK_EQ(bm.count(), static_cast<uint32_t>(1));
  CHECK_EQ(bm.blob().size(), static_cast<size_t>(2));
  CHECK(bm.full() == false);
  bm.set_all();
  CHECK(bm.full());
}
