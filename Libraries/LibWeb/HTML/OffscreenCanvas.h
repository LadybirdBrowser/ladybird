/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#offscreenrenderingcontext
// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using OffscreenRenderingContext = Variant<GC::Root<OffscreenCanvasRenderingContext2D>, GC::Root<WebGL::WebGLRenderingContext>, GC::Root<WebGL::WebGL2RenderingContext>, Empty>;

// https://html.spec.whatwg.org/multipage/canvas.html#imageencodeoptions
struct ImageEncodeOptions {
    FlyString type { "image/png"_fly_string };
    Optional<double> quality;
};

// https://html.spec.whatwg.org/multipage/canvas.html#offscreencanvas
class OffscreenCanvas : public DOM::EventTarget
    , public Web::Bindings::Transferable {
    WEB_PLATFORM_OBJECT(OffscreenCanvas, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(OffscreenCanvas);

public:
    static GC::Ref<OffscreenCanvas> create(JS::Realm&, WebIDL::UnsignedLong width, WebIDL::UnsignedLong height);

    static WebIDL::ExceptionOr<GC::Ref<OffscreenCanvas>> construct_impl(
        JS::Realm&,
        WebIDL::UnsignedLong width,
        WebIDL::UnsignedLong height);

    virtual ~OffscreenCanvas() override;

    // ^Web::Bindings::Transferable
    virtual WebIDL::ExceptionOr<void> transfer_steps(HTML::TransferDataEncoder&) override;
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(HTML::TransferDataDecoder&) override;
    virtual HTML::TransferType primary_interface() const override;

    WebIDL::UnsignedLong width() const;
    WebIDL::UnsignedLong height() const;

    RefPtr<Gfx::Bitmap> bitmap() const;

    WebIDL::ExceptionOr<void> set_width(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<void> set_height(WebIDL::UnsignedLong);

    Gfx::IntSize bitmap_size_for_canvas() const;

    JS::ThrowCompletionOr<OffscreenRenderingContext> get_context(Bindings::OffscreenRenderingContextId contextId, JS::Value options);

    WebIDL::ExceptionOr<GC::Ref<ImageBitmap>> transfer_to_image_bitmap();

    GC::Ref<WebIDL::Promise> convert_to_blob(Optional<ImageEncodeOptions> options);

    void set_oncontextlost(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oncontextlost();
    void set_oncontextrestored(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oncontextrestored();

private:
    OffscreenCanvas(JS::Realm&, RefPtr<Gfx::Bitmap> bitmap);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    enum class HasOrCreatedContext {
        No,
        Yes,
    };
    JS::ThrowCompletionOr<HasOrCreatedContext> create_2d_context(JS::Value options);

    void reset_context_to_default_state();
    WebIDL::ExceptionOr<void> set_new_bitmap_size(Gfx::IntSize new_size);

    Variant<GC::Ref<HTML::OffscreenCanvasRenderingContext2D>, GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>, Empty> m_context;

    RefPtr<Gfx::Bitmap> m_bitmap;
};

}
