/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/YUVData.h>
#include <LibTest/TestCase.h>

static void put_sample(ByteBuffer& plane, size_t offset, u16 value, u8 bit_depth, bool in_high_bits)
{
    if (bit_depth <= 8) {
        plane[offset] = static_cast<u8>(value);
        return;
    }
    if (in_high_bits)
        value <<= 16 - bit_depth;
    plane[offset] = static_cast<u8>(value & 0xff);
    plane[offset + 1] = static_cast<u8>(value >> 8);
}

static bool colors_match(Gfx::Color a, Gfx::Color b)
{
    auto close = [](u8 x, u8 y) { return (x > y ? x - y : y - x) <= 1; };
    return close(a.red(), b.red()) && close(a.green(), b.green()) && close(a.blue(), b.blue()) && a.alpha() == b.alpha();
}

// Only the rows before a plane's last one have to be padded up to its stride, so a plane can end right after its last
// sample. Such a plane must convert to the same pixels as a padded one, and, down to its last row and its last row's
// chroma, to what the planar conversion makes of the same samples, whether the height is even or odd.
TEST_CASE(biplanar_conversion_reads_planes_whose_last_row_is_not_padded)
{
    for (int height : { 1, 2, 3, 4, 5 }) {
        Gfx::IntSize size { 5, height };
        for (auto subsampling : { Media::Subsampling::yuv420(), Media::Subsampling::yuv422(), Media::Subsampling::yuv444() }) {
            for (u8 bit_depth : { 8, 10 }) {
                size_t bytes_per_sample = bit_depth > 8 ? 2 : 1;
                u16 scale = bit_depth > 8 ? 4 : 1;
                size_t stride = 16 * bytes_per_sample;
                auto chroma_size = subsampling.subsampled_size(size.to_type<size_t>());
                auto luma_row_byte_count = size.width() * bytes_per_sample;
                auto chroma_row_byte_count = chroma_size.width() * 2 * bytes_per_sample;

                // Every row, and every row of chroma, holds samples of its own, so a row that's left out or that takes
                // another row's chroma converts to different pixels.
                auto luma_sample = [&](size_t x, size_t y) { return static_cast<u16>((40 + (y * 37) + (x * 3)) * scale); };
                auto cb_sample = [&](size_t x, size_t y) { return static_cast<u16>((90 + (y * 29) + (x * 5)) * scale); };
                auto cr_sample = [&](size_t x, size_t y) { return static_cast<u16>((170 - (y * 23) + (x * 2)) * scale); };

                auto padded_luma = MUST(ByteBuffer::create_zeroed(stride * size.height()));
                auto padded_chroma = MUST(ByteBuffer::create_zeroed(stride * chroma_size.height()));
                auto sizes = MUST(Gfx::YUVData::plane_sizes(size, bit_depth, subsampling));
                auto planar_y = MUST(ByteBuffer::create_zeroed(sizes.y));
                auto planar_u = MUST(ByteBuffer::create_zeroed(sizes.u));
                auto planar_v = MUST(ByteBuffer::create_zeroed(sizes.v));
                for (size_t y = 0; y < static_cast<size_t>(size.height()); y++) {
                    for (size_t x = 0; x < static_cast<size_t>(size.width()); x++) {
                        put_sample(padded_luma, (y * stride) + (x * bytes_per_sample), luma_sample(x, y), bit_depth, true);
                        put_sample(planar_y, ((y * size.width()) + x) * bytes_per_sample, luma_sample(x, y), bit_depth, false);
                    }
                }
                for (size_t y = 0; y < chroma_size.height(); y++) {
                    for (size_t x = 0; x < chroma_size.width(); x++) {
                        put_sample(padded_chroma, (y * stride) + (x * 2 * bytes_per_sample), cb_sample(x, y), bit_depth, true);
                        put_sample(padded_chroma, (y * stride) + ((x * 2 + 1) * bytes_per_sample), cr_sample(x, y), bit_depth, true);
                        put_sample(planar_u, ((y * chroma_size.width()) + x) * bytes_per_sample, cb_sample(x, y), bit_depth, false);
                        put_sample(planar_v, ((y * chroma_size.width()) + x) * bytes_per_sample, cr_sample(x, y), bit_depth, false);
                    }
                }

                // The unpadded planes are copies in buffers of their own, which end right after their last sample.
                auto unpadded_luma = MUST(ByteBuffer::copy(padded_luma.bytes().trim((stride * (size.height() - 1)) + luma_row_byte_count)));
                auto unpadded_chroma = MUST(ByteBuffer::copy(padded_chroma.bytes().trim((stride * (chroma_size.height() - 1)) + chroma_row_byte_count)));

                auto padded = MUST(Gfx::biplanar_yuv_to_bitmap(size, bit_depth, subsampling, {}, { .data = padded_luma.bytes(), .stride = stride }, { .data = padded_chroma.bytes(), .stride = stride }));
                auto unpadded = MUST(Gfx::biplanar_yuv_to_bitmap(size, bit_depth, subsampling, {}, { .data = unpadded_luma.bytes(), .stride = stride }, { .data = unpadded_chroma.bytes(), .stride = stride }));
                auto planar = MUST(MUST(Gfx::YUVData::create(size, bit_depth, subsampling, {}, planar_y.bytes(), planar_u.bytes(), planar_v.bytes())).to_bitmap());
                for (int y = 0; y < size.height(); y++) {
                    for (int x = 0; x < size.width(); x++) {
                        if (unpadded->get_pixel(x, y) != padded->get_pixel(x, y))
                            FAIL(ByteString::formatted("{}x{} at {} bits, subsampled {}x{}: pixel ({}, {}) differs from the padded planes'", size.width(), size.height(), bit_depth, subsampling.x(), subsampling.y(), x, y));
                        if (!colors_match(unpadded->get_pixel(x, y), planar->get_pixel(x, y)))
                            FAIL(ByteString::formatted("{}x{} at {} bits, subsampled {}x{}: pixel ({}, {}) is {}, but the planar conversion gives {}", size.width(), size.height(), bit_depth, subsampling.x(), subsampling.y(), x, y, unpadded->get_pixel(x, y), planar->get_pixel(x, y)));
                    }
                }
            }
        }
    }
}
