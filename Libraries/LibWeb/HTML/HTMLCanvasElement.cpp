/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Checked.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/SharedImage.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLCanvasElement.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Canvas/SerializeBitmap.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/Layout/CanvasBox.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLCanvasElement);

static RefPtr<Gfx::Bitmap> create_transparent_canvas_bitmap(Gfx::IntSize const& size)
{
    auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size);
    if (bitmap_or_error.is_error())
        return nullptr;
    return bitmap_or_error.release_value();
}

HTMLCanvasElement::HTMLCanvasElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLCanvasElement::~HTMLCanvasElement() = default;

void HTMLCanvasElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLCanvasElement);
    Base::initialize(realm);
    document().page().register_canvas_element({}, unique_id());
}

void HTMLCanvasElement::finalize()
{
    // The remote canvas context belongs to the 2D context; tear it down with the
    // element, since nothing will reach the context afterwards.
    if (auto context = canvas_rendering_context_2d())
        context->discard_backing_storage();
    Base::finalize();
    document().page().unregister_canvas_element({}, unique_id());
}

void HTMLCanvasElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

bool HTMLCanvasElement::is_presentational_hint(FlyString const& name) const
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
    auto w = parse_non_negative_integer(get_attribute_value(HTML::AttributeNames::width));
    auto h = parse_non_negative_integer(get_attribute_value(HTML::AttributeNames::height));

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
}

void HTMLCanvasElement::set_width(unsigned value)
{
    if (value > 2147483647)
        value = 300;

    set_attribute_value(HTML::AttributeNames::width, String::number(value));
    notify_context_about_canvas_size_change();
    reset_context_to_default_state();
}

void HTMLCanvasElement::set_height(WebIDL::UnsignedLong value)
{
    if (value > 2147483647)
        value = 150;

    set_attribute_value(HTML::AttributeNames::height, String::number(value));
    notify_context_about_canvas_size_change();
    reset_context_to_default_state();
}

void HTMLCanvasElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);

    if (local_name.is_one_of(HTML::AttributeNames::width, HTML::AttributeNames::height)) {
        notify_context_about_canvas_size_change();
        reset_context_to_default_state();
        set_needs_layout_update(DOM::SetNeedsLayoutReason::HTMLCanvasElementWidthOrHeightChange);
    }
}

RefPtr<Layout::Node> HTMLCanvasElement::create_layout_node(CSS::ComputedProperties const& style)
{
    return make_ref_counted<Layout::CanvasBox>(document(), *this, style);
}

void HTMLCanvasElement::adjust_computed_style(CSS::ComputedProperties::Builder& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

JS::ThrowCompletionOr<HTMLCanvasElement::HasOrCreatedContext> HTMLCanvasElement::create_2d_context(JS::Value options)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<CanvasRenderingContext2D>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    m_context = TRY(CanvasRenderingContext2D::create(realm(), *this, options));
    return HasOrCreatedContext::Yes;
}

template<typename ContextType>
JS::ThrowCompletionOr<HTMLCanvasElement::HasOrCreatedContext> HTMLCanvasElement::create_webgl_context(JS::Value options)
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<ContextType>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    auto maybe_context = TRY(ContextType::create(realm(), *this, options));
    if (!maybe_context)
        return HasOrCreatedContext::No;

    m_context = GC::Ref<ContextType>(*maybe_context);
    return HasOrCreatedContext::Yes;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-getcontext
JS::ThrowCompletionOr<HTMLCanvasElement::RenderingContext> HTMLCanvasElement::get_context(String const& type, JS::Value options)
{
    // 1. If options is not an object, then set options to null.
    if (!options.is_object())
        options = JS::js_null();

    // 2. Set options to the result of converting options to a JavaScript value.
    // NOTE: No-op.

    // 3. Run the steps in the cell of the following table whose column header matches this canvas element's canvas context mode and whose row header matches contextId:
    // NOTE: See the spec for the full table.
    if (type == "2d"sv) {
        if (TRY(create_2d_context(options)) == HasOrCreatedContext::Yes)
            return m_context.get<GC::Ref<HTML::CanvasRenderingContext2D>>();

        return Empty {};
    }

    // NOTE: The WebGL spec says "experimental-webgl" is also acceptable and must be equivalent to "webgl". Other engines accept this, so we do too.
    if (type.is_one_of("webgl"sv, "experimental-webgl"sv)) {
        if (TRY(create_webgl_context<WebGL::WebGLRenderingContext>(options)) == HasOrCreatedContext::Yes)
            return m_context.get<GC::Ref<WebGL::WebGLRenderingContext>>();

        return Empty {};
    }

    if (type == "webgl2"sv) {
        if (TRY(create_webgl_context<WebGL::WebGL2RenderingContext>(options)) == HasOrCreatedContext::Yes)
            return m_context.get<GC::Ref<WebGL::WebGL2RenderingContext>>();

        return Empty {};
    }

    return Empty {};
}

Gfx::IntSize HTMLCanvasElement::bitmap_size_for_canvas(size_t minimum_width, size_t minimum_height) const
{
    auto width = max(this->width(), minimum_width);
    auto height = max(this->height(), minimum_height);

    Checked<size_t> area = width;
    area *= height;

    if (area.has_overflow()) {
        dbgln("Refusing to create {}x{} canvas (overflow)", width, height);
        return {};
    }
    if (area.value() > Gfx::max_canvas_area) {
        dbgln("Refusing to create {}x{} canvas (exceeds maximum size)", width, height);
        return {};
    }
    return Gfx::IntSize(width, height);
}

// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
bool HTMLCanvasElement::is_origin_clean() const
{
    return m_context.visit(
        [](GC::Ref<CanvasRenderingContext2D> const& context) { return context->origin_clean(); },
        // FIXME: WebGL and WebGL2 contexts do not track the origin-clean flag yet.
        [](auto const&) { return true; });
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-todataurl
WebIDL::ExceptionOr<String> HTMLCanvasElement::to_data_url(StringView type, Optional<JS::Value> js_quality)
{
    // 1. If this canvas element's bitmap's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!is_origin_clean())
        return WebIDL::SecurityError::create(realm(), "Canvas is not origin-clean"_utf16);

    // 2. If this canvas element's bitmap has no pixels (i.e. either its horizontal dimension or its vertical dimension is zero),
    //    then return the string "data:,". (This is the shortest data: URL; it represents the empty string in a text/plain resource.)
    auto bitmap = get_bitmap_from_surface();
    if (!bitmap)
        return "data:,"_string;

    // 3. Let file be a serialization of this canvas element's bitmap as a file, passing type and quality if given.
    Optional<double> quality = js_quality.has_value() && js_quality->is_number() ? js_quality->as_double() : Optional<double>();
    auto file = serialize_bitmap(*bitmap, type, quality);

    // 4. If file is null, then return "data:,".
    if (file.is_error()) {
        dbgln("HTMLCanvasElement: Failed to encode canvas bitmap to {}: {}", type, file.error());
        return "data:,"_string;
    }

    // 5. Return a data: URL representing file. [RFC2397]
    auto base64_encoded_or_error = encode_base64(file.value().buffer);
    if (base64_encoded_or_error.is_error()) {
        return "data:,"_string;
    }
    return URL::create_with_data(file.value().mime_type, base64_encoded_or_error.release_value(), true).to_string();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-toblob
WebIDL::ExceptionOr<void> HTMLCanvasElement::to_blob(GC::Ref<WebIDL::CallbackType> callback, StringView type, Optional<JS::Value> js_quality)
{
    // 1. If this canvas element's bitmap's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!is_origin_clean())
        return WebIDL::SecurityError::create(realm(), "Canvas is not origin-clean"_utf16);

    // 2. Let result be null.
    // 3. If this canvas element's bitmap has pixels (i.e., neither its horizontal dimension nor its vertical dimension is zero),
    //    then set result to a copy of this canvas element's bitmap.
    auto bitmap_result = get_bitmap_from_surface();

    Optional<double> quality = js_quality.has_value() && js_quality->is_number() ? js_quality->as_double() : Optional<double>();

    // 4. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, callback, bitmap_result, type, quality] {
        // 1. If result is non-null, then set result to a serialization of result as a file with type and quality if given.
        Optional<SerializeBitmapResult> file_result;
        if (bitmap_result) {
            if (auto result = serialize_bitmap(*bitmap_result, type, quality); !result.is_error())
                file_result = result.release_value();
        }

        // 2. Queue an element task on the canvas blob serialization task source given the canvas element to run these steps:
        queue_an_element_task(Task::Source::CanvasBlobSerializationTask, [this, callback, file_result = move(file_result)] {
            auto maybe_error = Bindings::throw_dom_exception_if_needed(vm(), [&]() -> WebIDL::ExceptionOr<void> {
                // 1. If result is non-null, then set result to a new Blob object, created in the relevant realm of this canvas element, representing result. [FILEAPI]
                GC::Ptr<FileAPI::Blob> blob_result;
                if (file_result.has_value())
                    blob_result = FileAPI::Blob::create(realm(), file_result->buffer, TRY_OR_THROW_OOM(vm(), String::from_utf8(file_result->mime_type)));

                // 2. Invoke callback with « result » and "report".
                TRY(WebIDL::invoke_callback(*callback, {}, WebIDL::ExceptionBehavior::Report, { { blob_result } }));
                return {};
            });
            if (maybe_error.is_throw_completion())
                report_exception(maybe_error.throw_completion(), realm());
        });
    }));
    return {};
}

WebGL::WebGLRenderingContextBase* HTMLCanvasElement::webgl_context() const
{
    return m_context.visit(
        [](GC::Ref<WebGL::WebGLRenderingContext> const& context) -> WebGL::WebGLRenderingContextBase* { return context.ptr(); },
        [](GC::Ref<WebGL::WebGL2RenderingContext> const& context) -> WebGL::WebGLRenderingContextBase* { return context.ptr(); },
        [](auto const&) -> WebGL::WebGLRenderingContextBase* { return nullptr; });
}

Optional<Painting::CanvasId> HTMLCanvasElement::canvas_id() const
{
    if (auto context = canvas_rendering_context_2d())
        return context->canvas_id();
    if (auto* webgl_context = this->webgl_context(); webgl_context && !webgl_context->is_context_lost())
        return webgl_context->context().canvas_id();
    return {};
}

RefPtr<Gfx::Bitmap> HTMLCanvasElement::get_bitmap_from_surface()
{
    auto const size = bitmap_size_for_canvas();
    if (size.is_empty())
        return nullptr;

    RefPtr<Gfx::Bitmap> bitmap;
    if (auto* webgl_context = this->webgl_context()) {
        bitmap = webgl_context->context().read_back_drawing_buffer({ {}, size });
    } else {
        if (auto context = canvas_rendering_context_2d()) {
            ensure_backing_storage();
            if (auto pixels = context->read_pixels({ {}, size }); pixels && pixels->size() == size)
                bitmap = pixels;
        } else {
            bitmap = create_transparent_canvas_bitmap(size);
        }
    }

    return bitmap;
}

void HTMLCanvasElement::notify_compositor_connection_lost()
{
    if (auto* webgl_context = this->webgl_context())
        webgl_context->lose_context_from_compositor_loss();
}

void HTMLCanvasElement::set_canvas_content_dirty()
{
    m_canvas_content_dirty = true;
}

void HTMLCanvasElement::prepare_for_compositing()
{
    if (!m_canvas_content_dirty)
        return;
    m_canvas_content_dirty = false;

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

void HTMLCanvasElement::notify_compositor_backing_storage_lost()
{
    if (auto* webgl_context = this->webgl_context()) {
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

}
