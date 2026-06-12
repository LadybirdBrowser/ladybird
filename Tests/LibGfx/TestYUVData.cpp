/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/FixedArray.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/YUVData.h>
#include <LibTest/TestCase.h>

#include <core/SkYUVAPixmaps.h>

struct AllocatedPlanes {
    FixedArray<u8> storage;
    Gfx::YUVData data;
};

static ErrorOr<AllocatedPlanes> allocate_yuv_data(Gfx::IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp)
{
    auto sizes = TRY(Gfx::YUVData::plane_sizes(size, bit_depth, subsampling));
    auto storage = TRY(FixedArray<u8>::create(sizes.total));

    auto y_data = storage.span().slice(0, sizes.y);
    auto u_data = storage.span().slice(sizes.y, sizes.u);
    auto v_data = storage.span().slice(sizes.y + sizes.u, sizes.v);
    auto data = TRY(Gfx::YUVData::create(size, bit_depth, subsampling, cicp, y_data, u_data, v_data));

    return AllocatedPlanes {
        .storage = move(storage),
        .data = data,
    };
}

static u16 expand_10_bit_sample_to_full_16_bit_range(u16 sample)
{
    return static_cast<u16>((sample << 6) | (sample >> 4));
}

template<size_t Size>
static void write_samples(Bytes plane, Array<u16, Size> const& samples)
{
    auto* destination = reinterpret_cast<u16*>(plane.data());
    for (size_t i = 0; i < Size; i++)
        destination[i] = samples[i];
}

template<size_t Size>
static void expect_samples(Bytes plane, Array<u16, Size> const& expected)
{
    auto const* samples = reinterpret_cast<u16 const*>(plane.data());
    for (size_t i = 0; i < Size; i++)
        EXPECT_EQ(samples[i], expected[i]);
}

template<size_t Size>
static void expect_expanded_pixmap_samples(SkPixmap const& plane, Array<u16, Size> const& native_samples, int width)
{
    for (size_t i = 0; i < Size; i++) {
        auto x = static_cast<int>(i % static_cast<size_t>(width));
        auto y = static_cast<int>(i / static_cast<size_t>(width));
        EXPECT_EQ(*plane.addr16(x, y), expand_10_bit_sample_to_full_16_bit_range(native_samples[i]));
    }
}

TEST_CASE(high_bit_depth_pixmaps_expand_without_mutating_native_samples)
{
    auto const cicp = Media::CodingIndependentCodePoints {
        Media::ColorPrimaries::BT709,
        Media::TransferCharacteristics::BT709,
        Media::MatrixCoefficients::BT709,
        Media::VideoFullRangeFlag::Full,
    };
    auto allocated_planes = TRY_OR_FAIL(allocate_yuv_data({ 2, 2 }, 10, Media::Subsampling { false, false }, cicp));
    auto& yuv_data = allocated_planes.data;

    Array<u16, 4> const y_samples { 0, 341, 682, 1023 };
    Array<u16, 4> const u_samples { 512, 513, 514, 515 };
    Array<u16, 4> const v_samples { 508, 509, 510, 511 };

    write_samples(yuv_data.y_data(), y_samples);
    write_samples(yuv_data.u_data(), u_samples);
    write_samples(yuv_data.v_data(), v_samples);

    auto bitmap_before = TRY_OR_FAIL(yuv_data.to_bitmap());

    auto pixmaps = yuv_data.make_pixmaps();
    EXPECT(pixmaps.isValid());
    EXPECT(pixmaps.ownsStorage());
    EXPECT(pixmaps.dataType() == SkYUVAPixmapInfo::DataType::kUnorm16);
    EXPECT_EQ(pixmaps.numPlanes(), 3);

    expect_expanded_pixmap_samples(pixmaps.plane(0), y_samples, 2);
    expect_expanded_pixmap_samples(pixmaps.plane(1), u_samples, 2);
    expect_expanded_pixmap_samples(pixmaps.plane(2), v_samples, 2);

    EXPECT_EQ(yuv_data.bit_depth(), 10);
    expect_samples(yuv_data.y_data(), y_samples);
    expect_samples(yuv_data.u_data(), u_samples);
    expect_samples(yuv_data.v_data(), v_samples);

    auto bitmap_after = TRY_OR_FAIL(yuv_data.to_bitmap());
    for (int y = 0; y < yuv_data.size().height(); y++) {
        for (int x = 0; x < yuv_data.size().width(); x++)
            EXPECT_EQ(bitmap_after->get_pixel(x, y), bitmap_before->get_pixel(x, y));
    }
}
