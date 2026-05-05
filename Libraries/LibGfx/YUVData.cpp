/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/YUVData.h>
#include <RustFFI.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
#include <cstddef>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

namespace Details {

struct YUVDataImpl {
    IntSize size;
    u8 bit_depth;
    Media::Subsampling subsampling;
    Media::CodingIndependentCodePoints cicp;

    FixedArray<u8> y_buffer;
    FixedArray<u8> u_buffer;
    FixedArray<u8> v_buffer;
};

}

ErrorOr<NonnullOwnPtr<YUVData>> YUVData::create(IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp)
{
    VERIFY(bit_depth <= 16);
    auto component_size = bit_depth <= 8 ? 1 : 2;

    auto y_buffer_size = static_cast<size_t>(size.width()) * size.height() * component_size;

    auto uv_size = subsampling.subsampled_size(size);
    auto uv_buffer_size = static_cast<size_t>(uv_size.width()) * uv_size.height() * component_size;

    auto y_buffer = TRY(FixedArray<u8>::create(y_buffer_size));
    auto u_buffer = TRY(FixedArray<u8>::create(uv_buffer_size));
    auto v_buffer = TRY(FixedArray<u8>::create(uv_buffer_size));

    auto impl = TRY(try_make<Details::YUVDataImpl>(Details::YUVDataImpl {
        .size = size,
        .bit_depth = bit_depth,
        .subsampling = subsampling,
        .cicp = cicp,
        .y_buffer = move(y_buffer),
        .u_buffer = move(u_buffer),
        .v_buffer = move(v_buffer),
    }));

    return adopt_nonnull_own_or_enomem(new (nothrow) YUVData(move(impl)));
}

YUVData::YUVData(NonnullOwnPtr<Details::YUVDataImpl> impl)
    : m_impl(move(impl))
{
}

YUVData::~YUVData() = default;

IntSize YUVData::size() const
{
    return m_impl->size;
}

u8 YUVData::bit_depth() const
{
    return m_impl->bit_depth;
}

Media::Subsampling YUVData::subsampling() const
{
    return m_impl->subsampling;
}

Media::CodingIndependentCodePoints const& YUVData::cicp() const
{
    return m_impl->cicp;
}

Bytes YUVData::y_data()
{
    return m_impl->y_buffer.span();
}

Bytes YUVData::u_data()
{
    return m_impl->u_buffer.span();
}

Bytes YUVData::v_data()
{
    return m_impl->v_buffer.span();
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
    auto const& impl = *m_impl;
    VERIFY(impl.bit_depth <= 12);

    auto bitmap = TRY(Bitmap::create(BitmapFormat::RGBA8888, AlphaType::Premultiplied, impl.size));
    auto* dst = reinterpret_cast<u8*>(bitmap->scanline(0));
    auto dst_stride = static_cast<u32>(bitmap->pitch());

    auto width = static_cast<u32>(impl.size.width());
    auto height = static_cast<u32>(impl.size.height());

    if (impl.cicp.matrix_coefficients() == Media::MatrixCoefficients::Identity) {
        if (impl.subsampling.x() || impl.subsampling.y())
            return Error::from_string_literal("Subsampled RGB is unsupported");

        if (impl.bit_depth <= 8) {
            auto const* y_data = impl.y_buffer.data();
            auto const* u_data = impl.u_buffer.data();
            auto const* v_data = impl.v_buffer.data();
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
            auto shift = impl.bit_depth - 8;
            auto const* y_data = reinterpret_cast<u16 const*>(impl.y_buffer.data());
            auto const* u_data = reinterpret_cast<u16 const*>(impl.u_buffer.data());
            auto const* v_data = reinterpret_cast<u16 const*>(impl.v_buffer.data());
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

    auto uv_size = impl.subsampling.subsampled_size(impl.size).to_type<u32>();

    bool full_range = impl.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;
    auto range = full_range ? FFI::YUVRange::Full : FFI::YUVRange::Limited;
    auto matrix = yuv_matrix_for_cicp(impl.cicp);

    auto y_stride = width;
    auto uv_stride = uv_size.width();

    bool success;
    if (impl.bit_depth <= 8) {
        success = FFI::yuv_u8_to_rgba(
            impl.y_buffer.data(), y_stride,
            impl.u_buffer.data(), uv_stride,
            impl.v_buffer.data(), uv_stride,
            width, height,
            impl.subsampling.x(), impl.subsampling.y(),
            dst, dst_stride,
            range, matrix);
    } else {
        success = FFI::yuv_u16_to_rgba(
            reinterpret_cast<u16 const*>(impl.y_buffer.data()), y_stride,
            reinterpret_cast<u16 const*>(impl.u_buffer.data()), uv_stride,
            reinterpret_cast<u16 const*>(impl.v_buffer.data()), uv_stride,
            width, height,
            impl.bit_depth,
            impl.subsampling.x(), impl.subsampling.y(),
            dst, dst_stride,
            range, matrix);
    }

    if (!success)
        return Error::from_string_literal("YUV-to-RGB conversion failed");

    return bitmap;
}

static SkYUVColorSpace skia_yuv_color_space(Media::CodingIndependentCodePoints cicp)
{
    bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;

    switch (cicp.matrix_coefficients()) {
    case Media::MatrixCoefficients::BT709:
        return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::FCC:
        return full_range ? kFCC_Full_SkYUVColorSpace : kFCC_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::BT470BG:
    case Media::MatrixCoefficients::BT601:
        return full_range ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::SMPTE240:
        return full_range ? kSMPTE240_Full_SkYUVColorSpace : kSMPTE240_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::YCgCo:
        return full_range ? kYCgCo_16bit_Full_SkYUVColorSpace : kYCgCo_16bit_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::BT2020NonConstantLuminance:
    case Media::MatrixCoefficients::BT2020ConstantLuminance:
        return full_range ? kBT2020_16bit_Full_SkYUVColorSpace : kBT2020_16bit_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::SMPTE2085:
        return full_range ? kYDZDX_Full_SkYUVColorSpace : kYDZDX_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::Identity:
        return kIdentity_SkYUVColorSpace;
    default:
        // Default to BT.709 for unsupported matrix coefficients
        return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    }
}

static SkYUVAInfo::Subsampling skia_subsampling(Media::Subsampling subsampling)
{
    if (!subsampling.x() && !subsampling.y())
        return SkYUVAInfo::Subsampling::k444;
    if (subsampling.x() && !subsampling.y())
        return SkYUVAInfo::Subsampling::k422;
    if (!subsampling.x() && subsampling.y())
        return SkYUVAInfo::Subsampling::k440;
    return SkYUVAInfo::Subsampling::k420;
}

static u16 expand_sample_to_full_16_bit_range(u16 sample, u8 bit_depth)
{
    if (bit_depth >= 16)
        return sample;

    auto const shift = 16 - bit_depth;
    auto const inverse_shift = bit_depth - shift;
    return static_cast<u16>((sample << shift) | (sample >> inverse_shift));
}

static void copy_plane_expanded_to_full_16_bit_range(FixedArray<u8> const& source_buffer, SkPixmap const& destination, IntSize plane_size, u8 bit_depth)
{
    VERIFY(bit_depth > 8);

    auto const* source = reinterpret_cast<u16 const*>(source_buffer.data());
    auto source_stride = static_cast<size_t>(plane_size.width());

    for (int row = 0; row < plane_size.height(); row++) {
        auto const* source_row = source + (static_cast<size_t>(row) * source_stride);
        auto* destination_row = destination.writable_addr16(0, row);
        for (int column = 0; column < plane_size.width(); column++)
            destination_row[column] = expand_sample_to_full_16_bit_range(source_row[column], bit_depth);
    }
}

SkYUVAPixmaps YUVData::make_pixmaps() const
{
    auto skia_size = SkISize::Make(m_impl->size.width(), m_impl->size.height());

    auto yuva_info = SkYUVAInfo(
        skia_size,
        SkYUVAInfo::PlaneConfig::kY_U_V,
        skia_subsampling(m_impl->subsampling),
        skia_yuv_color_space(m_impl->cicp));

    SkColorType color_type;
    SkYUVAPixmapInfo::DataType data_type;
    size_t component_size;
    if (m_impl->bit_depth <= 8) {
        color_type = kAlpha_8_SkColorType;
        data_type = SkYUVAPixmapInfo::DataType::kUnorm8;
        component_size = 1;
    } else {
        SkYUVAPixmapInfo pixmap_info(yuva_info, SkYUVAPixmapInfo::DataType::kUnorm16, nullptr);
        auto pixmaps = SkYUVAPixmaps::Allocate(pixmap_info);
        if (!pixmaps.isValid())
            return pixmaps;

        copy_plane_expanded_to_full_16_bit_range(m_impl->y_buffer, pixmaps.plane(0), m_impl->size, m_impl->bit_depth);

        auto uv_size = m_impl->subsampling.subsampled_size(m_impl->size);
        copy_plane_expanded_to_full_16_bit_range(m_impl->u_buffer, pixmaps.plane(1), uv_size, m_impl->bit_depth);
        copy_plane_expanded_to_full_16_bit_range(m_impl->v_buffer, pixmaps.plane(2), uv_size, m_impl->bit_depth);

        return pixmaps;
    }

    auto y_row_bytes = static_cast<size_t>(m_impl->size.width()) * component_size;

    auto uv_size = m_impl->subsampling.subsampled_size(m_impl->size);
    auto uv_row_bytes = static_cast<size_t>(uv_size.width()) * component_size;

    SkYUVAPixmapInfo pixmap_info(yuva_info, data_type, nullptr);

    // Create pixmaps from our buffers
    SkPixmap y_pixmap(
        SkImageInfo::Make(skia_size, color_type, kOpaque_SkAlphaType),
        m_impl->y_buffer.data(),
        y_row_bytes);
    SkPixmap u_pixmap(
        SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
        m_impl->u_buffer.data(),
        uv_row_bytes);
    SkPixmap v_pixmap(
        SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
        m_impl->v_buffer.data(),
        uv_row_bytes);

    SkPixmap plane_pixmaps[SkYUVAInfo::kMaxPlanes] = { y_pixmap, u_pixmap, v_pixmap, {} };

    return SkYUVAPixmaps::FromExternalPixmaps(yuva_info, plane_pixmaps);
}

}
