/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/YUVData.h>
#include <LibTest/TestCase.h>

namespace {

enum class SampleAlignment : u8 {
    LowBits,
    HighBits,
};

void write_sample(ByteBuffer& plane, size_t offset, u16 value, u8 bit_depth, SampleAlignment alignment)
{
    if (bit_depth <= 8) {
        plane[offset] = static_cast<u8>(value);
        return;
    }
    if (alignment == SampleAlignment::HighBits)
        value <<= 16 - bit_depth;
    plane[offset] = static_cast<u8>(value & 0xff);
    plane[offset + 1] = static_cast<u8>(value >> 8);
}

bool colors_are_close(Gfx::Color a, Gfx::Color b)
{
    auto close = [](u8 x, u8 y) { return (x > y ? x - y : y - x) <= 1; };
    return close(a.red(), b.red()) && close(a.green(), b.green()) && close(a.blue(), b.blue()) && a.alpha() == b.alpha();
}

constexpr size_t PLANE_STRIDE_IN_SAMPLES = 16;
constexpr u8 PADDING_BYTE = 0xee;

}

// Every luma row and every chroma row holds samples of its own. A row that is left out, or that takes its chroma from
// another row, then converts to different pixels than the planar conversion of the same samples.
TEST_CASE(biplanar_conversion_matches_planar_conversion)
{
    for (int height : { 1, 2, 3, 4, 5 }) {
        Gfx::IntSize size { 5, height };
        for (auto subsampling : { Media::Subsampling::yuv420(), Media::Subsampling::yuv422(), Media::Subsampling::yuv444() }) {
            for (u8 bit_depth : { 8, 10 }) {
                size_t bytes_per_sample = bit_depth > 8 ? 2 : 1;
                u16 scale = bit_depth > 8 ? 4 : 1;
                size_t stride = PLANE_STRIDE_IN_SAMPLES * bytes_per_sample;
                auto chroma_size = subsampling.subsampled_size(size.to_type<size_t>());

                auto luma_sample = [&](size_t x, size_t y) { return static_cast<u16>((60 + (y * 25) + (x * 4)) * scale); };
                auto blue_chroma_sample = [&](size_t x, size_t y) { return static_cast<u16>((110 + (y * 10) + (x * 3)) * scale); };
                auto red_chroma_sample = [&](size_t x, size_t y) { return static_cast<u16>((150 - (y * 10) + (x * 2)) * scale); };

                auto biplanar_luma = MUST(ByteBuffer::create_uninitialized(stride * size.height()));
                auto biplanar_chroma = MUST(ByteBuffer::create_uninitialized(stride * chroma_size.height()));
                biplanar_luma.bytes().fill(PADDING_BYTE);
                biplanar_chroma.bytes().fill(PADDING_BYTE);

                auto plane_sizes = MUST(Gfx::YUVData::plane_sizes(size, bit_depth, subsampling));
                auto planar_luma = MUST(ByteBuffer::create_zeroed(plane_sizes.y));
                auto planar_blue_chroma = MUST(ByteBuffer::create_zeroed(plane_sizes.u));
                auto planar_red_chroma = MUST(ByteBuffer::create_zeroed(plane_sizes.v));

                for (size_t y = 0; y < static_cast<size_t>(size.height()); y++) {
                    for (size_t x = 0; x < static_cast<size_t>(size.width()); x++) {
                        auto sample = luma_sample(x, y);
                        write_sample(biplanar_luma, (y * stride) + (x * bytes_per_sample), sample, bit_depth, SampleAlignment::HighBits);
                        write_sample(planar_luma, ((y * size.width()) + x) * bytes_per_sample, sample, bit_depth, SampleAlignment::LowBits);
                    }
                }
                for (size_t y = 0; y < chroma_size.height(); y++) {
                    for (size_t x = 0; x < chroma_size.width(); x++) {
                        auto blue = blue_chroma_sample(x, y);
                        auto red = red_chroma_sample(x, y);
                        auto biplanar_offset = (y * stride) + (x * 2 * bytes_per_sample);
                        write_sample(biplanar_chroma, biplanar_offset, blue, bit_depth, SampleAlignment::HighBits);
                        write_sample(biplanar_chroma, biplanar_offset + bytes_per_sample, red, bit_depth, SampleAlignment::HighBits);
                        auto planar_offset = ((y * chroma_size.width()) + x) * bytes_per_sample;
                        write_sample(planar_blue_chroma, planar_offset, blue, bit_depth, SampleAlignment::LowBits);
                        write_sample(planar_red_chroma, planar_offset, red, bit_depth, SampleAlignment::LowBits);
                    }
                }

                auto biplanar = MUST(Gfx::biplanar_yuv_to_bitmap(size, bit_depth, subsampling, {},
                    { .data = biplanar_luma.bytes(), .stride = stride },
                    { .data = biplanar_chroma.bytes(), .stride = stride }));
                auto planar = MUST(MUST(Gfx::YUVData::create(size, bit_depth, subsampling, {},
                                            planar_luma.bytes(), planar_blue_chroma.bytes(), planar_red_chroma.bytes()))
                        .to_bitmap());

                for (int y = 0; y < size.height(); y++) {
                    for (int x = 0; x < size.width(); x++) {
                        auto biplanar_color = biplanar->get_pixel(x, y);
                        auto planar_color = planar->get_pixel(x, y);
                        if (!colors_are_close(biplanar_color, planar_color)) {
                            FAIL(ByteString::formatted("{}x{} at {} bits, chroma {}x{}: pixel ({}, {}) is {}, but the planar conversion gives {}",
                                size.width(), size.height(), bit_depth, chroma_size.width(), chroma_size.height(), x, y, biplanar_color, planar_color));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE(biplanar_conversion_refuses_planes_whose_last_row_is_not_padded)
{
    constexpr Gfx::IntSize size { 5, 3 };
    constexpr size_t stride = PLANE_STRIDE_IN_SAMPLES;

    auto luma = MUST(ByteBuffer::create_zeroed(stride * size.height()));
    auto chroma = MUST(ByteBuffer::create_zeroed(stride * 2));
    Gfx::BiplanarYUVPlane padded_luma { .data = luma.bytes(), .stride = stride };
    Gfx::BiplanarYUVPlane padded_chroma { .data = chroma.bytes(), .stride = stride };
    Gfx::BiplanarYUVPlane unpadded_luma { .data = luma.bytes().trim((stride * (size.height() - 1)) + size.width()), .stride = stride };
    Gfx::BiplanarYUVPlane unpadded_chroma { .data = chroma.bytes().trim(stride + 6), .stride = stride };

    EXPECT(!Gfx::biplanar_yuv_to_bitmap(size, 8, Media::Subsampling::yuv420(), {}, padded_luma, padded_chroma).is_error());
    EXPECT(Gfx::biplanar_yuv_to_bitmap(size, 8, Media::Subsampling::yuv420(), {}, unpadded_luma, padded_chroma).is_error());
    EXPECT(Gfx::biplanar_yuv_to_bitmap(size, 8, Media::Subsampling::yuv420(), {}, padded_luma, unpadded_chroma).is_error());
}
