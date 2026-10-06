/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/VideoFrameSkia.h>
#include <LibGfx/YUVData.h>

#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>

namespace Compositor {

SkYUVColorSpace skia_yuv_color_space(Media::CodingIndependentCodePoints cicp)
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

static void copy_plane_expanded_to_full_16_bit_range(ReadonlyBytes source_buffer, SkPixmap const& destination, Gfx::IntSize plane_size, u8 bit_depth)
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

SkYUVAPixmaps make_yuva_pixmaps(Gfx::YUVData const& yuv_data)
{
    auto size = yuv_data.size();
    auto skia_size = SkISize::Make(size.width(), size.height());

    auto yuva_info = SkYUVAInfo(
        skia_size,
        SkYUVAInfo::PlaneConfig::kY_U_V,
        skia_subsampling(yuv_data.subsampling()),
        skia_yuv_color_space(yuv_data.cicp()));

    SkColorType color_type;
    SkYUVAPixmapInfo::DataType data_type;
    size_t component_size;
    if (yuv_data.bit_depth() <= 8) {
        color_type = kAlpha_8_SkColorType;
        data_type = SkYUVAPixmapInfo::DataType::kUnorm8;
        component_size = 1;
    } else {
        SkYUVAPixmapInfo pixmap_info(yuva_info, SkYUVAPixmapInfo::DataType::kUnorm16, nullptr);
        auto pixmaps = SkYUVAPixmaps::Allocate(pixmap_info);
        if (!pixmaps.isValid())
            return pixmaps;

        copy_plane_expanded_to_full_16_bit_range(yuv_data.y_data(), pixmaps.plane(0), size, yuv_data.bit_depth());

        auto uv_size = yuv_data.subsampling().subsampled_size(size);
        copy_plane_expanded_to_full_16_bit_range(yuv_data.u_data(), pixmaps.plane(1), uv_size, yuv_data.bit_depth());
        copy_plane_expanded_to_full_16_bit_range(yuv_data.v_data(), pixmaps.plane(2), uv_size, yuv_data.bit_depth());

        return pixmaps;
    }

    auto y_row_bytes = static_cast<size_t>(size.width()) * component_size;

    auto uv_size = yuv_data.subsampling().subsampled_size(size);
    auto uv_row_bytes = static_cast<size_t>(uv_size.width()) * component_size;

    SkYUVAPixmapInfo pixmap_info(yuva_info, data_type, nullptr);

    // Create pixmaps from our buffers
    SkPixmap y_pixmap(
        SkImageInfo::Make(skia_size, color_type, kOpaque_SkAlphaType),
        yuv_data.y_data().data(),
        y_row_bytes);
    SkPixmap u_pixmap(
        SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
        yuv_data.u_data().data(),
        uv_row_bytes);
    SkPixmap v_pixmap(
        SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
        yuv_data.v_data().data(),
        uv_row_bytes);

    SkPixmap plane_pixmaps[SkYUVAInfo::kMaxPlanes] = { y_pixmap, u_pixmap, v_pixmap, {} };

    return SkYUVAPixmaps::FromExternalPixmaps(yuva_info, plane_pixmaps);
}

}
