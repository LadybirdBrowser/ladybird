/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/SkiaUtils.h>
#include <LibTest/TestCase.h>

// A graph with every filter operation once, drawing an image frame under id 42, as the Rust codec in
// LibGfx/Rust/src/filter.rs serializes it. That codec checks the same bytes, so a change to the layout on either
// side fails one of the tests.
static constexpr StringView EVERY_OPERATION_FILTER_BYTES = "01020103332211ff0000003f010000010e0000c03f000020c0010c2a0000000000000001000000020000000300000004"
                                                           "00000005000000060000000700000008000000010000000000803e0000003f0000403f0000803f130000000d04000000"
                                                           "01050000803f000000400000404000ff007f0106000080400000a0400107020000000000003f00000104011100000000"
                                                           "cdcccc3dcdcc4c3e030000000000e0401000000008000000010f0000803f0000803f0110000000400000004000000020"
                                                           "41000000000300000001090100010000fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0"
                                                           "dfdedddcdbdad9d8d7d6d5d4d3d2d1d0cfcecdcccbcac9c8c7c6c5c4c3c2c1c0bfbebdbcbbbab9b8b7b6b5b4b3b2b1b0"
                                                           "afaeadacabaaa9a8a7a6a5a4a3a2a1a09f9e9d9c9b9a999897969594939291908f8e8d8c8b8a89888786858483828180"
                                                           "7f7e7d7c7b7a797877767574737271706f6e6d6c6b6a696867666564636261605f5e5d5c5b5a59585756555453525150"
                                                           "4f4e4d4c4b4a494847464544434241403f3e3d3c3b3a393837363534333231302f2e2d2c2b2a29282726252423222120"
                                                           "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a090807060504030201000000000108000000000000003f000080"
                                                           "3f0000c03f0000004000002040000040400000604000008040000090400000a0400000b0400000c0400000d0400000e0"
                                                           "400000f04000000041000008410000104100001841010a9a99993e010b0000b4420112010000000000000000"sv;

static constexpr u64 FIXTURE_IMAGE_FRAME_ID = 42;

static ByteBuffer every_operation_filter_bytes()
{
    return MUST(decode_hex(EVERY_OPERATION_FILTER_BYTES));
}

TEST_CASE(the_shared_bytes_name_the_image_frames_they_draw)
{
    auto bytes = every_operation_filter_bytes();
    Vector<u64> image_frame_ids;
    Gfx::for_each_filter_image_frame_id(bytes.bytes(), [&](u64 id) { image_frame_ids.append(id); });
    EXPECT_EQ(image_frame_ids.size(), 1u);
    EXPECT_EQ(image_frame_ids.first(), FIXTURE_IMAGE_FRAME_ID);

    image_frame_ids.clear();
    Gfx::for_each_filter_image_frame_id(bytes.bytes().trim(bytes.size() - 1), [&](u64 id) { image_frame_ids.append(id); });
    EXPECT(image_frame_ids.is_empty());
}

TEST_CASE(the_shared_bytes_build_a_skia_image_filter)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 1, 1 }));
    Gfx::DecodedImageFrame frame { *bitmap };
    auto bytes = every_operation_filter_bytes();
    size_t image_lookups = 0;
    auto image_filter = Gfx::to_skia_image_filter(bytes.bytes(), [&](u64 id) -> Gfx::DecodedImageFrame const& {
        EXPECT_EQ(id, FIXTURE_IMAGE_FRAME_ID);
        ++image_lookups;
        return frame;
    });
    EXPECT(image_filter != nullptr);
    EXPECT_EQ(image_lookups, 1u);
}
