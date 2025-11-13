/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tuple.h>
#include <LibWeb/Bindings/OffscreenCanvasPrototype.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvas);

GC::Ref<OffscreenCanvas> OffscreenCanvas::create(JS::Realm& realm, WebIDL::UnsignedLong width,
    WebIDL::UnsignedLong height)
{
    return MUST(OffscreenCanvas::construct_impl(realm, width, height));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas
WebIDL::ExceptionOr<GC::Ref<OffscreenCanvas>> OffscreenCanvas::construct_impl(
    JS::Realm& realm,
    WebIDL::UnsignedLong width,
    WebIDL::UnsignedLong height)
{
    RefPtr<Gfx::Bitmap> bitmap;
    if (width > 0 && height > 0) {
        // The new OffscreenCanvas(width, height) constructor steps are:
        auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::IntSize { width, height });

        if (bitmap_or_error.is_error()) {
            return WebIDL::InvalidStateError::create(realm, Utf16String::formatted("Error in allocating bitmap: {}", bitmap_or_error.error()));
        }
        bitmap = bitmap_or_error.release_value();
    }

    // 1. Initialize the bitmap of this to a rectangular array of transparent black pixels of the dimensions specified by width and height.
    // noop, the pixel value to set is equal to 0x00000000, which the bitmap already contains

    // 2. Initialize the width of this to width.
    // 3. Initialize the height of this to height.
    // noop, we use the height and width from the bitmap

    // FIXME: 4. Set this's inherited language to explicitly unknown.

    // FIXME: 5. Set this's inherited direction to "ltr".

    // 6. Let global be the relevant global object of this.
    auto& global = realm.global_object();

    // 7. If global is a Window object:
    if (is<HTML::Window>(global)) {
        auto& window = as<HTML::Window>(global);
        // 1.Let element be the document element of global's associated Document.
        auto* element = window.associated_document().document_element();
        // 2. If element is not null :
        if (element) {
            // FIXME: 1. Set the inherited language of this to element's language.
            // FIXME: 2. Set the inherited direction of this to element's directionality.
        }
    }

    return realm.create<OffscreenCanvas>(realm, bitmap);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas
OffscreenCanvas::OffscreenCanvas(JS::Realm& realm, RefPtr<Gfx::Bitmap> bitmap)
    : EventTarget(realm)
    , m_bitmap { move(bitmap) }
{
}

OffscreenCanvas::~OffscreenCanvas() = default;

WebIDL::ExceptionOr<void> OffscreenCanvas::transfer_steps(HTML::TransferDataEncoder&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) OffscreenCanvas::transfer_steps(HTML::TransferDataEncoder&)");
    return {};
}

WebIDL::ExceptionOr<void> OffscreenCanvas::transfer_receiving_steps(HTML::TransferDataDecoder&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) OffscreenCanvas::transfer_receiving_steps(HTML::TransferDataDecoder&)");
    return {};
}

HTML::TransferType OffscreenCanvas::primary_interface() const
{
    // FIXME: Implement this
    dbgln("(STUBBED) OffscreenCanvas::primary_interface()");
    return {};
}

WebIDL::UnsignedLong OffscreenCanvas::width() const
{
    if (!m_bitmap)
        return 0;

    return m_bitmap->size().width();
}

WebIDL::UnsignedLong OffscreenCanvas::height() const
{
    if (!m_bitmap)
        return 0;

    return m_bitmap->size().height();
}

void OffscreenCanvas::reset_context_to_default_state()
{
    m_context.visit(
        [](GC::Ref<OffscreenCanvasRenderingContext2D>& context) {
            context->reset_to_default_state();
        },
        [](GC::Ref<WebGL::WebGLRenderingContext>& context) {
            context->reset_to_default_state();
        },
        [](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
            context->reset_to_default_state();
        },
        [](Empty) {
            // Do nothing.
        });
}

WebIDL::ExceptionOr<void> OffscreenCanvas::set_new_bitmap_size(Gfx::IntSize new_size)
{
    if (new_size.width() == 0 || new_size.height() == 0)
        m_bitmap = nullptr;
    else {
        // FIXME: Other browsers appear to not throw for unreasonable sizes being set. We could consider deferring allocation of the bitmap until it is used,
        //        but for now, lets just allocate it here and throw if it fails instead of crashing.
        auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::IntSize { new_size.width(), new_size.height() });
        if (bitmap_or_error.is_error()) {
            return WebIDL::InvalidStateError::create(realm(), Utf16String::formatted("Error in allocating bitmap: {}", bitmap_or_error.error()));
        }
        m_bitmap = bitmap_or_error.release_value();
    }

    m_context.visit(
        [&](GC::Ref<OffscreenCanvasRenderingContext2D>& context) {
            context->set_size(new_size);
        },
        [&](GC::Ref<WebGL::WebGLRenderingContext>& context) {
            context->set_size(new_size);
        },
        [&](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
            context->set_size(new_size);
        },
        [](Empty) {
            // Do nothing.
        });
    return {};
}

RefPtr<Gfx::Bitmap> OffscreenCanvas::bitmap() const
{
    return m_bitmap;
}

WebIDL::ExceptionOr<void> OffscreenCanvas::set_width(WebIDL::UnsignedLong value)
{
    Gfx::IntSize current_size = bitmap_size_for_canvas();
    current_size.set_width(value);

    TRY(set_new_bitmap_size(current_size));
    reset_context_to_default_state();
    return {};
}
WebIDL::ExceptionOr<void> OffscreenCanvas::set_height(WebIDL::UnsignedLong value)
{
    Gfx::IntSize current_size = bitmap_size_for_canvas();
    current_size.set_height(value);

    TRY(set_new_bitmap_size(current_size));
    reset_context_to_default_state();
    return {};
}

Gfx::IntSize OffscreenCanvas::bitmap_size_for_canvas() const
{
    if (!m_bitmap)
        return { 0, 0 };
    return m_bitmap->size();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas-getcontext
JS::ThrowCompletionOr<OffscreenRenderingContext> OffscreenCanvas::get_context(Bindings::OffscreenRenderingContextId contextId, JS::Value options)
{
    // 1. If options is not an object, then set options to null.
    if (!options.is_object())
        options = JS::js_null();

    // 2. Set options to the result of converting options to a JavaScript value.
    // NOTE: No-op.

    // 3. Run the steps in the cell of the following table whose column header matches this OffscreenCanvas object's context mode and whose row header matches contextId:
    // NOTE: See the spec for the full table.
    if (contextId == Bindings::OffscreenRenderingContextId::_2d) {
        if (TRY(create_2d_context(options)) == HasOrCreatedContext::Yes)
            return GC::make_root(*m_context.get<GC::Ref<HTML::OffscreenCanvasRenderingContext2D>>());

        return Empty {};
    }

    if (contextId == Bindings::OffscreenRenderingContextId::Webgl) {
        dbgln("(STUBBED) OffscreenCanvas::get_context(Webgl)");

        return Empty {};
    }

    if (contextId == Bindings::OffscreenRenderingContextId::Webgl2) {
        dbgln("(STUBBED) OffscreenCanvas::get_context(Webgl2)");

        return Empty {};
    }

    return Empty {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas-transfertoimagebitmap
WebIDL::ExceptionOr<GC::Ref<ImageBitmap>> OffscreenCanvas::transfer_to_image_bitmap()
{
    // The transferToImageBitmap() method, when invoked, must run the following steps :

    // FIXME: 1. If the value of this OffscreenCanvas object's [[Detached]] internal slot is set to true, then throw an "InvalidStateError" DOMException.

    // 2. If this OffscreenCanvas object's context mode is set to none, then throw an "InvalidStateError" DOMException.
    if (m_context.has<Empty>()) {
        return WebIDL::InvalidStateError::create(realm(), "OffscreenCanvas has no context"_utf16);
    }

    // 3. Let image be a newly created ImageBitmap object that references the same underlying bitmap data as this OffscreenCanvas object's bitmap.
    auto image = ImageBitmap::create(realm());
    image->set_bitmap(m_bitmap);

    // 4. Set this OffscreenCanvas object's bitmap to reference a newly created bitmap of the same dimensions and color space as the previous bitmap, and with its pixels initialized to transparent black, or opaque black if the rendering context' s alpha is false.
    // FIXME: implement the checking of the alpha from the context
    auto size = bitmap_size_for_canvas();
    if (size.is_empty()) {
        m_bitmap = nullptr;
    } else {
        m_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, size));
    }

    // 5. Return image.
    return image;
}

static Tuple<FlyString, Optional<double>> options_convert_or_default(Optional<ImageEncodeOptions> options)
{

    if (!options.has_value()) {
        return { "image/png"_fly_string, {} };
    }

    return { options->type, options->quality };
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas-converttoblob
GC::Ref<WebIDL::Promise> OffscreenCanvas::convert_to_blob(Optional<ImageEncodeOptions> maybe_options)
{
    // The convertToBlob(options) method, when invoked, must run the following steps:

    // FIXME: 1. If the value of this OffscreenCanvas object's [[Detached]] internal slot is set to true, then return a promise rejected with an "InvalidStateError" DOMException.

    // FIXME: 2. If this OffscreenCanvas object's context mode is 2d and the rendering context's output bitmap's origin-clean flag is set to false, then return a promise rejected with a "SecurityError" DOMException.

    auto size = bitmap_size_for_canvas();

    // 3. If this OffscreenCanvas object's bitmap has no pixels (i.e., either its horizontal dimension or its vertical dimension is zero) then return a promise rejected with an "IndexSizeError" DOMException.
    if (size.height() == 0 or size.width() == 0) {
        auto error = WebIDL::IndexSizeError::create(realm(), "OffscreenCanvas has invalid dimensions. The bitmap has no pixels"_utf16);

        return WebIDL::create_rejected_promise_from_exception(realm(), error);
    }

    // 4. Let bitmap be a copy of this OffscreenCanvas object's bitmap.
    RefPtr<Gfx::Bitmap> bitmap;
    if (m_bitmap)
        bitmap = MUST(m_bitmap->clone());

    // 5. Let result be a new promise object.
    auto result_promise = WebIDL::create_promise(realm());

    // 6. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, result_promise, bitmap, maybe_options] {
        // 1. Let file be a serialization of bitmap as a file, with options's type and quality if present.
        Optional<SerializeBitmapResult> file_result {};
        auto options = options_convert_or_default(maybe_options);

        if (auto result = serialize_bitmap(*bitmap, options.get<0>(), options.get<1>()); !result.is_error())
            file_result = result.release_value();

        // 2. Queue an element task on the canvas blob serialization task source given the canvas element to run these steps:
        // FIXME: wait for spec bug to be resolve: https://github.com/whatwg/html/issues/11101

        // AD-HOC: queue the task in an appropiate queue. This depends if the global object is a window or a worker
        Function<void()> task_to_queue = [this, result_promise, file_result = move(file_result)] -> void {
            HTML::TemporaryExecutionContext context(realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            // 1. If file is null, then reject result with an "EncodingError" DOMException.
            if (!file_result.has_value()) {
                auto error = WebIDL::EncodingError::create(realm(), "Failed to convert OffscreenCanvas to Blob"_utf16);

                WebIDL::reject_promise(realm(), result_promise, error);
            } else {
                // 1. If result is non-null, resolve result with a new Blob object, created in the relevant realm of this OffscreenCanvas object, representing file. [FILEAPI]
                auto type = String::from_utf8(file_result->mime_type);
                if (type.is_error()) {
                    auto error = WebIDL::EncodingError::create(realm(), Utf16String::formatted("OOM Error while converting string in OffscreenCanvas to blob: {}", type.error()));
                    WebIDL::reject_promise(realm(), result_promise, error);
                    return;
                }

                GC::Ptr<FileAPI::Blob> blob_result = FileAPI::Blob::create(realm(), file_result->buffer, type.release_value());
                WebIDL::resolve_promise(realm(), result_promise, blob_result);
            }
        };

        auto& global_object = HTML::relevant_global_object(*this);

        // AD-HOC: if the global_object is a window, queue an element task on the canvas blob serialization task source
        if (is<HTML::Window>(global_object)) {
            auto& window = as<HTML::Window>(global_object);
            window.associated_document().document_element()->queue_an_element_task(Task::Source::CanvasBlobSerializationTask, move(task_to_queue));
            return;
        }

        // AD-HOC: the global object only can be a worker or a window
        VERIFY(is<HTML::WorkerGlobalScope>(global_object));

        auto& worker = as<HTML::WorkerGlobalScope>(global_object);

        // AD-HOC: if the global_object is a worker, queue a global task on the canvas blob serialization task source
        HTML::queue_global_task(Task::Source::CanvasBlobSerializationTask, worker, GC::create_function(heap(), move(task_to_queue)));
    }));

    // 7. Return result.
    return result_promise;
}
void OffscreenCanvas::set_oncontextlost(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::contextlost, event_handler);
}

GC::Ptr<WebIDL::CallbackType> OffscreenCanvas::oncontextlost()
{
    return event_handler_attribute(HTML::EventNames::contextlost);
}

void OffscreenCanvas::set_oncontextrestored(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::contextrestored, event_handler);
}

GC::Ptr<WebIDL::CallbackType> OffscreenCanvas::oncontextrestored()
{
    return event_handler_attribute(HTML::EventNames::contextrestored);
}

void OffscreenCanvas::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OffscreenCanvas);
}

void OffscreenCanvas::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_context.visit(
        [&](GC::Ref<OffscreenCanvasRenderingContext2D>& context) {
            visitor.visit(context);
        },
        [&](GC::Ref<WebGL::WebGLRenderingContext>& context) {
            visitor.visit(context);
        },
        [&](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
            visitor.visit(context);
        },
        [](Empty) {
        });
}

JS::ThrowCompletionOr<OffscreenCanvas::HasOrCreatedContext> OffscreenCanvas::create_2d_context(JS::Value options)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<OffscreenCanvasRenderingContext2D>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    m_context = TRY(OffscreenCanvasRenderingContext2D::create(realm(), *this, options));
    return HasOrCreatedContext::Yes;
}

}
