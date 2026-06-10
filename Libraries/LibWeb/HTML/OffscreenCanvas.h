/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

// https://html.spec.whatwg.org/multipage/canvas.html#offscreenrenderingcontext
// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using OffscreenRenderingContext = Variant<GC::Ref<OffscreenCanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>, Empty>;

// https://html.spec.whatwg.org/multipage/canvas.html#offscreencanvas
class OffscreenCanvas : public DOM::EventTarget
    , public Web::Bindings::Transferable {
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

    Gfx::IntSize bitmap_size_for_canvas() const;

    WebIDL::ExceptionOr<GC::Ref<ImageBitmap>> transfer_to_image_bitmap();

    void set_oncontextlost(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oncontextlost();
    void set_oncontextrestored(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oncontextrestored();

    CSS::ComputationContext canvas_font_computation_context() const;

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
