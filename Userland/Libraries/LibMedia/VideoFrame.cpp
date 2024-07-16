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
    Duration timestamp,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    Subsampling subsampling)
{
    VERIFY(bit_depth < 16);
    size_t component_size = bit_depth > 8 ? sizeof(u16) : sizeof(u8);
    size_t alignment_size = max(bit_depth > 8 ? sizeof(u16) : sizeof(u8), sizeof(void*));

    auto alloc_buffer = [&](size_t size) -> ErrorOr<u8*> {
        void* buffer = nullptr;
        auto result = posix_memalign(&buffer, alignment_size, size);
        if (result != 0)
            return Error::from_errno(result);
        return reinterpret_cast<u8*>(buffer);
    };

    auto y_data_size = size.to_type<size_t>().area() * component_size;
    auto uv_data_size = subsampling.subsampled_size(size).to_type<size_t>().area() * component_size;
    auto* y_buffer = TRY(alloc_buffer(y_data_size));
    auto* u_buffer = TRY(alloc_buffer(uv_data_size));
    auto* v_buffer = TRY(alloc_buffer(uv_data_size));

    return adopt_nonnull_own_or_enomem(new (nothrow) SubsampledYUVFrame(timestamp, size, bit_depth, cicp, subsampling, y_buffer, u_buffer, v_buffer));
}

ErrorOr<NonnullOwnPtr<SubsampledYUVFrame>> SubsampledYUVFrame::try_create_from_data(
    Duration timestamp,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    Subsampling subsampling,
    ReadonlyBytes y_data, ReadonlyBytes u_data, ReadonlyBytes v_data)
{
    auto frame = TRY(try_create(timestamp, size, bit_depth, cicp, subsampling));

    size_t component_size = bit_depth > 8 ? sizeof(u16) : sizeof(u8);
    auto y_data_size = size.to_type<size_t>().area() * component_size;
    auto uv_data_size = subsampling.subsampled_size(size).to_type<size_t>().area() * component_size;

    VERIFY(y_data.size() >= y_data_size);
    VERIFY(u_data.size() >= uv_data_size);
    VERIFY(v_data.size() >= uv_data_size);

    memcpy(frame->m_y_buffer, y_data.data(), y_data_size);
    memcpy(frame->m_u_buffer, u_data.data(), uv_data_size);
    memcpy(frame->m_v_buffer, v_data.data(), uv_data_size);
    return frame;
}

SubsampledYUVFrame::~SubsampledYUVFrame()
{
    free(m_y_buffer);
    free(m_u_buffer);
    free(m_v_buffer);
}

template<u32 subsampling_horizontal, typename T>
ALWAYS_INLINE void interpolate_row(u32 const row, u32 const width, T const* plane_u, T const* plane_v, T* __restrict__ u_row, T* __restrict__ v_row)
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

template<u32 subsampling_horizontal, u32 subsampling_vertical, typename T, typename Convert>
ALWAYS_INLINE DecoderErrorOr<void> convert_to_bitmap_subsampled(Convert convert, u32 const width, u32 const height, T const* plane_y, T const* plane_u, T const* plane_v, Gfx::Bitmap& bitmap)
{
    VERIFY(bitmap.width() >= 0);
    VERIFY(bitmap.height() >= 0);
    VERIFY(static_cast<u32>(bitmap.width()) == width);
    VERIFY(static_cast<u32>(bitmap.height()) == height);

    auto temporary_buffer = DECODER_TRY_ALLOC(FixedArray<T>::create(static_cast<size_t>(width) * 4));

    // Above rows
    auto* u_row_a = temporary_buffer.span().slice(static_cast<size_t>(width) * 0, width).data();
    auto* v_row_a = temporary_buffer.span().slice(static_cast<size_t>(width) * 1, width).data();

    // Below rows
    auto* u_row_b = temporary_buffer.span().slice(static_cast<size_t>(width) * 2, width).data();
    auto* v_row_b = temporary_buffer.span().slice(static_cast<size_t>(width) * 3, width).data();

    u32 const vertical_step = 1 << subsampling_vertical;

    interpolate_row<subsampling_horizontal>(0, width, plane_u, plane_v, u_row_a, v_row_a);

    // Do interpolation for all inner rows.
    u32 const rows_end = height - subsampling_vertical;
    for (u32 row = 0; row < rows_end; row += vertical_step) {
        // Horizontally scale the row if subsampled.
        auto uv_row = row >> subsampling_vertical;
        interpolate_row<subsampling_horizontal>(uv_row, width, plane_u, plane_v, u_row_b, v_row_b);

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

template<u32 subsampling_horizontal, u32 subsampling_vertical, typename T>
static ALWAYS_INLINE DecoderErrorOr<void> convert_to_bitmap_selecting_converter(CodingIndependentCodePoints cicp, u8 bit_depth, u32 const width, u32 const height, void* plane_y_data, void* plane_u_data, void* plane_v_data, Gfx::Bitmap& bitmap)
{
    auto const* plane_y = reinterpret_cast<T const*>(plane_y_data);
    auto const* plane_u = reinterpret_cast<T const*>(plane_u_data);
    auto const* plane_v = reinterpret_cast<T const*>(plane_v_data);

    constexpr auto output_cicp = CodingIndependentCodePoints(ColorPrimaries::BT709, TransferCharacteristics::SRGB, MatrixCoefficients::BT709, VideoFullRangeFlag::Full);

    if (bit_depth == 8 && cicp.transfer_characteristics() == output_cicp.transfer_characteristics() && cicp.color_primaries() == output_cicp.color_primaries() && cicp.video_full_range_flag() == VideoFullRangeFlag::Studio) {
        switch (cicp.matrix_coefficients()) {
        case MatrixCoefficients::BT470BG:
        case MatrixCoefficients::BT601:
            return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([](T y, T u, T v) { return ColorConverter::convert_simple_yuv_to_rgb<MatrixCoefficients::BT601, VideoFullRangeFlag::Studio>(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
        case MatrixCoefficients::BT709:
            return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([](T y, T u, T v) { return ColorConverter::convert_simple_yuv_to_rgb<MatrixCoefficients::BT709, VideoFullRangeFlag::Studio>(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
        default:
            break;
        }
    }

    auto converter = TRY(ColorConverter::create(bit_depth, cicp, output_cicp));
    return convert_to_bitmap_subsampled<subsampling_horizontal, subsampling_vertical>([&](T y, T u, T v) { return converter.convert_yuv(y, u, v); }, width, height, plane_y, plane_u, plane_v, bitmap);
}

template<u32 subsampling_horizontal, u32 subsampling_vertical>
static ALWAYS_INLINE DecoderErrorOr<void> convert_to_bitmap_selecting_bit_depth(CodingIndependentCodePoints cicp, u8 bit_depth, u32 const width, u32 const height, void* plane_y_data, void* plane_u_data, void* plane_v_data, Gfx::Bitmap& bitmap)
{
    if (bit_depth <= 8) {
        return convert_to_bitmap_selecting_converter<subsampling_horizontal, subsampling_vertical, u8>(cicp, bit_depth, width, height, plane_y_data, plane_u_data, plane_v_data, bitmap);
    }

    return convert_to_bitmap_selecting_converter<subsampling_horizontal, subsampling_vertical, u16>(cicp, bit_depth, width, height, plane_y_data, plane_u_data, plane_v_data, bitmap);
}

static DecoderErrorOr<void> convert_to_bitmap_selecting_subsampling(Subsampling subsampling, CodingIndependentCodePoints cicp, u8 bit_depth, u32 const width, u32 const height, void* plane_y, void* plane_u, void* plane_v, Gfx::Bitmap& bitmap)
{
    if (subsampling.x() && subsampling.y()) {
        return convert_to_bitmap_selecting_bit_depth<true, true>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
    }

    if (subsampling.x() && !subsampling.y()) {
        return convert_to_bitmap_selecting_bit_depth<true, false>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
    }

    if (!subsampling.x() && subsampling.y()) {
        return convert_to_bitmap_selecting_bit_depth<false, true>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
    }

    return convert_to_bitmap_selecting_bit_depth<false, false>(cicp, bit_depth, width, height, plane_y, plane_u, plane_v, bitmap);
}

DecoderErrorOr<void> SubsampledYUVFrame::output_to_bitmap(Gfx::Bitmap& bitmap)
{
    return convert_to_bitmap_selecting_subsampling(m_subsampling, cicp(), bit_depth(), width(), height(), m_y_buffer, m_u_buffer, m_v_buffer, bitmap);
}

}
