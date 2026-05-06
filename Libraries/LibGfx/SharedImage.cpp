/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
namespace Gfx {

static constexpr auto shared_image_wire_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_wire_alpha_type = BitmapAlpha::Premultiplied;
static constexpr auto shared_image_wire_color_space = BitmapColorSpace::Linear;
static constexpr auto shared_image_wire_origin = BitmapOrigin::TopLeft;

SharedImagePayload SharedImage::make_shareable_bitmap_payload(BitmapInfo const& info, NonnullRefPtr<Bitmap> const& bitmap, ColorSpace const& color_space)
{
    auto payload_info = info;
    payload_info.row_bytes = static_cast<u32>(bitmap->pitch());
    return SharedImagePayload {
        payload_info,
        ShareableBitmap { bitmap, ShareableBitmap::ConstructWithKnownGoodBitmap },
        color_space
    };
}

SharedImage::SharedImage(BitmapInfo const& info, NonnullRefPtr<Bitmap> bitmap)
    : m_info(info)
    , m_bitmap(move(bitmap))
{
}

bool SharedImage::is_shareable_bitmap_backed() const
{
    return m_backing_kind == BackingKind::ShareableBitmap;
}

SharedImage SharedImage::create_bitmap_backed(BitmapInfo const& info, BitmapAlpha alpha_type)
{
    auto bitmap = MUST(Bitmap::create_shareable(info.pixel_format, alpha_type, info.size));
    auto bitmap_info = info;
    bitmap_info.row_bytes = static_cast<u32>(bitmap->pitch());
    return SharedImage(bitmap_info, move(bitmap));
}

#if !defined(AK_OS_MACOS) && !defined(USE_VULKAN_DMABUF_IMAGES)
SharedImage::SharedImage(SharedImage&&) = default;
SharedImage& SharedImage::operator=(SharedImage&&) = default;
SharedImage::~SharedImage() = default;

SharedImage SharedImage::create(BitmapInfo const& info)
{
    return create_bitmap_backed(info, info.alpha_type);
}

SharedImage SharedImage::import_from_payload(SharedImagePayload payload)
{
    if (auto* shareable_bitmap = payload.shareable_bitmap()) {
        auto bitmap_info = payload.info();
        bitmap_info.row_bytes = static_cast<u32>(shareable_bitmap->bitmap()->pitch());
        auto shared_image = SharedImage(bitmap_info, NonnullRefPtr<Bitmap> { *shareable_bitmap->bitmap() });
        shared_image.set_color_space(payload.color_space());
        return shared_image;
    }
    VERIFY_NOT_REACHED();
}

SharedImagePayload SharedImage::export_payload() const
{
    if (m_backing_kind == BackingKind::ShareableBitmap)
        return make_shareable_bitmap_payload(m_info, m_bitmap, m_color_space);
    VERIFY_NOT_REACHED();
}
#endif

SharedImage SharedImage::create(IntSize size)
{
    return create({
        .size = size,
        .pixel_format = shared_image_wire_format,
        .color_space = shared_image_wire_color_space,
        .alpha_type = shared_image_wire_alpha_type,
        .origin = shared_image_wire_origin,
    });
}

NonnullRefPtr<PaintingSurface> SharedImage::create_painting_surface(NonnullRefPtr<SkiaBackendContext> context, PaintingSurface::Origin origin)
{
    return PaintingSurface::create_from_shared_image(*this, move(context), origin);
}

}
