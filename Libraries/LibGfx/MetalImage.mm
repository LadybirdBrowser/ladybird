/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibCore/MachPort.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>

#import <IOSurface/IOSurface.h>
#include <core/SkCanvas.h>

namespace Gfx {

static u32 iosurface_pixel_format_from_bitmap_format(BitmapFormat bitmap_format);
static IOSurfaceRef lookup_iosurface(Core::MachPort const& port);
static IOSurfaceRef create_iosurface(BitmapInfo const& info);
static Core::MachPort create_mach_port_from_iosurface(IOSurfaceRef iosurface);
static Core::MachPort copy_send_right(Core::MachPort const& port);
static NonnullRefPtr<Bitmap> create_bitmap_from_iosurface(IOSurfaceRef iosurface, BitmapInfo const& info);
static ErrorOr<NonnullRefPtr<SkiaBackendContext>> shared_image_skia_backend_context();
static ErrorOr<void> upload_decoded_image_frame_to_surface(DecodedImageFrame const& frame, PaintingSurface& painting_surface);

ErrorOr<void> upload_decoded_image_frame_to_shared_image(DecodedImageFrame const& frame, SharedImagePayload& payload)
{
    if (auto* shareable_bitmap = payload.shareable_bitmap()) {
        auto* destination_bitmap = shareable_bitmap->bitmap();
        if (!destination_bitmap)
            return Error::from_string_literal("MetalImage: shareable_bitmap_missing_destination_bitmap");

        if (destination_bitmap->size() != frame.size())
            return Error::from_string_literal("MetalImage: shareable_bitmap_size_mismatch");

        return upload_decoded_image_frame_to_bitmap(frame, *destination_bitmap, payload.info(), payload.color_space());
    }

    auto* mach_port = payload.mach_port();
    if (!mach_port)
        return Error::from_string_literal("MetalImage: missing_mach_port_payload");

    auto context = shared_image_skia_backend_context();
    if (context.is_error())
        return context.release_error();

    NonnullRefPtr<SkiaBackendContext> nonnull_context = context.release_value();
    auto shared_image = SharedImage::import_from_payload(SharedImagePayload(payload.info(), copy_send_right(*mach_port), payload.color_space()));
    auto painting_surface = shared_image.create_painting_surface(nonnull_context, PaintingSurface::Origin::TopLeft);
    TRY(upload_decoded_image_frame_to_surface(frame, painting_surface));
    return {};
}

SharedImage::SharedImage(Core::MachPort&& mach_port, BitmapInfo const& info, NonnullRefPtr<Bitmap> bitmap, void* platform_surface_handle)
    : m_backing_kind(BackingKind::IOSurface)
    , m_info(info)
    , m_bitmap(move(bitmap))
    , m_mach_port(move(mach_port))
    , m_platform_surface_handle(platform_surface_handle)
{
}

SharedImage SharedImage::create(BitmapInfo const& info)
{
    auto* iosurface = create_iosurface(info);
    auto bitmap = create_bitmap_from_iosurface(iosurface, info);
    auto image_info = info;
    image_info.row_bytes = static_cast<u32>(IOSurfaceGetBytesPerRow(iosurface));
    return SharedImage(create_mach_port_from_iosurface(iosurface), image_info, move(bitmap), iosurface);
}

SharedImage SharedImage::import_from_payload(SharedImagePayload payload)
{
    if (auto* shareable_bitmap = payload.shareable_bitmap()) {
        auto image_info = payload.info();
        image_info.row_bytes = static_cast<u32>(shareable_bitmap->bitmap()->pitch());
        auto shared_image = SharedImage(image_info, NonnullRefPtr<Bitmap> { *shareable_bitmap->bitmap() });
        shared_image.set_color_space(payload.color_space());
        return shared_image;
    }

    auto* mach_port = payload.mach_port();
    VERIFY(mach_port);

    auto* iosurface = lookup_iosurface(*mach_port);
    auto bitmap = create_bitmap_from_iosurface(iosurface, payload.info());
    auto image_info = payload.info();
    image_info.row_bytes = static_cast<u32>(IOSurfaceGetBytesPerRow(iosurface));
    auto shared_image = SharedImage(move(*mach_port), image_info, move(bitmap), iosurface);
    shared_image.set_color_space(payload.color_space());
    return shared_image;
}

SharedImagePayload SharedImage::export_payload() const
{
    if (m_backing_kind == BackingKind::ShareableBitmap)
        return make_shareable_bitmap_payload(m_info, m_bitmap, m_color_space);

    return SharedImagePayload { m_info, copy_send_right(m_mach_port), m_color_space };
}

SharedImage::SharedImage(SharedImage&& other)
    : m_backing_kind(other.m_backing_kind)
    , m_info(other.m_info)
    , m_color_space(move(other.m_color_space))
    , m_bitmap(move(other.m_bitmap))
    , m_mach_port(move(other.m_mach_port))
    , m_platform_surface_handle(exchange(other.m_platform_surface_handle, nullptr))
{
}

SharedImage& SharedImage::operator=(SharedImage&& other)
{
    if (this == &other)
        return *this;

    if (m_platform_surface_handle)
        CFRelease((IOSurfaceRef)m_platform_surface_handle);

    m_backing_kind = other.m_backing_kind;
    m_info = other.m_info;
    m_color_space = move(other.m_color_space);
    m_bitmap = move(other.m_bitmap);
    m_mach_port = move(other.m_mach_port);
    m_platform_surface_handle = exchange(other.m_platform_surface_handle, nullptr);
    return *this;
}

SharedImage::~SharedImage()
{
    if (m_platform_surface_handle)
        CFRelease((IOSurfaceRef)m_platform_surface_handle);
}

static u32 iosurface_pixel_format_from_bitmap_format(BitmapFormat bitmap_format)
{
    switch (bitmap_format) {
    case BitmapFormat::BGRA8888:
    case BitmapFormat::BGRx8888:
        return 'BGRA';
    case BitmapFormat::RGBA8888:
    case BitmapFormat::RGBx8888:
        return 'RGBA';
    default:
        VERIFY_NOT_REACHED();
    }
}

static IOSurfaceRef lookup_iosurface(Core::MachPort const& port)
{
    auto* iosurface = IOSurfaceLookupFromMachPort(port.port());
    VERIFY(iosurface);
    return iosurface;
}

static Core::MachPort create_mach_port_from_iosurface(IOSurfaceRef iosurface)
{
    auto port = IOSurfaceCreateMachPort(iosurface);
    return Core::MachPort::adopt_right(port, Core::MachPort::PortRight::Send);
}

static IOSurfaceRef create_iosurface(BitmapInfo const& info)
{
    int width = info.size.width();
    int height = info.size.height();
    int bytes_per_element = 4;
    uint32_t pixel_format = iosurface_pixel_format_from_bitmap_format(info.pixel_format);

    auto const* width_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
    auto const* height_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
    auto const* bytes_per_element_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bytes_per_element);
    auto const* pixel_format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
    VERIFY(width_number);
    VERIFY(height_number);
    VERIFY(bytes_per_element_number);
    VERIFY(pixel_format_number);

    void const* keys[] = {
        kIOSurfaceWidth,
        kIOSurfaceHeight,
        kIOSurfaceBytesPerElement,
        kIOSurfacePixelFormat,
    };
    void const* values[] = {
        width_number,
        height_number,
        bytes_per_element_number,
        pixel_format_number,
    };
    auto const* properties = CFDictionaryCreate(kCFAllocatorDefault,
        keys,
        values,
        4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    VERIFY(properties);

    auto* iosurface = IOSurfaceCreate(properties);
    VERIFY(iosurface);

    CFRelease(properties);
    CFRelease(pixel_format_number);
    CFRelease(bytes_per_element_number);
    CFRelease(height_number);
    CFRelease(width_number);

    return iosurface;
}

static Core::MachPort copy_send_right(Core::MachPort const& port)
{
    auto result = mach_port_mod_refs(mach_task_self(), port.port(), MACH_PORT_RIGHT_SEND, +1);
    VERIFY(result == KERN_SUCCESS);
    return Core::MachPort::adopt_right(port.port(), Core::MachPort::PortRight::Send);
}

static NonnullRefPtr<Bitmap> create_bitmap_from_iosurface(IOSurfaceRef iosurface, BitmapInfo const& info)
{
    CFRetain(iosurface);
    return MUST(Bitmap::create_wrapper(info.pixel_format, info.alpha_type, info.size, IOSurfaceGetBytesPerRow(iosurface), IOSurfaceGetBaseAddress(iosurface), [iosurface] {
        CFRelease(iosurface);
    }));
}

static ErrorOr<NonnullRefPtr<SkiaBackendContext>> shared_image_skia_backend_context()
{
    static RefPtr<SkiaBackendContext> context;
    if (context)
        return *context;

    auto metal_context = get_metal_context();
    if (!metal_context)
        return Error::from_string_literal("MetalImage: failed_to_create_metal_context");

    context = SkiaBackendContext::create_metal_context(metal_context.release_nonnull());
    if (!context)
        return Error::from_string_literal("MetalImage: failed_to_create_skia_backend_context");

    return *context;
}

static ErrorOr<void> upload_decoded_image_frame_to_surface(DecodedImageFrame const& frame, PaintingSurface& painting_surface)
{
    auto sk_image = sk_image_from_bitmap(frame.bitmap(), frame.color_space());
    if (!sk_image)
        return Error::from_string_literal("MetalImage: missing_source_image");

    painting_surface.lock_context();
    auto& canvas = painting_surface.canvas();
    canvas.clear(SK_ColorTRANSPARENT);
    canvas.drawImage(sk_image, 0.0f, 0.0f);
    painting_surface.unlock_context();

    if (auto surface_context = painting_surface.skia_backend_context()) {
        painting_surface.lock_context();
        surface_context->flush_and_submit(&painting_surface.sk_surface());
        painting_surface.unlock_context();
    }

    return {};
}

}
