/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <LibMedia/Color/ColorConverter.h>

#include "VideoFrame.h"

namespace Media {

ErrorOr<NonnullOwnPtr<SubsampledYUVFrame>> SubsampledYUVFrame::try_create(
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    bool subsampling_horizontal, bool subsampling_vertical,
    Span<u16> plane_y, Span<u16> plane_u, Span<u16> plane_v)
{
    auto plane_y_array = TRY(FixedArray<u16>::create(plane_y));
    auto plane_u_array = TRY(FixedArray<u16>::create(plane_u));
    auto plane_v_array = TRY(FixedArray<u16>::create(plane_v));
    return adopt_nonnull_own_or_enomem(new (nothrow) SubsampledYUVFrame(size, bit_depth, cicp, subsampling_horizontal, subsampling_vertical, plane_y_array, plane_u_array, plane_v_array));
}

template<u32 subsampling_horizontal>
ALWAYS_INLINE void interpolate_row(u32 const row, u32 const width, u16 const* plane_u, u16 const* plane_v, u16* __restrict__ u_row, u16* __restrict__ v_row)
{
    // OPTIMIZATION: __restrict__ allows some load eliminations because the planes and the rows will not alias.

    constexpr auto horizontal_step = 1u << subsampling_horizontal;
    auto const uv_width = (width + subsampling_horizontal) >> subsampling_horizontal;
    // Set the first column to the first chroma samples.
    u_row[0] = plane_u[row * uv_width];
    v_row[0] = plane_v[row * uv_width];

    auto const columns_end = width - subsampling_horizontal;
    // Interpolate the inner chroma columns.
    for (u32 column = 1; column < columns_end; column += horizontal_step) {
        auto uv_column = column >> subsampling_horizontal;
        u_row[column] = plane_u[row * uv_width + uv_column];
        v_row[column] = plane_v[row * uv_width + uv_column];

        if constexpr (subsampling_horizontal != 0) {
            u_row[column + 1] = (plane_u[row * uv_width + uv_column] + plane_u[row * uv_width + uv_column + 1]) >> 1;
            v_row[column + 1] = (plane_v[row * uv_width + uv_column] + plane_v[row * uv_width + uv_column + 1]) >> 1;
        }
    }

    // If there is a last chroma sample that hasn't been set above, set it now.
    if constexpr (subsampling_horizontal != 0) {
        if ((width & 1) == 0) {
            u_row[width - 1] = u_row[width - 2];
            v_row[width - 1] = v_row[width - 2];
        }
    }
}

template<u32 subsampling_horizontal, u32 subsampling_vertical, typename Convert>
ALWAYS_INLINE DecoderErrorOr<void> convert_to_bitmap_subsampled(Convert convert, u32 const width, u32 const height, FixedArray<u16> const& plane_y, FixedArray<u16> const& plane_u, FixedArray<u16> const& plane_v, Gfx::Bitmap& bitmap)
{
    VERIFY(bitmap.width() >= 0 && static_cast<u32>(bitmap.width()) == width);
    VERIFY(bitmap.height() >= 0 && static_cast<u32>(bitmap.height()) == height);

    auto temporary_buffer = DECODER_TRY_ALLOC(FixedArray<u16>::create(static_cast<size_t>(width) * 4));

    // Above rows
    auto* u_row_a = temporary_buffer.span().slice(static_cast<size_t>(width) * 0, width).data();
    auto* v_row_a = temporary_buffer.span().slice(static_cast<size_t>(width) * 1, width).data();

    // Below rows
    auto* u_row_b = temporary_buffer.span().slice(static_cast<size_t>(width) * 2, width).data();
    auto* v_row_b = temporary_buffer.span().slice(static_cast<size_t>(width) * 3, width).data();

    u32 const vertical_step = 1 << subsampling_vertical;

    interpolate_row<subsampling_horizontal>(0, width, plane_u.data(), plane_v.data(), u_row_a, v_row_a);

    // Do interpolation for all inner rows.
    u32 const rows_end = height - subsampling_vertical;
    for (u32 row = 0; row < rows_end; row += vertical_step) {
        // Horizontally scale the row if subsampled.
        auto uv_row = row >> subsampling_vertical;
        interpolate_row<subsampling_horizontal>(uv_row, width, plane_u.data(), plane_v.data(), u_row_b, v_row_b);

        // If subsampled vertically, vertically interpolate the middle row between the above and below rows.
        if constexpr (subsampling_vertical != 0) {
            // OPTIMIZATION: Splitting these two lines into separate loops enables vectorization.
            for (u32 column = 0; column < width; column++) {
                u_row_a[column] = (u_row_a[column] + u_row_b[column]) >> 1;
            }
            for (u32 column = 0; column < width; column++) {
                v_row_a[column] = (v_row_a[column] + v_row_b[column]) >> 1;
            }
        }

        auto const* y_row_a = &plane_y[static_cast<size_t>(row) * width];
        auto* scan_line_a = bitmap.scanline(static_cast<int>(row));

        for (size_t column = 0; column < width; column++) {
            scan_line_a[column] = convert(y_row_a[column], u_row_a[column], v_row_a[column]).value();
        }
        if constexpr (subsampling_vertical != 0) {
            auto const* y_row_b = &plane_y[static_cast<size_t>(row + 1) * width];
            auto* scan_line_b = bitmap.scanline(static_cast<int>(row + 1));
            for (size_t column = 0; column < width; column++) {
                scan_line_b[column] = convert(y_row_b[column], u_row_b[column], v_row_b[column]).value();
            }
        }

        AK::TypedTransfer<RemoveReference<decltype(*u_row_a)>>::move(u_row_a, u_row_b, width);
        AK::TypedTransfer<RemoveReference<decltype(*u_row_a)>>::move(v_row_a, v_row_b, width);
    }

    if constexpr (subsampling_vertical != 0) {
        // If there is a final row that hasn't been set above, convert it now.
        if ((height & 1) == 0) {
            auto const* y_row = &plane_y[static_cast<size_t>(height - 1) * width];
            auto* scan_line = bitmap.scanline(static_cast<int>(height - 1));
            for (size_t column = 0; column < width; column++) {
                scan_line[column] = convert(y_row[column], u_row_a[column], v_row_a[column]).value();
            }
        }
    }

    return {};
}

template<u32 subsampling_horizontal, u32 subsampling_vertical>
static ALWAYS_INLINE DecoderErrorOr<void> convert_to_bitmap_selecting_converter(CodingIndependentCodePoints cicp, u8 bit_depth, u32 const width, u32 const height, FixedArray<u16> const& plane_y, FixedArray<u16> const& plane_u, FixedArray<u16> const& plane_v, Gfx::Bitmap& bitmap)
{
    constexpr auto output_cicp = CodingIndependentCodePoints(ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::BT709, VideoFullRangeFlag::Full);

    if (bit_depth == 8 && cicp.transfer_characteristics() == output_cicp.transfer_characteristics() && cicp.color_primaries() == output_cicp.color_primaries() && cicp.video_full_range_flag() == VideoFullRangeFlag::Studio) {
        switch (cicp.matrix_coefficients()) {
        case MatrixCoefficients::BT709:
            return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([](u16 y, u16 u, u16 v) { return ColorConverter::convert_simple_yuv_to_rgb<MatrixCoefficients::BT709, VideoFullRangeFlag::Studio>(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
        case MatrixCoefficients::BT601:
            return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([](u16 y, u16 u, u16 v) { return ColorConverter::convert_simple_yuv_to_rgb<MatrixCoefficients::BT601, VideoFullRangeFlag::Studio>(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
        case MatrixCoefficients::BT2020ConstantLuminance:
        case MatrixCoefficients::BT2020NonConstantLuminance:
            return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([](u16 y, u16 u, u16 v) { return ColorConverter::convert_simple_yuv_to_rgb<MatrixCoefficients::BT2020ConstantLuminance, VideoFullRangeFlag::Studio>(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
        default:
            VERIFY_NOT_REACHED();
        }
    }

    auto converter = TRY(ColorConverter::create(bit_depth, cicp, output_cicp));
    return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([&](u16 y, u16 u, u16 v) { return converter.convert_yuv(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
}

static DecoderErrorOr<void> convert_to_bitmap_selecting_subsampling(bool subsampling_horizontal, bool subsampling_vertical, CodingIndependentCodePoints cicp, u8 bit_depth, u32 const width, u32 const height, FixedArray<u16> const& plane_y, FixedArray<u16> const& plane_u, FixedArray<u16> const& plane_v, Gfx::Bitmap& bitmap)
{
    if (subsampling_horizontal && subsampling_vertical) {
        return convert_to_bitmap_selecting_converter<true, true>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
    }

    if (subsampling_horizontal && !subsampling_vertical) {
        return convert_to_bitmap_selecting_converter<true, false>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
    }

    if (!subsampling_horizontal && subsampling_vertical) {
        return convert_to_bitmap_selecting_converter<false, true>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
    }

    return convert_to_bitmap_selecting_converter<false, false>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
}

DecoderErrorOr<void> SubsampledYUVFrame::output_to_bitmap(Gfx::Bitmap& bitmap)
{
    return convert_to_bitmap_selecting_subsampling(m_subsampling_horizontal, m_subsampling_vertical, cicp(), bit_depth(), width(), height(), m_plane_y, m_plane_u, m_plane_v, bitmap);
}

}
