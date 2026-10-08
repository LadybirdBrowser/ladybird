/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/SharedImage.h>
#include <LibWeb/Bindings/CanvasRenderingContext2DSettings.h>
#include <LibWeb/Bindings/WebGLRenderingContextBase.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLCanvasElement);

static HashMap<Compositing::CanvasId, UniqueNodeID>& placeholder_canvas_elements()
{
    static NeverDestroyed<HashMap<Compositing::CanvasId, UniqueNodeID>> elements;
    return *elements;
}

HTMLCanvasElement::HTMLCanvasElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLCanvasElement::~HTMLCanvasElement() = default;

void HTMLCanvasElement::initialize_element()
{
    document().page().register_canvas_element({}, unique_id());
}

void HTMLCanvasElement::finalize()
{
    // The remote canvas context belongs to the 2D context; tear it down with the
    // element, since nothing will reach the context afterwards.
    if (auto context = canvas_rendering_context_2d())
        context->discard_backing_storage();
    if (m_placeholder_canvas_id.has_value()) {
        placeholder_canvas_elements().remove(*m_placeholder_canvas_id);
        if (document().page().has_compositor_host())
            document().page().compositor_host().release_placeholder_canvas(*m_placeholder_canvas_id);
    }
    Base::finalize();
    document().page().unregister_canvas_element({}, unique_id());
}

void HTMLCanvasElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

bool HTMLCanvasElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::width,
        HTML::AttributeNames::height);
}

void HTMLCanvasElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);
    // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images
    // The width and height attributes map to the aspect-ratio property on canvas elements.

    // FIXME: Multiple elements have aspect-ratio presentational hints, make this into a helper function

    // https://html.spec.whatwg.org/multipage/rendering.html#map-to-the-aspect-ratio-property
    // if element has both attributes w and h, and parsing those attributes' values using the rules for parsing non-negative integers doesn't generate an error for either
    auto w = parse_non_negative_integer(attribute(HTML::AttributeNames::width).value_or({}));
    auto h = parse_non_negative_integer(attribute(HTML::AttributeNames::height).value_or({}));

    // then the user agent is expected to use the parsed integers as a presentational hint for the 'aspect-ratio' property of the form auto w / h.
    if (w.has_value() && h.has_value()) {
        auto aspect_ratio = CSS::StyleValueList::create(
            CSS::StyleValueVector {
                CSS::KeywordStyleValue::create(CSS::Keyword::Auto),
                CSS::RatioStyleValue::create(CSS::NumberStyleValue::create(w.value()), CSS::NumberStyleValue::create(h.value())),
            },
            CSS::StyleValueList::Separator::Space);
        properties.append({ .property_id = CSS::PropertyID::AspectRatio, .value = aspect_ratio });
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-width
WebIDL::UnsignedLong HTMLCanvasElement::width() const
{
    // The width and height IDL attributes must reflect the respective content attributes of the same name, with the same defaults.
    // https://html.spec.whatwg.org/multipage/canvas.html#obtain-numeric-values
    // The rules for parsing non-negative integers must be used to obtain their numeric values.
    // If an attribute is missing, or if parsing its value returns an error, then the default value must be used instead.
    // The width attribute defaults to 300
    if (auto width_string = get_attribute(HTML::AttributeNames::width); width_string.has_value()) {
        if (auto width = parse_non_negative_integer(*width_string); width.has_value() && *width <= 2147483647)
            return *width;
    }

    return 300;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-height
WebIDL::UnsignedLong HTMLCanvasElement::height() const
{
    // The width and height IDL attributes must reflect the respective content attributes of the same name, with the same defaults.
    // https://html.spec.whatwg.org/multipage/canvas.html#obtain-numeric-values
    // The rules for parsing non-negative integers must be used to obtain their numeric values.
    // If an attribute is missing, or if parsing its value returns an error, then the default value must be used instead.
    // the height attribute defaults to 150
    if (auto height_string = get_attribute(HTML::AttributeNames::height); height_string.has_value()) {
        if (auto height = parse_non_negative_integer(*height_string); height.has_value() && *height <= 2147483647)
            return *height;
    }

    return 150;
}

void HTMLCanvasElement::reset_context_to_default_state()
{
    m_context.visit(
        [](GC::Ref<CanvasRenderingContext2D>& context) {
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

Canvas2DContextBase* HTMLCanvasElement::canvas_2d_context() const
{
    return canvas_rendering_context_2d().ptr();
}

WebGL::WebGLRenderingContextBase* HTMLCanvasElement::canvas_webgl_context() const
{
    return m_context.visit(
        [](GC::Ref<WebGL::WebGLRenderingContext> const& context) -> WebGL::WebGLRenderingContextBase* { return context.ptr(); },
        [](GC::Ref<WebGL::WebGL2RenderingContext> const& context) -> WebGL::WebGLRenderingContextBase* { return context.ptr(); },
        [](auto const&) -> WebGL::WebGLRenderingContextBase* { return nullptr; });
}

Page& HTMLCanvasElement::canvas_page()
{
    return document().page();
}

JS::Object& HTMLCanvasElement::canvas_relevant_global_object() const
{
    return HTML::relevant_global_object(*this);
}

GC::Ptr<Bindings::Wrappable> HTMLCanvasElement::canvas_relevant_global_impl() const
{
    return document().window();
}

void HTMLCanvasElement::did_change_canvas_content()
{
    set_canvas_content_dirty();

    // NB: Don't request a display list recording here: the new content reaches the compositor through the canvas
    // surface registry when the canvas is presented, and the cached DrawCanvas command is invalidated when the
    // content generation moves in prepare_for_compositing.
    set_needs_repaint(InvalidateDisplayList::No);
}

void HTMLCanvasElement::did_create_canvas_backing_storage()
{
    set_needs_repaint(InvalidateDisplayList::PaintCommands);
}

// https://drafts.csswg.org/css-font-loading/#font-source
CSS::FontComputer& HTMLCanvasElement::canvas_font_computer()
{
    // 1. If object's font style source object is a canvas element, return the element's node document.
    return document().font_computer();
}

CSS::ColorResolutionStyle HTMLCanvasElement::canvas_color_resolution_style()
{
    document().update_style_for_element(*this, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
    return CSS::ColorResolutionStyle::for_element(*this);
}

CSSPixelRect HTMLCanvasElement::canvas_viewport_rect() const
{
    if (auto navigable = this->navigable())
        return navigable->viewport_rect();
    return {};
}

CSS::ComputationContext HTMLCanvasElement::canvas_font_computation_context()
{
    DOM::AbstractElement abstract_element { *this };
    Optional<CSS::Length::ResolutionContext> length_resolution_context;

    if (is_connected() && this->navigable()) {
        length_resolution_context = CSS::Length::ResolutionContext::for_element(abstract_element);
    } else {
        // NB: This is similar to the document's LRC but using the default canvas context font size of 10px
        CSS::Length::FontMetrics font_metrics { 10, Platform::FontPlugin::the().default_font(8)->pixel_metrics(), CSS::InitialValues::line_height() };

        CSSPixelRect viewport_rect;
        if (auto navigable = this->navigable())
            viewport_rect = navigable->viewport_rect();

        length_resolution_context = {
            .viewport_rect = viewport_rect,
            .font_metrics = font_metrics,
            .root_font_metrics = font_metrics
        };
    }

    return CSS::ComputationContext {
        .length_resolution_context = length_resolution_context.value(),

        // NB: We require a abstract element here since tree counting functions are allowed in font values unlike for
        //     OffscreenCanvas
        .abstract_element = abstract_element,

        // NB: We don't require a color scheme since this is only used for resolving font values, not colors
        .color_scheme = {}
    };
}

void HTMLCanvasElement::notify_context_about_canvas_size_change()
{
    m_context.visit(
        [&](GC::Ref<CanvasRenderingContext2D>& context) {
            context->set_size(bitmap_size_for_canvas());
        },
        [&](GC::Ref<WebGL::WebGLRenderingContext>& context) {
            context->set_size(bitmap_size_for_canvas());
        },
        [&](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
            context->set_size(bitmap_size_for_canvas());
        },
        [](Empty) {
            // Do nothing.
        });
    Painting::push_canvas_paint_facts(*this);
}

WebIDL::ExceptionOr<void> HTMLCanvasElement::set_width(unsigned value)
{
    if (m_is_placeholder)
        return WebIDL::InvalidStateError::create("Cannot resize a placeholder canvas"_utf16);

    if (value > 2147483647)
        value = 300;

    set_attribute_value(HTML::AttributeNames::width, Utf16String::number(value));
    notify_context_about_canvas_size_change();
    reset_context_to_default_state();
    return {};
}

WebIDL::ExceptionOr<void> HTMLCanvasElement::set_height(WebIDL::UnsignedLong value)
{
    if (m_is_placeholder)
        return WebIDL::InvalidStateError::create("Cannot resize a placeholder canvas"_utf16);

    if (value > 2147483647)
        value = 150;

    set_attribute_value(HTML::AttributeNames::height, Utf16String::number(value));
    notify_context_about_canvas_size_change();
    reset_context_to_default_state();
    return {};
}

void HTMLCanvasElement::attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);

    if (local_name.is_one_of(HTML::AttributeNames::width, HTML::AttributeNames::height)) {
        notify_context_about_canvas_size_change();
        reset_context_to_default_state();
        set_needs_layout_update(DOM::SetNeedsLayoutReason::HTMLCanvasElementWidthOrHeightChange);
    }
}

CSS::ElementBoxKind HTMLCanvasElement::box_kind() const
{
    return CSS::ElementBoxKind::Canvas;
}

HTMLCanvasElement::HasOrCreatedContext HTMLCanvasElement::create_2d_context(CanvasRenderingContext2DSettings context_attributes)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<CanvasRenderingContext2D>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    m_context = CanvasRenderingContext2D::create(*this, context_attributes);
    return HasOrCreatedContext::Yes;
}

template<typename ContextType>
JS::ThrowCompletionOr<HTMLCanvasElement::HasOrCreatedContext> HTMLCanvasElement::create_webgl_context(JS::Value options)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<ContextType>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    auto maybe_context = TRY(ContextType::create(HTML::relevant_realm(*this), WebGL::CanvasOwner { GC::Ref { *this } }, options));
    if (!maybe_context)
        return HasOrCreatedContext::No;

    m_context = GC::Ref<ContextType>(*maybe_context);
    return HasOrCreatedContext::Yes;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-getcontext
JS::ThrowCompletionOr<HTMLCanvasElement::RenderingContext> HTMLCanvasElement::get_context(Utf16View type, JS::Value options)
{
    if (!options.is_object())
        options = JS::js_null();

    if (m_is_placeholder) {
        return WebIDL::throw_dom_exception_if_needed(vm(), HTML::relevant_realm(*this), [] -> WebIDL::ExceptionOr<RenderingContext> {
            return WebIDL::InvalidStateError::create("Canvas has transferred control to an OffscreenCanvas"_utf16);
        });
    }

    if (type == u"2d"sv) {
        auto context_attributes = TRY(Bindings::convert_to_idl_value_for_canvas_rendering_context2d_settings(vm(), options));
        if (create_2d_context(context_attributes) == HasOrCreatedContext::Yes)
            return m_context.get<GC::Ref<HTML::CanvasRenderingContext2D>>();

        return Empty {};
    }

    // NOTE: The WebGL spec says "experimental-webgl" is also acceptable and must be equivalent to "webgl". Other engines accept this, so we do too.
    if (type.is_one_of(u"webgl"sv, u"experimental-webgl"sv)) {
        if (TRY(create_webgl_context<WebGL::WebGLRenderingContext>(options)) == HasOrCreatedContext::Yes)
            return m_context.get<GC::Ref<WebGL::WebGLRenderingContext>>();

        return Empty {};
    }

    if (type == u"webgl2"sv) {
        if (TRY(create_webgl_context<WebGL::WebGL2RenderingContext>(options)) == HasOrCreatedContext::Yes)
            return m_context.get<GC::Ref<WebGL::WebGL2RenderingContext>>();

        return Empty {};
    }

    return Empty {};
}

Gfx::IntSize HTMLCanvasElement::bitmap_size_for_canvas() const
{
    return bitmap_size_for_dimensions(width(), height());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-todataurl
WebIDL::ExceptionOr<Utf16String> HTMLCanvasElement::to_data_url(Utf16View type, Optional<JS::Value> js_quality)
{
    // 1. If this canvas element's bitmap's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!is_origin_clean())
        return WebIDL::SecurityError::create(HTML::relevant_realm(*this), "Canvas is not origin-clean"_utf16);

    // 2. If this canvas element's bitmap has no pixels (i.e. either its horizontal dimension or its vertical dimension is zero),
    //    then return the string "data:,". (This is the shortest data: URL; it represents the empty string in a text/plain resource.)
    auto bitmap = get_bitmap_from_surface();
    if (!is_origin_clean())
        return WebIDL::SecurityError::create(HTML::relevant_realm(*this), "Canvas is not origin-clean"_utf16);
    if (!bitmap)
        return "data:,"_utf16;

    // 3. Let file be a serialization of this canvas element's bitmap as a file, passing type and quality if given.
    Optional<double> quality = js_quality.has_value() && js_quality->is_number() ? js_quality->as_double() : Optional<double>();
    auto file = serialize_bitmap(*bitmap, type, quality);

    // 4. If file is null, then return "data:,".
    if (file.is_error()) {
        dbgln("HTMLCanvasElement: Failed to encode canvas bitmap to {}: {}", type, file.error());
        return "data:,"_utf16;
    }

    // 5. Return a data: URL representing file. [RFC2397]
    auto base64_encoded_or_error = encode_base64(file.value().buffer);
    if (base64_encoded_or_error.is_error()) {
        return "data:,"_utf16;
    }
    auto mime_type = serialized_bitmap_mime_type_to_byte_string(file.value().mime_type);
    return utf16_string_from_url_ascii(URL::create_with_data(mime_type, base64_encoded_or_error.release_value(), true).to_string());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-toblob
WebIDL::ExceptionOr<void> HTMLCanvasElement::to_blob(GC::Ref<WebIDL::CallbackType> callback, Utf16View type, Optional<JS::Value> js_quality)
{
    // 1. If this canvas element's bitmap's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!is_origin_clean())
        return WebIDL::SecurityError::create(HTML::relevant_realm(*this), "Canvas is not origin-clean"_utf16);

    // 2. Let result be null.
    // 3. If this canvas element's bitmap has pixels (i.e., neither its horizontal dimension nor its vertical dimension is zero),
    //    then set result to a copy of this canvas element's bitmap.
    auto bitmap_result = get_bitmap_from_surface();
    if (!is_origin_clean())
        return WebIDL::SecurityError::create(HTML::relevant_realm(*this), "Canvas is not origin-clean"_utf16);

    // 4. Run these steps in parallel:
    auto type_string = Utf16String::from_utf16(type);
    Optional<double> quality = js_quality.has_value() && js_quality->is_number() ? js_quality->as_double() : Optional<double>();
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, callback, bitmap_result, type = move(type_string), quality] {
        // 1. If result is non-null, then set result to a serialization of result as a file with type and quality if given.
        Optional<SerializeBitmapResult> file_result;
        if (bitmap_result) {
            if (auto result = serialize_bitmap(*bitmap_result, type, quality); !result.is_error())
                file_result = result.release_value();
        }

        // 2. Queue an element task on the canvas blob serialization task source given the canvas element to run these steps:
        queue_an_element_task(Task::Source::CanvasBlobSerializationTask, [this, callback, file_result = move(file_result)] {
            auto& realm = HTML::relevant_realm(*this);
            auto maybe_error = WebIDL::throw_dom_exception_if_needed(vm(), realm, [&]() -> WebIDL::ExceptionOr<void> {
                // 1. If result is non-null, then set result to a new Blob object, created in the relevant realm of this canvas element, representing result. [FILEAPI]
                GC::Ptr<FileAPI::Blob> blob_result;
                if (file_result.has_value())
                    blob_result = FileAPI::Blob::create(file_result->buffer, Utf16String::from_utf16(serialized_bitmap_mime_type_to_utf16_view(file_result->mime_type)));

                // 2. Invoke callback with « result » and "report".
                auto callback_argument = blob_result ? JS::Value { Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { *blob_result }) } : JS::js_null();
                TRY(WebIDL::invoke_callback(*callback, {}, WebIDL::ExceptionBehavior::Report, { { callback_argument } }));
                return {};
            });
            if (maybe_error.is_throw_completion())
                report_exception(maybe_error.throw_completion(), HTML::relevant_realm(*this));
        });
    }));
    return {};
}

Optional<Compositing::CanvasId> HTMLCanvasElement::canvas_id() const
{
    if (m_is_placeholder)
        return m_placeholder_canvas_id;
    if (auto context = canvas_rendering_context_2d())
        return context->canvas_id();
    if (auto* webgl_context = canvas_webgl_context(); webgl_context && !webgl_context->is_context_lost())
        return webgl_context->context().canvas_id();
    return {};
}

void HTMLCanvasElement::notify_compositor_connection_lost()
{
    if (auto* webgl_context = canvas_webgl_context())
        webgl_context->lose_context_from_compositor_loss();
}

void HTMLCanvasElement::set_canvas_content_dirty()
{
    m_canvas_content_dirty = true;
}

void HTMLCanvasElement::prepare_for_compositing()
{
    if (m_canvas_content_dirty) {
        m_canvas_content_dirty = false;

        // NB: The content generation is recorded into DrawCanvas display list commands, letting display list damage
        //     computation see that the canvas content changed. Canvases are prepared for compositing before painting
        //     in the rendering update, so display lists recorded in the same update pick up the new generation.
        ++m_content_generation;

        m_context.visit(
            [](GC::Ref<CanvasRenderingContext2D>& context) {
                context->prepare_for_compositing();
            },
            [](GC::Ref<WebGL::WebGLRenderingContext>& context) {
                context->prepare_for_compositing();
            },
            [](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
                context->prepare_for_compositing();
            },
            [](Empty) {
                // Do nothing.
            });
    }
    Painting::push_canvas_paint_facts(*this);
}

void HTMLCanvasElement::notify_compositor_backing_storage_lost()
{
    if (auto* webgl_context = canvas_webgl_context()) {
        webgl_context->restore_context_after_compositor_reconnect();
        return;
    }
    if (auto context_2d = canvas_rendering_context_2d())
        context_2d->notify_backing_storage_lost();
}

Optional<Gfx::IntSize> HTMLCanvasElement::canvas_surface_content_size() const
{
    if (!canvas_id().has_value())
        return {};

    auto size = bitmap_size_for_canvas();
    if (size.is_empty())
        return {};
    return size;
}

void HTMLCanvasElement::ensure_backing_storage()
{
    if (auto context = canvas_rendering_context_2d())
        context->ensure_backing_storage();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-transfercontroltooffscreen
WebIDL::ExceptionOr<GC::Ref<OffscreenCanvas>> HTMLCanvasElement::transfer_control_to_offscreen()
{
    // 1. If this canvas element's context mode is not set to none, throw an "InvalidStateError" DOMException.
    if (m_is_placeholder || !m_context.has<Empty>())
        return WebIDL::InvalidStateError::create("Canvas already has a rendering context"_utf16);

    // 2. Let offscreenCanvas be a new OffscreenCanvas object with its width and height equal to the values of the
    //    width and height content attributes of this canvas element.
    auto offscreen_canvas = OffscreenCanvas::create(*document().window(), width(), height());

    // 3. Set the placeholder canvas element of offscreenCanvas to a weak reference to this canvas element.
    if (document().page().has_compositor_host()) {
        if (auto link = document().page().compositor_host().allocate_placeholder_canvas(); link.has_value()) {
            m_placeholder_canvas_id = link->canvas_id;
            placeholder_canvas_elements().set(link->canvas_id, unique_id());
            offscreen_canvas->set_placeholder_link(*link);
        }
    }

    // 4. Set this canvas element's context mode to placeholder.
    m_is_placeholder = true;

    // FIXME: 5. Set offscreenCanvas's inherited language and direction to the language and direction of this canvas element.

    // 6. Return offscreenCanvas.
    return offscreen_canvas;
}

void HTMLCanvasElement::placeholder_frame_committed(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool origin_clean)
{
    auto element_id = placeholder_canvas_elements().get(canvas_id);
    if (!element_id.has_value())
        return;
    if (auto* element = as_if<HTMLCanvasElement>(DOM::Node::from_unique_id(*element_id)))
        element->did_commit_placeholder_frame(size, origin_clean);
}

void HTMLCanvasElement::did_commit_placeholder_frame(Gfx::IntSize size, bool origin_clean)
{
    m_placeholder_frame_is_origin_clean = origin_clean;

    queue_an_element_task(Task::Source::DOMManipulation, [this, size] {
        if (width() != static_cast<WebIDL::UnsignedLong>(size.width()))
            set_attribute_value(HTML::AttributeNames::width, Utf16String::number(size.width()));
        if (height() != static_cast<WebIDL::UnsignedLong>(size.height()))
            set_attribute_value(HTML::AttributeNames::height, Utf16String::number(size.height()));
    });
}

RefPtr<Gfx::Bitmap> HTMLCanvasElement::get_bitmap_from_surface()
{
    if (!m_is_placeholder)
        return CanvasHost::get_bitmap_from_surface();

    auto size = bitmap_size_for_canvas();
    if (size.is_empty())
        return nullptr;

    if (m_placeholder_canvas_id.has_value() && document().page().has_compositor_host()) {
        auto frame = document().page().compositor_host().read_placeholder_canvas_pixels(*m_placeholder_canvas_id, { {}, size });
        m_placeholder_frame_is_origin_clean = frame.origin_clean;
        if (frame.bitmap && frame.bitmap->size() == size)
            return frame.bitmap;
    }
    return CanvasHost::get_bitmap_from_surface();
}

// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
bool HTMLCanvasElement::is_origin_clean() const
{
    if (m_is_placeholder)
        return m_placeholder_frame_is_origin_clean;
    return CanvasHost::is_origin_clean();
}

}
