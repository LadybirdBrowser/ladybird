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
    if (bit_depth == 0 || bit_depth > 12)
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

// Where one of the components of an RGB picture lies, in a plane of its own or interleaved with another.
struct RGBComponent {
    u8 const* first_sample;
    size_t row_stride;
    size_t samples_per_pixel;
};

// Each stored sample holds padding_bit_count unused bits below its value.
template<typename Sample>
static void rgb_components_to_bitmap(Bitmap& bitmap, u8 bit_depth, u8 padding_bit_count, Media::VideoFullRangeFlag range, RGBComponent green, RGBComponent blue, RGBComponent red)
{
    auto bits_beyond_eight = max(bit_depth, 8) - 8;
    // ITU-T H.273, 8.3: studio range RGB spans 16 to 235 at 8 bits, scaled up for deeper samples.
    auto is_studio_range = range == Media::VideoFullRangeFlag::Studio;
    u32 black = 16u << bits_beyond_eight;
    u32 span = 219u << bits_beyond_eight;

    auto size = bitmap.size();
    for (int row = 0; row < size.height(); row++) {
        auto* destination = bitmap.scanline_u8(row);
        auto sample_at = [&](RGBComponent const& component, int column) -> u8 {
            auto const* samples = reinterpret_cast<Sample const*>(component.first_sample + (static_cast<size_t>(row) * component.row_stride));
            u32 sample = samples[static_cast<size_t>(column) * component.samples_per_pixel] >> padding_bit_count;
            if (!is_studio_range)
                return static_cast<u8>(sample >> bits_beyond_eight);
            sample = clamp(sample, black, black + span) - black;
            return static_cast<u8>(((sample * 255) + (span / 2)) / span);
        };
        for (int column = 0; column < size.width(); column++) {
            destination[(column * 4) + 0] = sample_at(red, column);
            destination[(column * 4) + 1] = sample_at(green, column);
            destination[(column * 4) + 2] = sample_at(blue, column);
            destination[(column * 4) + 3] = 255;
        }
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

        // The planes hold green, blue and red, in that order.
        if (m_bit_depth <= 8) {
            RGBComponent green { m_y_data.data(), width, 1 };
            RGBComponent blue { m_u_data.data(), width, 1 };
            RGBComponent red { m_v_data.data(), width, 1 };
            rgb_components_to_bitmap<u8>(*bitmap, m_bit_depth, 0, m_cicp.video_full_range_flag(), green, blue, red);
        } else {
            auto row_stride = width * sizeof(u16);
            RGBComponent green { m_y_data.data(), row_stride, 1 };
            RGBComponent blue { m_u_data.data(), row_stride, 1 };
            RGBComponent red { m_v_data.data(), row_stride, 1 };
            rgb_components_to_bitmap<u16>(*bitmap, m_bit_depth, 0, m_cicp.video_full_range_flag(), green, blue, red);
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

ErrorOr<NonnullRefPtr<Bitmap>> biplanar_yuv_to_bitmap(IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp, BiplanarYUVPlane luma, BiplanarYUVPlane chroma)
{
    auto is_rgb = cicp.matrix_coefficients() == Media::MatrixCoefficients::Identity;
    if (is_rgb && subsampling != Media::Subsampling::yuv444())
        return Error::from_string_literal("Subsampled RGB is unsupported");
    if (size.is_empty())
        return Error::from_string_literal("Bi-planar YUV size is empty");

    size_t bytes_per_sample = bit_depth > 8 ? 2 : 1;
    auto plane_covers_rows = [](BiplanarYUVPlane const& plane, size_t row_byte_count, size_t row_count) {
        if (plane.stride < row_byte_count)
            return false;
        Checked<size_t> required_size = plane.stride;
        required_size *= row_count;
        return !required_size.has_overflow() && plane.data.size() >= required_size.value();
    };
    auto chroma_size = subsampling.subsampled_size(size.to_type<size_t>());
    auto chroma_width = chroma_size.width();
    auto chroma_height = chroma_size.height();
    if (!plane_covers_rows(luma, size.width() * bytes_per_sample, size.height()) || !plane_covers_rows(chroma, chroma_width * 2 * bytes_per_sample, chroma_height))
        return Error::from_string_literal("Bi-planar YUV planes are smaller than their size requires");

    auto bitmap = TRY(Bitmap::create(BitmapFormat::RGBA8888, AlphaType::Premultiplied, size));
    auto* destination = reinterpret_cast<u8*>(bitmap->scanline(0));
    auto destination_stride = static_cast<u32>(bitmap->pitch());

    auto width = static_cast<u32>(size.width());
    auto height = static_cast<u32>(size.height());

    if (is_rgb) {
        // The luma plane holds green, and the chroma plane holds blue and red in that order.
        RGBComponent green { luma.data.data(), luma.stride, 1 };
        RGBComponent blue { chroma.data.data(), chroma.stride, 2 };
        RGBComponent red { chroma.data.data() + bytes_per_sample, chroma.stride, 2 };
        if (bytes_per_sample == 1)
            rgb_components_to_bitmap<u8>(*bitmap, bit_depth, 0, cicp.video_full_range_flag(), green, blue, red);
        else
            rgb_components_to_bitmap<u16>(*bitmap, bit_depth, 16 - bit_depth, cicp.video_full_range_flag(), green, blue, red);
        return bitmap;
    }

    bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;
    auto range = full_range ? FFI::YUVRange::Full : FFI::YUVRange::Limited;
    auto matrix = yuv_matrix_for_cicp(cicp);

    bool success;
    if (bit_depth <= 8) {
        success = FFI::yuv_biplanar_u8_to_rgba(
            luma.data.data(), static_cast<u32>(luma.stride),
            chroma.data.data(), static_cast<u32>(chroma.stride),
            width, height,
            subsampling.x(), subsampling.y(),
            destination, destination_stride,
            range, matrix);
    } else {
        success = FFI::yuv_biplanar_u16_to_rgba(
            reinterpret_cast<u16 const*>(luma.data.data()), static_cast<u32>(luma.stride / sizeof(u16)),
            reinterpret_cast<u16 const*>(chroma.data.data()), static_cast<u32>(chroma.stride / sizeof(u16)),
            width, height,
            subsampling.x(), subsampling.y(),
            destination, destination_stride,
            range, matrix);
    }

    if (!success)
        return Error::from_string_literal("YUV-to-RGB conversion failed");

    return bitmap;
}

}
