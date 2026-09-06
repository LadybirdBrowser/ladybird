/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Hex.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/SkiaUtils.h>
#include <LibTest/TestCase.h>

// The serialized form of every_operation_filter(), with its image frame under id 42. The Rust codec in
// LibGfx/Rust/src/filter.rs checks the same bytes, so the two sides cannot drift apart without one of the tests
// noticing.
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

// Every filter operation once, nested so that each input position is exercised.
static Gfx::Filter every_operation_filter(Gfx::DecodedImageFrame const& frame)
{
    auto image = Gfx::Filter::image(frame, { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, Gfx::ScalingMode::Bilinear);
    auto offset = Gfx::Filter::offset(1.5f, -2.5f, image);
    auto arithmetic = Gfx::Filter::arithmetic({}, offset, 0.25f, 0.5f, 0.75f, 1.0f);
    auto flood = Gfx::Filter::flood(Gfx::Color(0x11, 0x22, 0x33, 0xff), 0.5f);
    auto blend = Gfx::Filter::blend(flood, arithmetic, Gfx::CompositingAndBlendingOperator::SourceOver);

    auto color = Gfx::Filter::color(Gfx::ColorFilterType::Grayscale, 0.5f);
    auto blur = Gfx::Filter::blur(4.0f, 5.0f, color);
    auto drop_shadow = Gfx::Filter::drop_shadow(1.0f, 2.0f, 3.0f, Gfx::Color(0x00, 0xff, 0x00, 0x7f), blur);

    auto turbulence = Gfx::Filter::turbulence(Gfx::TurbulenceType::FractalNoise, 0.1f, 0.2f, 3, 7.0f, { 16, 8 });
    auto dilate = Gfx::Filter::dilate(2.0f, 2.0f, {});
    auto erode = Gfx::Filter::erode(1.0f, 1.0f, dilate);
    auto displacement_map = Gfx::Filter::displacement_map(turbulence, erode, 10.0f, Gfx::ChannelSelector::Red, Gfx::ChannelSelector::Alpha);

    auto color_space_conversion = Gfx::Filter::convert_interpolation_color_space(Gfx::InterpolationColorSpace::SRGB, Gfx::InterpolationColorSpace::LinearRGB);
    auto hue_rotate = Gfx::Filter::hue_rotate(90.0f, color_space_conversion);
    auto saturate = Gfx::Filter::saturate(0.3f, hue_rotate);
    float matrix[20];
    for (size_t i = 0; i < 20; ++i)
        matrix[i] = static_cast<float>(i) * 0.5f;
    auto color_matrix = Gfx::Filter::color_matrix(matrix, saturate);
    Array<u8, 256> table;
    for (size_t i = 0; i < table.size(); ++i)
        table[i] = static_cast<u8>(255 - i);
    auto color_table = Gfx::Filter::color_table(table.span(), {}, {}, {}, color_matrix);

    Vector<Optional<Gfx::Filter>> merge_inputs;
    merge_inputs.append(drop_shadow);
    merge_inputs.append({});
    merge_inputs.append(displacement_map);
    merge_inputs.append(color_table);
    auto merge = Gfx::Filter::merge(merge_inputs);

    return Gfx::Filter::compose(blend, merge);
}

static Gfx::DecodedImageFrame test_frame()
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 1, 1 }));
    return Gfx::DecodedImageFrame { *bitmap };
}

// The fixture bytes with their image frame renamed to the id `frame` was handed out at runtime.
static ByteBuffer expected_bytes_for(Gfx::DecodedImageFrame const& frame)
{
    auto bytes = MUST(decode_hex(EVERY_OPERATION_FILTER_BYTES));
    u64 fixture_id = FIXTURE_IMAGE_FRAME_ID;
    ReadonlyBytes fixture_id_bytes { &fixture_id, sizeof(fixture_id) };
    Optional<size_t> id_offset;
    for (size_t offset = 0; offset + sizeof(fixture_id) <= bytes.size(); ++offset) {
        if (bytes.bytes().slice(offset, sizeof(fixture_id)) != fixture_id_bytes)
            continue;
        VERIFY(!id_offset.has_value());
        id_offset = offset;
    }
    VERIFY(id_offset.has_value());
    u64 frame_id = frame.id();
    bytes.overwrite(*id_offset, &frame_id, sizeof(frame_id));
    return bytes;
}

TEST_CASE(every_operation_serializes_to_the_shared_bytes)
{
    auto frame = test_frame();
    auto filter = every_operation_filter(frame);
    auto expected = expected_bytes_for(frame);
    EXPECT_EQ(filter.serialized_bytes(), expected.bytes());
    EXPECT_EQ(filter.image_frames().size(), 1u);
    EXPECT_EQ(filter.image_frames()[0].id, frame.id());
    EXPECT_EQ(filter.image_frame(frame.id()).id(), frame.id());
}

TEST_CASE(the_shared_bytes_name_the_image_frames_they_draw)
{
    auto frame = test_frame();
    auto expected = expected_bytes_for(frame);
    Vector<u64> image_frame_ids;
    Gfx::for_each_filter_image_frame_id(expected.bytes(), [&](u64 id) { image_frame_ids.append(id); });
    EXPECT_EQ(image_frame_ids.size(), 1u);
    EXPECT_EQ(image_frame_ids.first(), frame.id());

    image_frame_ids.clear();
    Gfx::for_each_filter_image_frame_id(expected.bytes().trim(expected.size() - 1), [&](u64 id) { image_frame_ids.append(id); });
    EXPECT(image_frame_ids.is_empty());
}

TEST_CASE(the_shared_bytes_build_a_skia_image_filter)
{
    auto frame = test_frame();
    auto expected = expected_bytes_for(frame);
    size_t image_lookups = 0;
    auto image_filter = Gfx::to_skia_image_filter(expected.bytes(), [&](u64 id) -> Gfx::DecodedImageFrame const& {
        EXPECT_EQ(id, frame.id());
        ++image_lookups;
        return frame;
    });
    EXPECT(image_filter != nullptr);
    EXPECT_EQ(image_lookups, 1u);
    EXPECT(Gfx::to_skia_image_filter(every_operation_filter(frame)) != nullptr);
}
