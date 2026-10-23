/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>
#include <Compositor/VideoFrameSkia.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameHandle.h>
#include <LibMediaCodecs/VideoSurface.h>

#include <core/SkCanvas.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Compositor {

#if !defined(AK_OS_MACOS) && !defined(USE_VULKAN_DMABUF_IMAGES)
sk_sp<SkImage> sk_image_from_video_surface(Media::VideoSurface const&, Media::CodingIndependentCodePoints, Gfx::SkiaBackendContext&)
{
    return nullptr;
}
#endif

#ifndef USE_VULKAN_DMABUF_IMAGES
void video_surface_image_will_show_new_frame(SkImage&)
{
}
#endif

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

// A surface-backed image samples the surface where it lies, so it stays valid for every frame decoded into that
// surface. A decoder recycles a bounded set of surfaces, so each one's image is kept for as long as the surface is.
struct VideoSurfaceImage {
    AK_ALLOC_WITH_KMALLOC;

    NonnullRefPtr<Media::VideoSurface const> surface;
    NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    Media::CodingIndependentCodePoints cicp;
    sk_sp<SkImage> image;
    Media::VideoFramePoolID shown_pool_id { 0 };
    u32 shown_slot_index { 0 };
    u64 shown_slot_acquisition_id { 0 };
};

static HashMap<u32, NonnullOwnPtr<VideoSurfaceImage>>& video_surface_images()
{
    // Leaked so that no image outlives the GPU context at exit.
    static auto& images = *new HashMap<u32, NonnullOwnPtr<VideoSurfaceImage>>;
    return images;
}

static sk_sp<SkImage> image_for_video_surface(Media::VideoFrame const& frame, Media::VideoSurface const& surface, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto& images = video_surface_images();
    auto* cached_image = [&]() -> VideoSurfaceImage* {
        auto cached = images.get(surface.id());
        if (!cached.has_value())
            return nullptr;
        auto& image = **cached;
        if (image.surface.ptr() != &surface || image.skia_backend_context.ptr() != &skia_backend_context || image.cicp != frame.cicp())
            return nullptr;
        return &image;
    }();

    if (!cached_image) {
        // A surface that only this cache still holds will not be handed out again.
        images.remove_all_matching([](auto, auto const& image) { return image->surface->ref_count() == 1; });

        auto image = sk_image_from_video_surface(surface, frame.cicp(), skia_backend_context);
        if (!image)
            return nullptr;
        auto new_image = make<VideoSurfaceImage>(surface, skia_backend_context, frame.cicp(), move(image));
        cached_image = new_image.ptr();
        images.set(surface.id(), move(new_image));
    }

    auto handle = Media::VideoFrameHandle::for_frame(frame);
    if (cached_image->shown_pool_id != handle.pool_id || cached_image->shown_slot_index != handle.slot_index || cached_image->shown_slot_acquisition_id != handle.slot_acquisition_id) {
        video_surface_image_will_show_new_frame(*cached_image->image);
        cached_image->shown_pool_id = handle.pool_id;
        cached_image->shown_slot_index = handle.slot_index;
        cached_image->shown_slot_acquisition_id = handle.slot_acquisition_id;
    }
    return cached_image->image;
}

sk_sp<SkImage> sk_image_from_video_frame(Media::VideoFrame const& frame, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    sk_sp<SkImage> image;
    auto* gr_context = skia_backend_context ? skia_backend_context->sk_context() : nullptr;
    auto surface = frame.surface();

    if (surface != nullptr && skia_backend_context)
        image = image_for_video_surface(frame, *surface, *skia_backend_context);

    auto yuv_data = image ? Optional<Gfx::YUVData> {} : frame.yuv_data();
    if (!image && !yuv_data.has_value() && surface == nullptr)
        return nullptr;

    auto color_space = Gfx::ColorSpace {};
    if (auto color_space_result = Gfx::ColorSpace::from_cicp(frame.cicp()); !color_space_result.is_error())
        color_space = color_space_result.release_value();

    if (!image && gr_context && yuv_data.has_value()) {
        image = SkImages::TextureFromYUVAPixmaps(
            gr_context,
            make_yuva_pixmaps(*yuv_data),
            skgpu::Mipmapped::kNo,
            false,
            Gfx::to_skia_color_space(color_space));
    }

    if (!image) {
        // Without a GPU to sample a surface on, its pixels have to be read back into memory to be drawn.
        auto bitmap_or_error = frame.to_bitmap();
        if (bitmap_or_error.is_error()) {
            dbgln("Could not convert video frame to bitmap: {}", bitmap_or_error.release_error());
            return nullptr;
        }
        auto raster_image = Gfx::sk_image_adopting_bitmap(bitmap_or_error.release_value(), color_space);
        if (gr_context)
            image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
        if (!image)
            image = move(raster_image);
    }

    return image;
}

ErrorOr<Gfx::DecodedImageFrame> decoded_image_frame_from_video_frame(Media::VideoFrame const& frame, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    if (frame.surface() != nullptr && skia_backend_context) {
        auto image = sk_image_from_video_frame(frame, skia_backend_context);
        if (!image)
            return Error::from_string_literal("Could not make an image of the video frame's surface");
        auto size = frame.size().to_type<int>();
        auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
        auto rect = SkRect::MakeIWH(size.width(), size.height());
        surface->canvas().drawImageRect(image.get(), rect, rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor), nullptr, SkCanvas::kStrict_SrcRectConstraint);
        auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
        // The pixels are read back in sRGB.
        surface->read_into_bitmap(*bitmap);
        return Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { move(bitmap) } };
    }

    auto bitmap = TRY(frame.to_bitmap());
    auto color_space = Gfx::ColorSpace {};
    if (auto color_space_result = Gfx::ColorSpace::from_cicp(frame.cicp()); !color_space_result.is_error())
        color_space = color_space_result.release_value();
    return Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { move(bitmap) }, move(color_space) };
}

}
