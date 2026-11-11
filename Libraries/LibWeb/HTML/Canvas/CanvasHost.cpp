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
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>
#include <LibWeb/HTML/ImageBitmapRenderingContext.h>
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

static Optional<Gfx::DecodedImageFrame> transparent_canvas_frame(Gfx::IntSize size)
{
    auto bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size);
    if (bitmap.is_error())
        return {};
    auto frame = Gfx::DecodedImageFrame::create(*bitmap.value());
    if (frame.is_error())
        return {};
    return frame.release_value();
}

Optional<Gfx::DecodedImageFrame> CanvasHost::get_srgb_frame_from_surface()
{
    auto const size = bitmap_size_for_canvas();
    if (size.is_empty())
        return {};

    // NB: A blank output bitmap of an ImageBitmapRenderingContext is the transparent bitmap below.
    if (auto* context = canvas_bitmap_renderer_context(); context && !context->is_blank()) {
        // FIXME: Convert bitmap data that isn't in the sRGB color space.
        if (auto const* frame = context->frame(); frame)
            return frame->with_pixels_reinterpreted_in({});
        return {};
    }

    if (auto* webgl_context = canvas_webgl_context())
        return webgl_context->read_back_drawing_buffer({ {}, size });

    if (auto* context = canvas_2d_context()) {
        context->ensure_backing_storage();
        if (auto pixels = context->read_pixels({ {}, size }); pixels.has_value() && pixels->size() == size)
            return pixels;
        return {};
    }

    return transparent_canvas_frame(size);
}

Optional<Gfx::DecodedImageFrame> CanvasHost::get_frame_from_surface()
{
    // NB: The output bitmap of an ImageBitmapRenderingContext refers to its bitmap data, with its color space.
    if (auto* context = canvas_bitmap_renderer_context(); context && !context->is_blank()) {
        if (auto const* frame = context->frame())
            return *frame;
        return {};
    }

    if (auto* context = canvas_2d_context(); context && context->color_space() != Bindings::PredefinedColorSpace::Srgb) {
        auto const size = bitmap_size_for_canvas();
        if (size.is_empty())
            return {};
        context->ensure_backing_storage();
        if (auto pixels = context->read_pixels({ {}, size }, Gfx::AlphaType::Premultiplied, context->color_space()); pixels.has_value() && pixels->size() == size)
            return pixels;
        return {};
    }

    return get_srgb_frame_from_surface();
}

Optional<Gfx::DecodedImageFrame> CanvasHost::get_frame_for_serialization(Utf16View type)
{
    if (serialization_of_the_bitmap_supports_color_profiles(type))
        return get_frame_from_surface();
    return get_srgb_frame_from_surface();
}

// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
bool CanvasHost::is_origin_clean() const
{
    if (auto* context = canvas_2d_context())
        return context->origin_clean();
    if (auto* context = canvas_bitmap_renderer_context())
        return context->origin_clean();
    // NB: The texture uploads of a WebGL context throw a "SecurityError" DOMException for a source that is not
    //     origin-clean, so its bitmap stays origin-clean. A video is the exception, as long as its media data is
    //     never CORS-cross-origin (see image_is_not_origin_clean()).
    return true;
}

}
