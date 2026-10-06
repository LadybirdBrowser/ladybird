/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/YUVData.h>
#include <RustFFI.h>

#include <cstddef>

namespace Gfx {

static ErrorOr<size_t> checked_plane_size(Gfx::IntSize size, size_t component_size)
{
    Checked<size_t> plane_size = static_cast<size_t>(size.width());
    plane_size *= static_cast<size_t>(size.height());
    plane_size *= component_size;
    if (plane_size.has_overflow())
        return Error::from_string_literal("YUVData plane size overflow");
    return plane_size.value();
}

ErrorOr<YUVData::PlaneSizes> YUVData::plane_sizes(IntSize size, u8 bit_depth, Media::Subsampling subsampling)
{
    if (size.is_empty())
        return Error::from_string_literal("YUVData size is empty");
    if (bit_depth == 0 || bit_depth > 16)
        return Error::from_string_literal("Invalid YUVData bit depth");

    auto component_size = bit_depth <= 8 ? 1 : 2;
    auto y_buffer_size = TRY(checked_plane_size(size, component_size));
    auto uv_size = subsampling.subsampled_size(size);
    auto uv_buffer_size = TRY(checked_plane_size(uv_size, component_size));

    Checked<size_t> total_size = y_buffer_size;
    total_size += uv_buffer_size;
    total_size += uv_buffer_size;
    if (total_size.has_overflow())
        return Error::from_string_literal("YUVData total size overflow");

    return PlaneSizes {
        .y = y_buffer_size,
        .u = uv_buffer_size,
        .v = uv_buffer_size,
        .total = total_size.value(),
    };
}

ErrorOr<YUVData> YUVData::create(IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp, Bytes y_data, Bytes u_data, Bytes v_data)
{
    auto sizes = TRY(plane_sizes(size, bit_depth, subsampling));
    if (y_data.size() != sizes.y || u_data.size() != sizes.u || v_data.size() != sizes.v)
        return Error::from_string_literal("YUVData plane data size mismatch");

    return YUVData { size, bit_depth, subsampling, cicp, y_data, u_data, v_data };
}

YUVData::YUVData(IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp, Bytes y_data, Bytes u_data, Bytes v_data)
    : m_size(size)
    , m_bit_depth(bit_depth)
    , m_subsampling(subsampling)
    , m_cicp(cicp)
    , m_y_data(y_data)
    , m_u_data(u_data)
    , m_v_data(v_data)
{
}

static FFI::YUVMatrix yuv_matrix_for_cicp(Media::CodingIndependentCodePoints const& cicp)
{
    switch (cicp.matrix_coefficients()) {
    case Media::MatrixCoefficients::Identity:
        VERIFY_NOT_REACHED();
    case Media::MatrixCoefficients::FCC:
        return FFI::YUVMatrix::Fcc;
    case Media::MatrixCoefficients::BT470BG:
        return FFI::YUVMatrix::Bt470BG;
    case Media::MatrixCoefficients::BT601:
        return FFI::YUVMatrix::Bt601;
    case Media::MatrixCoefficients::SMPTE240:
        return FFI::YUVMatrix::Smpte240;
    case Media::MatrixCoefficients::BT2020NonConstantLuminance:
    case Media::MatrixCoefficients::BT2020ConstantLuminance:
        return FFI::YUVMatrix::Bt2020;
    case Media::MatrixCoefficients::BT709:
    case Media::MatrixCoefficients::Unspecified:
    default:
        return FFI::YUVMatrix::Bt709;
    }
}

ErrorOr<NonnullRefPtr<Bitmap>> YUVData::to_bitmap() const
{
    VERIFY(m_bit_depth <= 12);

    auto bitmap = TRY(Bitmap::create(BitmapFormat::RGBA8888, AlphaType::Premultiplied, m_size));
    auto* dst = reinterpret_cast<u8*>(bitmap->scanline(0));
    auto dst_stride = static_cast<u32>(bitmap->pitch());

    auto width = static_cast<u32>(m_size.width());
    auto height = static_cast<u32>(m_size.height());

    if (m_cicp.matrix_coefficients() == Media::MatrixCoefficients::Identity) {
        if (m_subsampling.x() || m_subsampling.y())
            return Error::from_string_literal("Subsampled RGB is unsupported");

        if (m_bit_depth <= 8) {
            auto const* y_data = m_y_data.data();
            auto const* u_data = m_u_data.data();
            auto const* v_data = m_v_data.data();
            auto y_stride = static_cast<int>(width);

            for (u32 row = 0; row < height; row++) {
                auto* dst_row = dst + (static_cast<size_t>(row) * dst_stride);
                auto const* y_row = y_data + (static_cast<size_t>(row) * y_stride);
                auto const* u_row = u_data + (static_cast<size_t>(row) * y_stride);
                auto const* v_row = v_data + (static_cast<size_t>(row) * y_stride);
                for (u32 col = 0; col < width; col++) {
                    dst_row[(col * 4) + 0] = v_row[col];
                    dst_row[(col * 4) + 1] = y_row[col];
                    dst_row[(col * 4) + 2] = u_row[col];
                    dst_row[(col * 4) + 3] = 255;
                }
            }
        } else {
            // Our buffers hold native N-bit values in the low bits of each u16; shift right to reduce
            // to 8-bit for the output.
            auto shift = m_bit_depth - 8;
            auto const* y_data = reinterpret_cast<u16 const*>(m_y_data.data());
            auto const* u_data = reinterpret_cast<u16 const*>(m_u_data.data());
            auto const* v_data = reinterpret_cast<u16 const*>(m_v_data.data());
            auto y_stride = static_cast<int>(width);

            for (u32 row = 0; row < height; row++) {
                auto* dst_row = dst + (static_cast<size_t>(row) * dst_stride);
                auto const* y_row = y_data + (static_cast<size_t>(row) * y_stride);
                auto const* u_row = u_data + (static_cast<size_t>(row) * y_stride);
                auto const* v_row = v_data + (static_cast<size_t>(row) * y_stride);
                for (u32 col = 0; col < width; col++) {
                    dst_row[(col * 4) + 0] = static_cast<u8>(v_row[col] >> shift);
                    dst_row[(col * 4) + 1] = static_cast<u8>(y_row[col] >> shift);
                    dst_row[(col * 4) + 2] = static_cast<u8>(u_row[col] >> shift);
                    dst_row[(col * 4) + 3] = 255;
                }
            }
        }

        return bitmap;
    }

    auto uv_size = m_subsampling.subsampled_size(m_size).to_type<u32>();

    bool full_range = m_cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;
    auto range = full_range ? FFI::YUVRange::Full : FFI::YUVRange::Limited;
    auto matrix = yuv_matrix_for_cicp(m_cicp);

    auto y_stride = width;
    auto uv_stride = uv_size.width();

    bool success;
    if (m_bit_depth <= 8) {
        success = FFI::yuv_u8_to_rgba(
            m_y_data.data(), y_stride,
            m_u_data.data(), uv_stride,
            m_v_data.data(), uv_stride,
            width, height,
            m_subsampling.x(), m_subsampling.y(),
            dst, dst_stride,
            range, matrix);
    } else {
        success = FFI::yuv_u16_to_rgba(
            reinterpret_cast<u16 const*>(m_y_data.data()), y_stride,
            reinterpret_cast<u16 const*>(m_u_data.data()), uv_stride,
            reinterpret_cast<u16 const*>(m_v_data.data()), uv_stride,
            width, height,
            m_bit_depth,
            m_subsampling.x(), m_subsampling.y(),
            dst, dst_stride,
            range, matrix);
    }

    if (!success)
        return Error::from_string_literal("YUV-to-RGB conversion failed");

    return bitmap;
}

ErrorOr<NonnullRefPtr<Bitmap>> biplanar_yuv_to_bitmap(IntSize size, u8 bit_depth, Media::CodingIndependentCodePoints cicp, BiplanarYUVPlane luma, BiplanarYUVPlane chroma)
{
    if (cicp.matrix_coefficients() == Media::MatrixCoefficients::Identity)
        return Error::from_string_literal("Subsampled RGB is unsupported");

    auto bitmap = TRY(Bitmap::create(BitmapFormat::RGBA8888, AlphaType::Premultiplied, size));
    auto* destination = reinterpret_cast<u8*>(bitmap->scanline(0));
    auto destination_stride = static_cast<u32>(bitmap->pitch());

    auto width = static_cast<u32>(size.width());
    auto height = static_cast<u32>(size.height());

    bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;
    auto range = full_range ? FFI::YUVRange::Full : FFI::YUVRange::Limited;
    auto matrix = yuv_matrix_for_cicp(cicp);

    bool success;
    if (bit_depth <= 8) {
        success = FFI::yuv_nv12_to_rgba(
            luma.data.data(), static_cast<u32>(luma.stride),
            chroma.data.data(), static_cast<u32>(chroma.stride),
            width, height,
            destination, destination_stride,
            range, matrix);
    } else {
        success = FFI::yuv_p010_to_rgba(
            reinterpret_cast<u16 const*>(luma.data.data()), static_cast<u32>(luma.stride / sizeof(u16)),
            reinterpret_cast<u16 const*>(chroma.data.data()), static_cast<u32>(chroma.stride / sizeof(u16)),
            width, height,
            destination, destination_stride,
            range, matrix);
    }

    if (!success)
        return Error::from_string_literal("YUV-to-RGB conversion failed");

    return bitmap;
}

}
