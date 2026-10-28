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
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>
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

// Kept for a while after it was last drawn in case its surface comes back, but no longer, since the image keeps the
// surface's memory alive after the decoder has let go of it.
struct VideoSurfaceImage {
    AK_ALLOC_WITH_KMALLOC;

    NonnullRefPtr<Media::VideoSurface const> surface;
    NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    Media::CodingIndependentCodePoints cicp;
    sk_sp<SkImage> image;
    u64 shown_frame_id { 0 };
    MonotonicTime last_drawn_time { MonotonicTime() };
};

// Surface IDs are chosen by whoever sent the surface, so images are kept apart per sender.
struct VideoSurfaceImageKey {
    u64 sender_id { 0 };
    u64 surface_id { 0 };

    bool operator==(VideoSurfaceImageKey const&) const = default;
};

}

template<>
struct AK::Traits<Compositor::VideoSurfaceImageKey> : public DefaultTraits<Compositor::VideoSurfaceImageKey> {
    static unsigned hash(Compositor::VideoSurfaceImageKey const& key) { return pair_int_hash(u64_hash(key.sender_id), u64_hash(key.surface_id)); }
};

namespace Compositor {

static constexpr auto UNDRAWN_VIDEO_SURFACE_IMAGE_LIFETIME = AK::Duration::from_seconds(1);
static constexpr size_t MAX_VIDEO_SURFACE_IMAGES_PER_SENDER = 32;

static HashMap<VideoSurfaceImageKey, NonnullOwnPtr<VideoSurfaceImage>>& video_surface_images()
{
    // Leaked so that no image outlives the GPU context at exit.
    static auto& images = *new HashMap<VideoSurfaceImageKey, NonnullOwnPtr<VideoSurfaceImage>>;
    return images;
}

static void expire_undrawn_video_surface_images();

static Core::Timer& video_surface_image_expiry_timer()
{
    static auto& timer = Core::Timer::create_single_shot(AK::Duration::zero(), [] { expire_undrawn_video_surface_images(); }).leak_ref();
    return timer;
}

static void schedule_video_surface_image_expiry()
{
    auto& timer = video_surface_image_expiry_timer();
    if (timer.is_active())
        return;
    auto const& images = video_surface_images();
    if (images.is_empty())
        return;
    auto oldest_drawn_time = MonotonicTime::now();
    for (auto const& entry : images)
        oldest_drawn_time = min(oldest_drawn_time, entry.value->last_drawn_time);
    auto delay = max(AK::Duration::zero(), oldest_drawn_time + UNDRAWN_VIDEO_SURFACE_IMAGE_LIFETIME - MonotonicTime::now());
    timer.set_interval(delay);
    timer.start();
}

static void expire_undrawn_video_surface_images()
{
    auto expiry_time = MonotonicTime::now() - UNDRAWN_VIDEO_SURFACE_IMAGE_LIFETIME;
    video_surface_images().remove_all_matching([&](auto const&, auto const& image) {
        return image->last_drawn_time <= expiry_time;
    });
    schedule_video_surface_image_expiry();
}

static void make_room_for_video_surface_image(u64 sender_id)
{
    auto& images = video_surface_images();
    size_t sender_image_count = 0;
    Optional<VideoSurfaceImageKey> least_recently_drawn_key;
    MonotonicTime least_recently_drawn_time = MonotonicTime::now();
    for (auto const& [key, image] : images) {
        if (key.sender_id != sender_id)
            continue;
        sender_image_count++;
        if (!least_recently_drawn_key.has_value() || image->last_drawn_time < least_recently_drawn_time) {
            least_recently_drawn_key = key;
            least_recently_drawn_time = image->last_drawn_time;
        }
    }
    if (sender_image_count >= MAX_VIDEO_SURFACE_IMAGES_PER_SENDER)
        images.remove(least_recently_drawn_key.value());
}

static sk_sp<SkImage> image_for_video_surface(Media::VideoFrame const& frame, Media::VideoSurface const& surface, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto sender_id = frame.storage()->sender_id();
    VideoSurfaceImageKey key { sender_id, surface.id() };

    auto& images = video_surface_images();
    auto* cached_image = [&]() -> VideoSurfaceImage* {
        auto cached = images.get(key);
        if (!cached.has_value())
            return nullptr;
        auto& image = *cached.value();
        if (image.skia_backend_context.ptr() != &skia_backend_context || image.cicp != frame.cicp())
            return nullptr;
        return &image;
    }();

    if (!cached_image) {
        images.remove(key);
        auto image = sk_image_from_video_surface(surface, frame.cicp(), skia_backend_context);
        if (!image)
            return nullptr;
        make_room_for_video_surface_image(sender_id);
        auto new_image = make<VideoSurfaceImage>(surface, skia_backend_context, frame.cicp(), move(image));
        cached_image = new_image.ptr();
        images.set(key, move(new_image));
    }

    cached_image->last_drawn_time = MonotonicTime::now();
    schedule_video_surface_image_expiry();

    if (cached_image->shown_frame_id != frame.id()) {
        video_surface_image_will_show_new_frame(*cached_image->image);
        cached_image->shown_frame_id = frame.id();
    }
    return cached_image->image;
}

static Gfx::ColorSpace color_space_of(Media::VideoFrame const& frame)
{
    auto color_space_result = Gfx::ColorSpace::from_cicp(frame.cicp());
    if (color_space_result.is_error())
        return {};
    return color_space_result.release_value();
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

    auto color_space = color_space_of(frame);

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

// Draws the frame's surface on the GPU into a fresh surface and reads its pixels back from there, in sRGB.
static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> read_back_video_surface_through_gpu(Media::VideoFrame const& frame, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto image = sk_image_from_video_frame(frame, skia_backend_context);
    if (!image)
        return Error::from_string_literal("Could not make an image of the video frame's surface");
    auto size = frame.size().to_type<int>();
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
    auto rect = SkRect::MakeIWH(size.width(), size.height());
    surface->canvas().drawImageRect(image.get(), rect, rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor), nullptr, SkCanvas::kStrict_SrcRectConstraint);
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    surface->read_into_bitmap(*bitmap);
    return bitmap;
}

sk_sp<SkImage> sk_image_in_memory_from_video_frame(Media::VideoFrame const& frame, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto bitmap_or_error = frame.to_bitmap();
    if (!bitmap_or_error.is_error())
        return Gfx::sk_image_adopting_bitmap(bitmap_or_error.release_value(), color_space_of(frame));

    // NB: Only macOS reads a video surface on the CPU. Elsewhere the GPU reads it, into a surface of its own: a readback
    //     of the image of the video surface could be one Skia cached for an earlier frame that the surface showed.
    if (frame.surface() != nullptr && skia_backend_context) {
        bitmap_or_error = read_back_video_surface_through_gpu(frame, skia_backend_context);
        if (!bitmap_or_error.is_error())
            return Gfx::sk_image_adopting_bitmap(bitmap_or_error.release_value(), {});
    }

    dbgln("Could not convert video frame to bitmap: {}", bitmap_or_error.release_error());
    return nullptr;
}

ErrorOr<Gfx::DecodedImageFrame> decoded_image_frame_from_video_frame(Media::VideoFrame const& frame, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    if (frame.surface() != nullptr && skia_backend_context) {
        auto bitmap = TRY(read_back_video_surface_through_gpu(frame, skia_backend_context));
        // The pixels are read back in sRGB.
        return Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { move(bitmap) } };
    }

    auto bitmap = TRY(frame.to_bitmap());
    return Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { move(bitmap) }, color_space_of(frame) };
}

}
