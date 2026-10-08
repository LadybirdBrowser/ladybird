/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::HTML {

class CanvasHost {
public:
    virtual ~CanvasHost() = default;

    virtual Gfx::IntSize bitmap_size_for_canvas() const = 0;

    virtual Canvas2DContextBase* canvas_2d_context() const = 0;
    virtual WebGL::WebGLRenderingContextBase* canvas_webgl_context() const = 0;

    virtual Page& canvas_page() = 0;
    virtual DOM::EventTarget& canvas_event_target() = 0;
    virtual JS::Object& canvas_relevant_global_object() const = 0;
    virtual GC::Ptr<Bindings::Wrappable> canvas_relevant_global_impl() const = 0;

    virtual void did_change_canvas_content() { }
    virtual void did_create_canvas_backing_storage() { }

    virtual CSS::FontComputer& canvas_font_computer() = 0;
    virtual CSS::ComputationContext canvas_font_computation_context() = 0;
    virtual CSS::ColorResolutionStyle canvas_color_resolution_style() = 0;
    virtual CSSPixelRect canvas_viewport_rect() const = 0;

    virtual RefPtr<Gfx::Bitmap> get_bitmap_from_surface();
    virtual bool is_origin_clean() const;

protected:
    static Gfx::IntSize bitmap_size_for_dimensions(u64 width, u64 height);
};

}
