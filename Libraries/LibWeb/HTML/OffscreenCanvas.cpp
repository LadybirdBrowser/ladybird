/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tuple.h>
#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/CanvasRenderingContext2DSettings.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/BindingsGlue.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvas);

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas
GC::Ref<OffscreenCanvas> OffscreenCanvas::create(
    DOM::EventTarget& relevant_global_object,
    WebIDL::UnsignedLongLong width,
    WebIDL::UnsignedLongLong height)
{
    // The new OffscreenCanvas(width, height) constructor steps are:

    // 1. Initialize the bitmap of this to a rectangular array of transparent black pixels of the dimensions specified by width and height.
    // 2. Initialize the width of this to width.
    // 3. Initialize the height of this to height.

    // FIXME: 4. Set this's inherited language to explicitly unknown.

    // FIXME: 5. Set this's inherited direction to "ltr".

    // 7. If global is a Window object:
    if (auto* window = as_if<Window>(relevant_global_object)) {
        // 1.Let element be the document element of global's associated Document.
        auto* element = window->associated_document().document_element();
        // 2. If element is not null :
        if (element) {
            // FIXME: 1. Set the inherited language of this to element's language.
            // FIXME: 2. Set the inherited direction of this to element's directionality.
        }
    }

    return GC::Heap::the().allocate<OffscreenCanvas>(relevant_global_object, width, height);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas
OffscreenCanvas::OffscreenCanvas(GC::Ref<DOM::EventTarget> relevant_global_object, WebIDL::UnsignedLongLong width, WebIDL::UnsignedLongLong height)
    : EventTarget()
    , m_width(width)
    , m_height(height)
    , m_global_object(relevant_global_object)
{
}

OffscreenCanvas::~OffscreenCanvas() = default;

JS::Object& OffscreenCanvas::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-offscreencanvas-interface:transfer-steps
WebIDL::ExceptionOr<void> OffscreenCanvas::transfer_steps(JS::Realm& realm, HTML::TransferDataEncoder& data_holder)
{
    // 1. If value's context mode is not equal to none, then throw an "InvalidStateError" DOMException.
    if (!m_context.has<Empty>())
        return WebIDL::InvalidStateError::create("Cannot transfer an OffscreenCanvas that has a rendering context"_utf16);

    // 2. Set value's context mode to detached.
    // NB: The [[Detached]] internal slot is set once these steps return, and it also stands for this context mode.

    // 3. Let width and height be the dimensions of value's bitmap.
    auto width = m_width;
    auto height = m_height;

    // FIXME: 4. Let language and direction be the values of value's inherited language and inherited direction.

    // 5. Unset value's bitmap.
    m_width = 0;
    m_height = 0;

    // 6. Set dataHolder.[[Width]] to width and dataHolder.[[Height]] to height.
    TRY(encode_or_throw_data_clone_error(realm, data_holder, width));
    TRY(encode_or_throw_data_clone_error(realm, data_holder, height));

    // FIXME: 7. Set dataHolder.[[Language]] to language and dataHolder.[[Direction]] to direction.

    // 8. Set dataHolder.[[PlaceholderCanvas]] to be a weak reference to value's placeholder canvas element, if value
    //    has one, or null if it does not.
    auto placeholder_link = m_placeholder_link;
    m_placeholder_link.clear();
    TRY(encode_or_throw_data_clone_error(realm, data_holder, placeholder_link.has_value()));
    if (placeholder_link.has_value()) {
        TRY(encode_or_throw_data_clone_error(realm, data_holder, placeholder_link->canvas_id.value()));
        TRY(encode_or_throw_data_clone_error(realm, data_holder, placeholder_link->secret));
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-offscreencanvas-interface:transfer-receiving-steps
WebIDL::ExceptionOr<void> OffscreenCanvas::transfer_receiving_steps(JS::Realm& realm, HTML::TransferDataDecoder& data_holder)
{
    // 1. Initialize value's bitmap to a rectangular array of transparent black pixels with width given by
    //    dataHolder.[[Width]] and height given by dataHolder.[[Height]].
    m_width = TRY(decode_or_throw_data_clone_error<WebIDL::UnsignedLongLong>(realm, data_holder));
    m_height = TRY(decode_or_throw_data_clone_error<WebIDL::UnsignedLongLong>(realm, data_holder));

    // FIXME: 2. Set value's inherited language to dataHolder.[[Language]] and its inherited direction to
    //           dataHolder.[[Direction]].

    // 3. If dataHolder.[[PlaceholderCanvas]] is not null, set value's placeholder canvas element to
    //    dataHolder.[[PlaceholderCanvas]] (while maintaining the weak reference semantics).
    if (TRY(decode_or_throw_data_clone_error<bool>(realm, data_holder))) {
        auto canvas_id = TRY(decode_or_throw_data_clone_error<u64>(realm, data_holder));
        auto secret = TRY(decode_or_throw_data_clone_error<u64>(realm, data_holder));
        m_placeholder_link = Compositor::PlaceholderCanvasLink { Compositing::CanvasId { canvas_id }, secret };
    }

    return {};
}

HTML::TransferType OffscreenCanvas::primary_interface() const
{
    return TransferType::OffscreenCanvas;
}

void OffscreenCanvas::replace_bitmap()
{
    auto size = bitmap_size_for_canvas();
    m_context.visit(
        [&](auto& context) {
            context->set_size(size);
            context->reset_to_default_state();
            did_change_canvas_content();
        },
        [](Empty) {
            // Do nothing.
        });
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas-width
WebIDL::ExceptionOr<void> OffscreenCanvas::set_width(WebIDL::UnsignedLongLong value)
{
    if (is_detached())
        return WebIDL::InvalidStateError::create("OffscreenCanvas is detached"_utf16);

    m_width = value;
    replace_bitmap();
    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas-height
WebIDL::ExceptionOr<void> OffscreenCanvas::set_height(WebIDL::UnsignedLongLong value)
{
    if (is_detached())
        return WebIDL::InvalidStateError::create("OffscreenCanvas is detached"_utf16);

    m_height = value;
    replace_bitmap();
    return {};
}

Gfx::IntSize OffscreenCanvas::bitmap_size_for_canvas() const
{
    return bitmap_size_for_dimensions(m_width, m_height);
}

Canvas2DContextBase* OffscreenCanvas::canvas_2d_context() const
{
    if (auto const* context = m_context.get_pointer<GC::Ref<OffscreenCanvasRenderingContext2D>>())
        return context->ptr();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-offscreencanvas-transfertoimagebitmap
WebIDL::ExceptionOr<GC::Ref<ImageBitmap>> OffscreenCanvas::transfer_to_image_bitmap()
{
    // The transferToImageBitmap() method, when invoked, must run the following steps :

    // 1. If the value of this OffscreenCanvas object's [[Detached]] internal slot is set to true, then throw an "InvalidStateError" DOMException.
    if (is_detached())
        return WebIDL::InvalidStateError::create("OffscreenCanvas is detached"_utf16);

    // 2. If this OffscreenCanvas object's context mode is set to none, then throw an "InvalidStateError" DOMException.
    if (m_context.has<Empty>()) {
        return WebIDL::InvalidStateError::create("OffscreenCanvas has no context"_utf16);
    }

    // 3. Let image be a newly created ImageBitmap object that references the same underlying bitmap data as this OffscreenCanvas object's bitmap.
    auto image = ImageBitmap::create();
    image->set_bitmap(get_bitmap_from_surface());

    // 4. Set this OffscreenCanvas object's bitmap to reference a newly created bitmap of the same dimensions and color space as the previous bitmap, and with its pixels initialized to transparent black, or opaque black if the rendering context' s alpha is false.
    if (auto* context = m_context.get_pointer<GC::Ref<OffscreenCanvasRenderingContext2D>>())
        (*context)->replace_bitmap_with_cleared_bitmap();
    else if (auto* webgl_context = canvas_webgl_context())
        webgl_context->context().clear_drawing_buffer();

    // 5. Return image.
    return image;
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

WebGL::WebGLRenderingContextBase* OffscreenCanvas::canvas_webgl_context() const
{
    return m_context.visit(
        [](GC::Ref<WebGL::WebGLRenderingContext> const& context) -> WebGL::WebGLRenderingContextBase* { return context.ptr(); },
        [](GC::Ref<WebGL::WebGL2RenderingContext> const& context) -> WebGL::WebGLRenderingContextBase* { return context.ptr(); },
        [](auto const&) -> WebGL::WebGLRenderingContextBase* { return nullptr; });
}

Page& OffscreenCanvas::canvas_page()
{
    return Bindings::principal_host_defined_page(relevant_global_object().shape().realm());
}

// https://drafts.csswg.org/css-font-loading/#font-source
CSS::FontComputer& OffscreenCanvas::canvas_font_computer()
{
    // 2. Otherwise, object's font style source object is an OffscreenCanvas object:

    // 1. Let global be object's relevant global object.
    auto& global_object = relevant_global_object();

    // 2. If global is a Window object, then return global's associated Document.
    if (auto* window = window_from_global_object(global_object))
        return window->associated_document().font_computer();

    // 3. Assert: global implements WorkerGlobalScope.
    auto* worker_global_scope = Bindings::worker_global_scope_from_global_object(global_object);
    VERIFY(worker_global_scope);

    // 4. Return global.
    return worker_global_scope->font_computer();
}

CSS::ColorResolutionStyle OffscreenCanvas::canvas_color_resolution_style()
{
    return {};
}

CSS::ComputationContext OffscreenCanvas::canvas_font_computation_context()
{
    // NB: The default font for a canvas is 10px sans-serif so we use a point size of 8 here.
    CSS::Length::FontMetrics font_metrics { 10, Platform::FontPlugin::the().default_font(8)->pixel_metrics(), CSS::InitialValues::line_height() };

    return CSS::ComputationContext {
        .length_resolution_context = {
            .viewport_rect = { 0, 0, 0, 0 },
            .font_metrics = font_metrics,
            .root_font_metrics = font_metrics },

        // NB: We don't require an abstract element because tree counting and random() functions aren't allowed in
        //     offscreen canvas context values
        .abstract_element = {},

        // NB: We don't require a color scheme since this is only used for resolving font values, not colors
        .color_scheme = {}
    };
}

void OffscreenCanvas::set_placeholder_link(Compositor::PlaceholderCanvasLink link)
{
    m_placeholder_link = link;
}

void OffscreenCanvas::did_change_canvas_content()
{
    if (!m_placeholder_link.has_value() || m_placeholder_commit_is_pending)
        return;

    // FIXME: Commit frames of OffscreenCanvases in shared and service workers.
    auto* window = as_if<Window>(*m_global_object);
    auto* dedicated_worker = as_if<DedicatedWorkerGlobalScope>(*m_global_object);
    if (!window && !dedicated_worker)
        return;

    m_placeholder_commit_is_pending = true;
    auto& page = canvas_page();
    page.enqueue_offscreen_canvas_placeholder_commit({}, *this);
    if (window)
        page.client().request_frame();
    else
        dedicated_worker->schedule_rendering_update();
}

void OffscreenCanvas::commit_to_placeholder()
{
    m_placeholder_commit_is_pending = false;
    auto& page = canvas_page();
    if (!m_placeholder_link.has_value() || !page.has_compositor_host())
        return;

    Optional<Compositing::CanvasId> source_canvas_id;
    m_context.visit(
        [&](GC::Ref<OffscreenCanvasRenderingContext2D>& context) {
            context->ensure_backing_storage();
            context->prepare_for_compositing();
            page.compositor_host().flush_canvas_2d_stream();
            source_canvas_id = context->canvas_id();
        },
        [&](OneOf<GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>> auto& context) {
            if (context->is_context_lost())
                return;
            context->prepare_for_compositing();
            source_canvas_id = context->context().canvas_id();
        },
        [](Empty) {
            // Do nothing.
        });

    auto clamp_dimension = [](WebIDL::UnsignedLongLong dimension) {
        return static_cast<int>(min(dimension, static_cast<WebIDL::UnsignedLongLong>(NumericLimits<int>::max())));
    };
    page.compositor_host().commit_placeholder_canvas(*m_placeholder_link, source_canvas_id, { clamp_dimension(m_width), clamp_dimension(m_height) }, is_origin_clean());
}

void OffscreenCanvas::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    visitor.visit(m_global_object);
}

OffscreenCanvas::HasOrCreatedContext OffscreenCanvas::create_2d_context(CanvasRenderingContext2DSettings context_attributes)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<OffscreenCanvasRenderingContext2D>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    m_context = OffscreenCanvasRenderingContext2D::create(*this, context_attributes);
    return HasOrCreatedContext::Yes;
}

template<typename ContextType>
JS::ThrowCompletionOr<OffscreenCanvas::HasOrCreatedContext> OffscreenCanvas::create_webgl_context(JS::Value options)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<ContextType>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    auto& realm = relevant_global_object().shape().realm();
    auto maybe_context = TRY(ContextType::create(realm, WebGL::CanvasOwner { GC::Ref { *this } }, options));
    if (!maybe_context)
        return HasOrCreatedContext::No;

    m_context = GC::Ref<ContextType>(*maybe_context);
    return HasOrCreatedContext::Yes;
}

}

namespace Web::Bindings {

WebIDL::ExceptionOr<GC::Ref<HTML::OffscreenCanvas>> construct_offscreen_canvas(JS::Realm& realm, WebIDL::UnsignedLongLong width, WebIDL::UnsignedLongLong height)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    return HTML::OffscreenCanvas::create(global_scope->this_impl(), width, height);
}

GC::Ref<WebIDL::Promise> convert_to_blob(JS::Realm& realm, HTML::OffscreenCanvas& offscreen_canvas, Optional<ImageEncodeOptions> const& options)
{
    // 1. If the value of this's [[Detached]] internal slot is true, then return a promise rejected with an "InvalidStateError" DOMException.
    if (offscreen_canvas.is_detached())
        return WebIDL::create_rejected_promise_for(offscreen_canvas.relevant_global_object(), WebIDL::InvalidStateError::create("OffscreenCanvas is detached"_utf16));

    // 2. If this's context mode is 2d and the rendering context's output bitmap's origin-clean flag is set to false, then return a promise rejected with a "SecurityError" DOMException.
    if (!offscreen_canvas.is_origin_clean())
        return WebIDL::create_rejected_promise_for(offscreen_canvas.relevant_global_object(), WebIDL::SecurityError::create("OffscreenCanvas is not origin-clean"_utf16));

    // 3. If this's bitmap has no pixels (i.e., either its horizontal dimension or its vertical dimension is zero), then return a promise rejected with an "IndexSizeError" DOMException.
    if (offscreen_canvas.width() == 0 || offscreen_canvas.height() == 0) {
        auto error = WebIDL::IndexSizeError::create("OffscreenCanvas has invalid dimensions. The bitmap has no pixels"_utf16);

        return WebIDL::create_rejected_promise_for(offscreen_canvas.relevant_global_object(), error);
    }

    // 4. Let bitmap be a copy of this's bitmap.
    auto bitmap = offscreen_canvas.get_bitmap_from_surface();

    // 5. Let result be a new promise object.
    auto result_promise = WebIDL::create_promise_for(offscreen_canvas.relevant_global_object());

    // 6. Let global be this's relevant global object.
    auto& global = offscreen_canvas.relevant_global_object();

    auto image_encode_options = options.value_or({});

    // 7. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [realm = GC::Ref(realm), &global, result_promise, bitmap, image_encode_options] {
        // 1. Let file be a serialization of bitmap as a file, with options's type and quality if present.
        Optional<HTML::SerializeBitmapResult> file_result {};

        if (bitmap) {
            if (auto result = HTML::serialize_bitmap(*bitmap, image_encode_options.type, image_encode_options.quality); !result.is_error())
                file_result = result.release_value();
        }

        // 2. Queue a global task on the canvas blob serialization task source given global to run these steps:
        HTML::queue_global_task(HTML::Task::Source::CanvasBlobSerializationTask, global, GC::create_function(GC::Heap::the(), [realm, result_promise, file_result = move(file_result)] -> void {
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            // 1. If file is null, then reject result with an "EncodingError" DOMException.
            if (!file_result.has_value()) {
                auto error = WebIDL::EncodingError::create("Failed to convert OffscreenCanvas to Blob"_utf16);
                WebIDL::reject_promise(result_promise, error);
            }
            // 2. Otherwise, resolve result with a new Blob object, created in global's relevant realm, representing file. [FILEAPI]
            else {
                auto blob = FileAPI::Blob::create(file_result->buffer, Utf16String::from_utf8(serialized_bitmap_mime_type_to_byte_string(file_result->mime_type)));
                WebIDL::resolve_promise(result_promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, blob));
            }
        }));
    }));

    // 8. Return result.
    return result_promise;
}

JS::ThrowCompletionOr<HTML::OffscreenRenderingContext> get_context(JS::Realm& realm, HTML::OffscreenCanvas& offscreen_canvas, OffscreenRenderingContextId context_id, JS::Value options)
{
    // 1. If options is not an object, then set options to null.
    if (!options.is_object())
        options = JS::js_null();

    // 2. Set options to the result of converting options to a JavaScript value.
    // NOTE: No-op.

    // 3. Run the steps in the cell of the following table whose column header
    // matches this OffscreenCanvas object's context mode and whose row header
    // matches contextId:
    // NOTE: See the spec for the full table.
    if (offscreen_canvas.is_detached()) {
        return WebIDL::throw_dom_exception_if_needed(realm.vm(), realm, [] -> WebIDL::ExceptionOr<HTML::OffscreenRenderingContext> {
            return WebIDL::InvalidStateError::create("OffscreenCanvas is detached"_utf16);
        });
    }

    if (context_id == OffscreenRenderingContextId::_2d) {
        auto context_attributes = TRY(convert_to_idl_value_for_canvas_rendering_context2d_settings(offscreen_canvas.vm(), options));
        if (offscreen_canvas.create_2d_context(context_attributes) == HTML::OffscreenCanvas::HasOrCreatedContext::Yes)
            return offscreen_canvas.context().get<GC::Ref<HTML::OffscreenCanvasRenderingContext2D>>();

        return Empty {};
    }

    if (context_id == OffscreenRenderingContextId::Webgl) {
        if (TRY(offscreen_canvas.create_webgl_context<WebGL::WebGLRenderingContext>(options)) == HTML::OffscreenCanvas::HasOrCreatedContext::Yes)
            return offscreen_canvas.context().get<GC::Ref<WebGL::WebGLRenderingContext>>();

        return Empty {};
    }

    if (context_id == OffscreenRenderingContextId::Webgl2) {
        if (TRY(offscreen_canvas.create_webgl_context<WebGL::WebGL2RenderingContext>(options)) == HTML::OffscreenCanvas::HasOrCreatedContext::Yes)
            return offscreen_canvas.context().get<GC::Ref<WebGL::WebGL2RenderingContext>>();

        return Empty {};
    }

    return Empty {};
}

}
