/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebGL/TextureUpload.h>

static constexpr Web::WebGL::GLenum UNSIGNED_BYTE = 0x1401;
static constexpr Web::WebGL::GLenum RGB = 0x1907;
static constexpr Web::WebGL::GLenum RGBA = 0x1908;
static constexpr Web::WebGL::GLenum UNSIGNED_SHORT_5_6_5 = 0x8363;

TEST_CASE(required_2d_texture_data_size_uses_dimensions)
{
    auto size = Web::WebGL::required_2d_texture_data_size(16, 16, RGB, UNSIGNED_BYTE, {});
    EXPECT(size.has_value());
    EXPECT_EQ(*size, 768u);
}

TEST_CASE(required_2d_texture_data_size_includes_row_alignment)
{
    auto size = Web::WebGL::required_2d_texture_data_size(3, 2, RGB, UNSIGNED_BYTE, {});
    EXPECT(size.has_value());
    EXPECT_EQ(*size, 21u);
}

TEST_CASE(required_2d_texture_data_size_includes_unpack_state)
{
    Web::WebGL::PixelUnpackState unpack_state {
        .alignment = 8,
        .row_length = 10,
        .skip_pixels = 2,
        .skip_rows = 3,
    };
    auto size = Web::WebGL::required_2d_texture_data_size(4, 2, RGBA, UNSIGNED_BYTE, unpack_state);
    EXPECT(size.has_value());
    EXPECT_EQ(*size, 184u);
}

TEST_CASE(required_2d_texture_data_size_handles_packed_types)
{
    auto size = Web::WebGL::required_2d_texture_data_size(5, 2, RGB, UNSIGNED_SHORT_5_6_5, {});
    EXPECT(size.has_value());
    EXPECT_EQ(*size, 22u);
}

TEST_CASE(required_2d_texture_data_size_rejects_overlapping_rows)
{
    Web::WebGL::PixelUnpackState unpack_state {
        .row_length = 8,
        .skip_pixels = 5,
    };
    auto size = Web::WebGL::required_2d_texture_data_size(4, 2, RGBA, UNSIGNED_BYTE, unpack_state);
    EXPECT(!size.has_value());
}

TEST_CASE(required_2d_texture_data_size_rejects_skip_overflow)
{
    Web::WebGL::PixelUnpackState unpack_state {
        .row_length = NumericLimits<size_t>::max(),
        .skip_pixels = NumericLimits<size_t>::max(),
    };
    auto size = Web::WebGL::required_2d_texture_data_size(1, 1, RGBA, UNSIGNED_BYTE, unpack_state);
    EXPECT(!size.has_value());
}

TEST_CASE(required_2d_texture_data_size_rejects_skipped_rows_size_overflow)
{
    Web::WebGL::PixelUnpackState unpack_state {
        .skip_rows = NumericLimits<size_t>::max(),
    };
    auto size = Web::WebGL::required_2d_texture_data_size(1, 1, RGBA, UNSIGNED_BYTE, unpack_state);
    EXPECT(!size.has_value());
}

TEST_CASE(required_2d_texture_data_size_rejects_required_size_overflow)
{
    Web::WebGL::PixelUnpackState unpack_state {
        .row_length = 2,
        .skip_pixels = 1,
        .skip_rows = NumericLimits<size_t>::max() / 8,
    };
    auto size = Web::WebGL::required_2d_texture_data_size(1, 1, RGBA, UNSIGNED_BYTE, unpack_state);
    EXPECT(!size.has_value());
}
