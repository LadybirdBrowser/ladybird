/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/ImageBitmapRenderingContext.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/exports.rs.
void* web_image_bitmap_rendering_context_create(JSVM*, void* canvas, void* canvas_host, bool alpha);
Gfx::DecodedImageFrame const* web_image_bitmap_rendering_context_frame(void const* context);
bool web_image_bitmap_rendering_context_is_blank(void const* context);
bool web_image_bitmap_rendering_context_origin_clean(void const* context);
bool web_image_bitmap_rendering_context_alpha(void const* context);
}

GC::Ref<ImageBitmapRenderingContext> ImageBitmapRenderingContext::create(HTMLCanvasElement& canvas, bool alpha)
{
    // NB: The canvas is a GC cell and a CanvasHost at different addresses, so Rust gets both.
    GC::Cell& canvas_cell = canvas;
    CanvasHost& canvas_host = canvas;
    auto* context = web_image_bitmap_rendering_context_create(JS::HostABI::vm_to_abi(JS::VM::the()), &canvas_cell, &canvas_host, alpha);
    return GC::Ref { *static_cast<ImageBitmapRenderingContext*>(context) };
}

Gfx::DecodedImageFrame const* ImageBitmapRenderingContext::frame() const
{
    return web_image_bitmap_rendering_context_frame(this);
}

bool ImageBitmapRenderingContext::is_blank() const
{
    return web_image_bitmap_rendering_context_is_blank(this);
}

bool ImageBitmapRenderingContext::origin_clean() const
{
    return web_image_bitmap_rendering_context_origin_clean(this);
}

bool ImageBitmapRenderingContext::alpha() const
{
    return web_image_bitmap_rendering_context_alpha(this);
}

}
