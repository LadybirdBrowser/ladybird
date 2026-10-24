/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibCompositing/CanvasSize.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Rect.h>
#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/Canvas/PredefinedColorSpace.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::HTML {

Gfx::IntSize CanvasHost::bitmap_size_for_dimensions(u64 width, u64 height)
{
    if (width > NumericLimits<int>::max() || height > NumericLimits<int>::max()) {
        dbgln("Refusing to create {}x{} canvas (exceeds maximum dimensions)", width, height);
        return {};
    }

    Checked<u64> area = width;
    area *= height;

    if (area.has_overflow()) {
        dbgln("Refusing to create {}x{} canvas (overflow)", width, height);
        return {};
    }
    if (area.value() > static_cast<u64>(Compositing::max_canvas_area)) {
        dbgln("Refusing to create {}x{} canvas (exceeds maximum size)", width, height);
        return {};
    }
    return { static_cast<int>(width), static_cast<int>(height) };
}

static RefPtr<Gfx::Bitmap> create_transparent_canvas_bitmap(Gfx::IntSize size)
{
    auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size);
    if (bitmap_or_error.is_error())
        return nullptr;
    return bitmap_or_error.release_value();
}

RefPtr<Gfx::Bitmap> CanvasHost::get_bitmap_from_surface()
{
    auto const size = bitmap_size_for_canvas();
    if (size.is_empty())
        return nullptr;

    if (auto* webgl_context = canvas_webgl_context())
        return webgl_context->read_back_drawing_buffer({ {}, size });

    if (auto* context = canvas_2d_context()) {
        context->ensure_backing_storage();
        if (auto pixels = context->read_pixels({ {}, size }); pixels && pixels->size() == size)
            return pixels;
        return nullptr;
    }

    return create_transparent_canvas_bitmap(size);
}

Optional<Gfx::DecodedImageFrame> CanvasHost::get_frame_from_surface()
{
    if (auto* context = canvas_2d_context(); context && context->color_space() != Bindings::PredefinedColorSpace::Srgb) {
        auto const size = bitmap_size_for_canvas();
        if (size.is_empty())
            return {};
        context->ensure_backing_storage();
        auto color_space = context->color_space();
        if (auto pixels = context->read_pixels({ {}, size }, Gfx::AlphaType::Premultiplied, color_space); pixels && pixels->size() == size)
            return Gfx::DecodedImageFrame { *pixels, gfx_color_space_for(color_space) };
        return {};
    }

    auto bitmap = get_bitmap_from_surface();
    if (!bitmap)
        return {};
    return Gfx::DecodedImageFrame { *bitmap };
}

Optional<Gfx::DecodedImageFrame> CanvasHost::get_frame_for_serialization(Utf16View type)
{
    if (serialization_of_the_bitmap_supports_color_profiles(type))
        return get_frame_from_surface();
    auto bitmap = get_bitmap_from_surface();
    if (!bitmap)
        return {};
    return Gfx::DecodedImageFrame { *bitmap };
}

// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
bool CanvasHost::is_origin_clean() const
{
    if (auto* context = canvas_2d_context())
        return context->origin_clean();
    // NB: The texture uploads of a WebGL context throw a "SecurityError" DOMException for a source that is not
    //     origin-clean, so its bitmap stays origin-clean. A video is the exception, as long as its media data is
    //     never CORS-cross-origin (see image_is_not_origin_clean()).
    return true;
}

}
