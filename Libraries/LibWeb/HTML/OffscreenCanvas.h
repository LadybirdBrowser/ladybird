/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

// https://html.spec.whatwg.org/multipage/canvas.html#offscreenrenderingcontext
// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using OffscreenRenderingContext = Variant<GC::Ref<OffscreenCanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>, Empty>;

// https://html.spec.whatwg.org/multipage/canvas.html#offscreencanvas
class OffscreenCanvas : public DOM::EventTarget
    , public Web::Bindings::Transferable
    , public CanvasHost {
    WEB_WRAPPABLE(OffscreenCanvas, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(OffscreenCanvas);

public:
    static WebIDL::ExceptionOr<GC::Ref<OffscreenCanvas>> create(
        DOM::EventTarget& relevant_global_object,
        WebIDL::UnsignedLong width,
        WebIDL::UnsignedLong height);

    virtual ~OffscreenCanvas() override;

    JS::Object& relevant_global_object() const;

    // ^Web::Bindings::Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(JS::Realm&, HTML::TransferDataEncoder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(JS::Realm&, HTML::TransferDataDecoder&) override;
    virtual HTML::TransferType primary_interface() const override;

    WebIDL::UnsignedLong width() const;
    WebIDL::UnsignedLong height() const;

    RefPtr<Gfx::Bitmap> bitmap() const;

    WebIDL::ExceptionOr<void> set_width(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<void> set_height(WebIDL::UnsignedLong);

    // ^CanvasHost
    virtual Gfx::IntSize bitmap_size_for_canvas() const override;
    virtual Canvas2DContextBase* canvas_2d_context() const override { return nullptr; }
    virtual WebGL::WebGLRenderingContextBase* canvas_webgl_context() const override;
    virtual Page& canvas_page() override;
    virtual DOM::EventTarget& canvas_event_target() override { return *this; }
    virtual JS::Object& canvas_relevant_global_object() const override { return relevant_global_object(); }
    virtual GC::Ptr<Bindings::Wrappable> canvas_relevant_global_impl() const override { return m_global_object; }
    virtual CSS::FontComputer& canvas_font_computer() override;
    virtual CSS::ComputationContext canvas_font_computation_context() override;
    virtual CSS::ColorResolutionContext canvas_color_resolution_context() override;
    virtual CSSPixelRect canvas_viewport_rect() const override { return {}; }

    WebIDL::ExceptionOr<GC::Ref<ImageBitmap>> transfer_to_image_bitmap();

    void set_oncontextlost(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oncontextlost();
    void set_oncontextrestored(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oncontextrestored();

    enum class HasOrCreatedContext {
        No,
        Yes,
    };
    HasOrCreatedContext create_2d_context(CanvasRenderingContext2DSettings);
    OffscreenRenderingContext const& context() const { return m_context; }

private:
    OffscreenCanvas(GC::Ref<DOM::EventTarget> relevant_global_object, RefPtr<Gfx::Bitmap> bitmap);

    virtual void visit_edges(Cell::Visitor&) override;

    void reset_context_to_default_state();
    WebIDL::ExceptionOr<void> set_new_bitmap_size(Gfx::IntSize new_size);

    Variant<GC::Ref<HTML::OffscreenCanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>, Empty> m_context;

    RefPtr<Gfx::Bitmap> m_bitmap;
    GC::Ref<DOM::EventTarget> m_global_object;
};

}
