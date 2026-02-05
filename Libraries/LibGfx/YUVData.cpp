/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/YUVData.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
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

    // Lazily created when ImmutableBitmap needs it
    mutable Optional<SkYUVAPixmaps> pixmaps;

    SkYUVColorSpace skia_yuv_color_space() const
    {
        bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;

        switch (cicp.matrix_coefficients()) {
        case Media::MatrixCoefficients::BT470BG:
        case Media::MatrixCoefficients::BT601:
            return full_range ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::BT709:
            return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::BT2020NonConstantLuminance:
        case Media::MatrixCoefficients::BT2020ConstantLuminance:
            if (bit_depth <= 8)
                return kBT2020_8bit_Limited_SkYUVColorSpace;
            else if (bit_depth <= 10)
                return kBT2020_10bit_Limited_SkYUVColorSpace;
            else
                return kBT2020_12bit_Limited_SkYUVColorSpace;
        case Media::MatrixCoefficients::Identity:
            return kIdentity_SkYUVColorSpace;
        default:
            // Default to BT.709 for unsupported matrix coefficients
            return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
        }
    }

    SkYUVAInfo::Subsampling skia_subsampling() const
    {
        // Map Media::Subsampling to Skia's subsampling enum
        // x() = horizontal subsampling, y() = vertical subsampling
        if (!subsampling.x() && !subsampling.y())
            return SkYUVAInfo::Subsampling::k444; // 4:4:4 - no subsampling
        if (subsampling.x() && !subsampling.y())
            return SkYUVAInfo::Subsampling::k422; // 4:2:2 - horizontal only
        if (!subsampling.x() && subsampling.y())
            return SkYUVAInfo::Subsampling::k440; // 4:4:0 - vertical only
        return SkYUVAInfo::Subsampling::k420;     // 4:2:0 - both
    }

    SkYUVAPixmaps const& get_or_create_pixmaps() const
    {
        if (pixmaps.has_value())
            return pixmaps.value();

        auto skia_size = SkISize::Make(size.width(), size.height());

        // Use Y_U_V plane configuration (3 separate planes)
        auto yuva_info = SkYUVAInfo(
            skia_size,
            SkYUVAInfo::PlaneConfig::kY_U_V,
            skia_subsampling(),
            skia_yuv_color_space());

        // Determine color type based on bit depth
        SkColorType color_type;
        if (bit_depth <= 8) {
            color_type = kAlpha_8_SkColorType;
        } else {
            // 10/12/16-bit data stored in 16-bit values
            color_type = kA16_unorm_SkColorType;
        }

        // Calculate row bytes for each plane
        auto component_size = bit_depth <= 8 ? 1 : 2;
        auto y_row_bytes = static_cast<size_t>(size.width()) * component_size;

        auto uv_size = subsampling.subsampled_size(size);
        auto uv_row_bytes = static_cast<size_t>(uv_size.width()) * component_size;

        // Create pixmap info for each plane
        SkYUVAPixmapInfo::DataType data_type = bit_depth <= 8
            ? SkYUVAPixmapInfo::DataType::kUnorm8
            : SkYUVAPixmapInfo::DataType::kUnorm16;

        SkYUVAPixmapInfo pixmap_info(yuva_info, data_type, nullptr);

        // Create pixmaps from our buffers
        SkPixmap y_pixmap(
            SkImageInfo::Make(size.width(), size.height(), color_type, kOpaque_SkAlphaType),
            y_buffer.data(),
            y_row_bytes);
        SkPixmap u_pixmap(
            SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
            u_buffer.data(),
            uv_row_bytes);
        SkPixmap v_pixmap(
            SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
            v_buffer.data(),
            uv_row_bytes);

        SkPixmap plane_pixmaps[SkYUVAInfo::kMaxPlanes] = { y_pixmap, u_pixmap, v_pixmap, {} };

        pixmaps = SkYUVAPixmaps::FromExternalPixmaps(yuva_info, plane_pixmaps);
        return pixmaps.value();
    }
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
        .pixmaps = {},
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

SkYUVAPixmaps const& YUVData::skia_yuva_pixmaps() const
{
    return m_impl->get_or_create_pixmaps();
}

}
