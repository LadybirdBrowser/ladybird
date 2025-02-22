/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Checked.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLCanvasElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Layout/CanvasBox.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLCanvasElement);

static constexpr auto max_canvas_area = 16384 * 16384;

HTMLCanvasElement::HTMLCanvasElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLCanvasElement::~HTMLCanvasElement() = default;

void HTMLCanvasElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLCanvasElement);
}

void HTMLCanvasElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_context.visit(
        [&](GC::Ref<CanvasRenderingContext2D>& context) {
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

bool HTMLCanvasElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::width,
        HTML::AttributeNames::height);
}

void HTMLCanvasElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images
    // The width and height attributes map to the aspect-ratio property on canvas elements.

    // FIXME: Multiple elements have aspect-ratio presentational hints, make this into a helper function

    // https://html.spec.whatwg.org/multipage/rendering.html#map-to-the-aspect-ratio-property
    // if element has both attributes w and h, and parsing those attributes' values using the rules for parsing non-negative integers doesn't generate an error for either
    auto w = parse_non_negative_integer(get_attribute_value(HTML::AttributeNames::width));
    auto h = parse_non_negative_integer(get_attribute_value(HTML::AttributeNames::height));

    if (w.has_value() && h.has_value())
        // then the user agent is expected to use the parsed integers as a presentational hint for the 'aspect-ratio' property of the form auto w / h.
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::AspectRatio,
            CSS::StyleValueList::create(CSS::StyleValueVector {
                                            CSS::CSSKeywordValue::create(CSS::Keyword::Auto),
                                            CSS::RatioStyleValue::create(CSS::Ratio { static_cast<double>(w.value()), static_cast<double>(h.value()) }) },

                CSS::StyleValueList::Separator::Space));
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

WebIDL::ExceptionOr<void> HTMLCanvasElement::set_width(unsigned value)
{
    if (value > 2147483647)
        value = 300;

    TRY(set_attribute(HTML::AttributeNames::width, String::number(value)));
    notify_context_about_canvas_size_change();
    reset_context_to_default_state();
    return {};
}

WebIDL::ExceptionOr<void> HTMLCanvasElement::set_height(WebIDL::UnsignedLong value)
{
    if (value > 2147483647)
        value = 150;

    TRY(set_attribute(HTML::AttributeNames::height, String::number(value)));
    notify_context_about_canvas_size_change();
    reset_context_to_default_state();
    return {};
}

void HTMLCanvasElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);

    if (local_name.equals_ignoring_ascii_case(HTML::AttributeNames::width) || local_name.equals_ignoring_ascii_case(HTML::AttributeNames::height)) {
        notify_context_about_canvas_size_change();
        reset_context_to_default_state();
    }
}

GC::Ptr<Layout::Node> HTMLCanvasElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::CanvasBox>(document(), *this, move(style));
}

void HTMLCanvasElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

HTMLCanvasElement::HasOrCreatedContext HTMLCanvasElement::create_2d_context()
{
    if (!m_context.has<Empty>())
        return m_context.has<GC::Ref<CanvasRenderingContext2D>>() ? HasOrCreatedContext::Yes : HasOrCreatedContext::No;

    m_context = CanvasRenderingContext2D::create(realm(), *this);
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
        if (create_2d_context() == HasOrCreatedContext::Yes)
            return GC::make_root(*m_context.get<GC::Ref<HTML::CanvasRenderingContext2D>>());

        return Empty {};
    }

    // NOTE: The WebGL spec says "experimental-webgl" is also acceptable and must be equivalent to "webgl". Other engines accept this, so we do too.
    if (type.is_one_of("webgl"sv, "experimental-webgl"sv)) {
        if (TRY(create_webgl_context<WebGL::WebGLRenderingContext>(options)) == HasOrCreatedContext::Yes)
            return GC::make_root(*m_context.get<GC::Ref<WebGL::WebGLRenderingContext>>());

        return Empty {};
    }

    if (type == "webgl2"sv) {
        if (TRY(create_webgl_context<WebGL::WebGL2RenderingContext>(options)) == HasOrCreatedContext::Yes)
            return GC::make_root(*m_context.get<GC::Ref<WebGL::WebGL2RenderingContext>>());

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
    if (area.value() > max_canvas_area) {
        dbgln("Refusing to create {}x{} canvas (exceeds maximum size)", width, height);
        return {};
    }
    return Gfx::IntSize(width, height);
}

struct SerializeBitmapResult {
    ByteBuffer buffer;
    StringView mime_type;
};

// https://html.spec.whatwg.org/multipage/canvas.html#a-serialisation-of-the-bitmap-as-a-file
static ErrorOr<SerializeBitmapResult> serialize_bitmap(Gfx::Bitmap const& bitmap, StringView type, JS::Value quality)
{
    // If type is an image format that supports variable quality (such as "image/jpeg"), quality is given, and type is not "image/png", then,
    // if quality is a Number in the range 0.0 to 1.0 inclusive, the user agent must treat quality as the desired quality level.
    // Otherwise, the user agent must use its default quality value, as if the quality argument had not been given.
    bool valid_quality = quality.is_number() && quality.as_double() >= 0.0 && quality.as_double() <= 1.0;

    if (type.equals_ignoring_ascii_case("image/jpeg"sv)) {
        AllocatingMemoryStream file;
        Gfx::JPEGWriter::Options jpeg_options;
        if (valid_quality)
            jpeg_options.quality = static_cast<int>(quality.as_double() * 100);
        TRY(Gfx::JPEGWriter::encode(file, bitmap, jpeg_options));
        return SerializeBitmapResult { TRY(file.read_until_eof()), "image/jpeg"sv };
    }

    // User agents must support PNG ("image/png"). User agents may support other types.
    // If the user agent does not support the requested type, then it must create the file using the PNG format. [PNG]
    return SerializeBitmapResult { TRY(Gfx::PNGWriter::encode(bitmap)), "image/png"sv };
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-todataurl
String HTMLCanvasElement::to_data_url(StringView type, JS::Value quality)
{
    // It is possible the canvas doesn't have a associated bitmap so create one
    allocate_painting_surface_if_needed();
    auto surface = this->surface();
    auto size = bitmap_size_for_canvas();
    if (!surface && !size.is_empty()) {
        // If the context is not initialized yet, we need to allocate transparent surface for serialization
        auto skia_backend_context = navigable()->traversable_navigable()->skia_backend_context();
        surface = Gfx::PaintingSurface::create_with_size(skia_backend_context, size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
    }

    // FIXME: 1. If this canvas element's bitmap's origin-clean flag is set to false, then throw a "SecurityError" DOMException.

    // 2. If this canvas element's bitmap has no pixels (i.e. either its horizontal dimension or its vertical dimension is zero)
    //    then return the string "data:,". (This is the shortest data: URL; it represents the empty string in a text/plain resource.)
    if (!surface)
        return "data:,"_string;

    // 3. Let file be a serialization of this canvas element's bitmap as a file, passing type and quality if given.
    auto snapshot = Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface);
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, surface->size()));
    surface->read_into_bitmap(*bitmap);
    auto file = serialize_bitmap(bitmap, type, move(quality));

    // 4. If file is null then return "data:,".
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
WebIDL::ExceptionOr<void> HTMLCanvasElement::to_blob(GC::Ref<WebIDL::CallbackType> callback, StringView type, JS::Value quality)
{
    // It is possible the canvas doesn't have a associated bitmap so create one
    allocate_painting_surface_if_needed();
    auto surface = this->surface();
    auto size = bitmap_size_for_canvas();
    if (!surface && !size.is_empty()) {
        // If the context is not initialized yet, we need to allocate transparent surface for serialization
        auto skia_backend_context = navigable()->traversable_navigable()->skia_backend_context();
        surface = Gfx::PaintingSurface::create_with_size(skia_backend_context, size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
    }

    // FIXME: 1. If this canvas element's bitmap's origin-clean flag is set to false, then throw a "SecurityError" DOMException.

    // 2. Let result be null.
    RefPtr<Gfx::Bitmap> bitmap_result;

    // 3. If this canvas element's bitmap has pixels (i.e., neither its horizontal dimension nor its vertical dimension is zero),
    //    then set result to a copy of this canvas element's bitmap.
    if (surface) {
        bitmap_result = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, surface->size()));
        surface->read_into_bitmap(*bitmap_result);
    }

    // 4. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, callback, bitmap_result, type, quality] {
        // 1. If result is non-null, then set result to a serialization of result as a file with type and quality if given.
        Optional<SerializeBitmapResult> file_result;
        if (bitmap_result) {
            if (auto result = serialize_bitmap(*bitmap_result, type, move(quality)); !result.is_error())
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
                TRY(WebIDL::invoke_callback(*callback, {}, WebIDL::ExceptionBehavior::Report, move(blob_result)));
                return {};
            });
            if (maybe_error.is_throw_completion())
                report_exception(maybe_error.throw_completion(), realm());
        });
    }));
    return {};
}

void HTMLCanvasElement::present()
{
    if (auto surface = this->surface())
        surface->flush();

    m_context.visit(
        [](GC::Ref<CanvasRenderingContext2D>&) {
            // Do nothing, CRC2D writes directly to the canvas bitmap.
        },
        [](GC::Ref<WebGL::WebGLRenderingContext>& context) {
            context->present();
        },
        [](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
            context->present();
        },
        [](Empty) {
            // Do nothing.
        });
}

RefPtr<Gfx::PaintingSurface> HTMLCanvasElement::surface() const
{
    return m_context.visit(
        [&](GC::Ref<CanvasRenderingContext2D> const& context) {
            return context->surface();
        },
        [&](GC::Ref<WebGL::WebGLRenderingContext> const& context) -> RefPtr<Gfx::PaintingSurface> {
            return context->surface();
        },
        [&](GC::Ref<WebGL::WebGL2RenderingContext> const& context) -> RefPtr<Gfx::PaintingSurface> {
            return context->surface();
        },
        [](Empty) -> RefPtr<Gfx::PaintingSurface> {
            return {};
        });
}

void HTMLCanvasElement::allocate_painting_surface_if_needed()
{
    m_context.visit(
        [&](GC::Ref<CanvasRenderingContext2D>& context) {
            context->allocate_painting_surface_if_needed();
        },
        [&](GC::Ref<WebGL::WebGLRenderingContext>& context) {
            context->allocate_painting_surface_if_needed();
        },
        [&](GC::Ref<WebGL::WebGL2RenderingContext>& context) {
            context->allocate_painting_surface_if_needed();
        },
        [](Empty) {
            // Do nothing.
        });
}

}
