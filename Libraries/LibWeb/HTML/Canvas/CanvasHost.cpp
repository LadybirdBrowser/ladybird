/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/Rect.h>
#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
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
    if (area.value() > static_cast<u64>(Gfx::max_canvas_area)) {
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
        return webgl_context->context().read_back_drawing_buffer({ {}, size });

    if (auto* context = canvas_2d_context()) {
        context->ensure_backing_storage();
        if (auto pixels = context->read_pixels({ {}, size }); pixels && pixels->size() == size)
            return pixels;
        return nullptr;
    }

    return create_transparent_canvas_bitmap(size);
}

// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
bool CanvasHost::is_origin_clean() const
{
    if (auto* context = canvas_2d_context())
        return context->origin_clean();
    // FIXME: WebGL and WebGL2 contexts do not track the origin-clean flag yet.
    return true;
}

}
