/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Promise.h>
#include <LibGfx/Font/OpenType/Font.h>
#include <LibGfx/Font/VectorFont.h>
#include <LibGfx/Font/WOFF/Font.h>
#include <LibGfx/Font/WOFF2/Font.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/FontFacePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

static NonnullRefPtr<Core::Promise<NonnullRefPtr<Gfx::VectorFont>>> load_vector_font(ByteBuffer const& data)
{
    auto promise = Core::Promise<NonnullRefPtr<Gfx::VectorFont>>::construct();

    // FIXME: 'Asynchronously' shouldn't mean 'later on the main thread'.
    //        Can we defer this to a background thread?
    Platform::EventLoopPlugin::the().deferred_invoke([&data, promise] {
        // FIXME: This should be de-duplicated with StyleComputer::FontLoader::try_load_font
        // We don't have the luxury of knowing the MIME type, so we have to try all formats.
        auto ttf = OpenType::Font::try_load_from_externally_owned_memory(data);
        if (!ttf.is_error()) {
            promise->resolve(ttf.release_value());
            return;
        }
        auto woff = WOFF::Font::try_load_from_externally_owned_memory(data);
        if (!woff.is_error()) {
            promise->resolve(woff.release_value());
            return;
        }
        auto woff2 = WOFF2::Font::try_load_from_externally_owned_memory(data);
        if (!woff2.is_error()) {
            promise->resolve(woff2.release_value());
            return;
        }
        promise->reject(Error::from_string_literal("Automatic format detection failed"));
    });

    return promise;
}

JS_DEFINE_ALLOCATOR(FontFace);

template<CSS::PropertyID PropertyID>
RefPtr<CSS::StyleValue const> parse_property_string(JS::Realm& realm, StringView value)
{
    auto maybe_parser = CSS::Parser::Parser::create(CSS::Parser::ParsingContext(realm), value);
    if (maybe_parser.is_error())
        return {};

    return maybe_parser.release_value().parse_as_css_value(PropertyID);
}

// https://drafts.csswg.org/css-font-loading/#font-face-constructor
JS::NonnullGCPtr<FontFace> FontFace::construct_impl(JS::Realm& realm, String family, FontFaceSource source, FontFaceDescriptors const& descriptors)
{
    auto& vm = realm.vm();
    auto base_url = HTML::relevant_settings_object(realm.global_object()).api_base_url();

    // 1. Let font face be a fresh FontFace object. Set font face’s status attribute to "unloaded",
    //    Set its internal [[FontStatusPromise]] slot to a fresh pending Promise object.
    auto promise = WebIDL::create_promise(realm);

    // FIXME: Parse the family argument, and the members of the descriptors argument,
    //    according to the grammars of the corresponding descriptors of the CSS @font-face rule.
    //    If the source argument is a CSSOMString, parse it according to the grammar of the CSS src descriptor of the @font-face rule.
    //    If any of them fail to parse correctly, reject font face’s [[FontStatusPromise]] with a DOMException named "SyntaxError",
    //    set font face’s corresponding attributes to the empty string, and set font face’s status attribute to "error".
    //    Otherwise, set font face’s corresponding attributes to the serialization of the parsed values.

    // 2. (Out of order) If the source argument was a CSSOMString, set font face’s internal [[Urls]]
    //    slot to the string.
    //    If the source argument was a BinaryData, set font face’s internal [[Data]] slot
    //    to the passed argument.
    Vector<CSS::ParsedFontFace::Source> sources;
    ByteBuffer buffer;
    if (auto* string = source.get_pointer<String>()) {
        auto maybe_parser = CSS::Parser::Parser::create(CSS::Parser::ParsingContext(realm, base_url), *string);
        if (maybe_parser.is_error()) {
            WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "FontFace constructor: Invalid source string"_fly_string));
        } else {
            auto parser = maybe_parser.release_value();
            sources = parser.parse_as_font_face_src();
            if (sources.is_empty())
                WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "FontFace constructor: Invalid source string"_fly_string));
        }
    } else {
        auto buffer_source = source.get<JS::Handle<WebIDL::BufferSource>>();
        auto maybe_buffer = WebIDL::get_buffer_source_copy(buffer_source->raw_object());
        if (maybe_buffer.is_error()) {
            VERIFY(maybe_buffer.error().code() == ENOMEM);
            auto throw_completion = vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            WebIDL::reject_promise(realm, promise, *throw_completion.value());
        } else {
            buffer = maybe_buffer.release_value();
        }
    }

    if (buffer.is_empty() && sources.is_empty())
        WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "FontFace constructor: Invalid font source"_fly_string));

    auto font = realm.heap().allocate<FontFace>(realm, realm, promise, move(sources), move(buffer), move(family), descriptors);

    // 1. (continued) Return font face. If font face’s status is "error", terminate this algorithm;
    //    otherwise, complete the rest of these steps asynchronously.
    if (font->status() == Bindings::FontFaceLoadStatus::Error)
        return font;

    // 3. If font face’s [[Data]] slot is not null, queue a task to run the following steps synchronously:
    if (font->m_binary_data.is_empty())
        return font;

    HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), JS::create_heap_function(vm.heap(), [font] {
        // 1.  Set font face’s status attribute to "loading".
        font->m_status = Bindings::FontFaceLoadStatus::Loading;

        // 2. FIXME: For each FontFaceSet font face is in:

        // 3. Asynchronously, attempt to parse the data in it as a font.
        //    When this is completed, successfully or not, queue a task to run the following steps synchronously:
        font->m_font_load_promise = load_vector_font(font->m_binary_data);

        font->m_font_load_promise->when_resolved([font = JS::make_handle(font)](auto const& vector_font) -> ErrorOr<void> {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), JS::create_heap_function(font->heap(), [font = JS::NonnullGCPtr(*font), vector_font] {
                HTML::TemporaryExecutionContext context(HTML::relevant_settings_object(*font), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                // 1. If the load was successful, font face now represents the parsed font;
                //    fulfill font face’s [[FontStatusPromise]] with font face, and set its status attribute to "loaded".

                // FIXME: Are we supposed to set the properties of the FontFace based on the loaded vector font?
                font->m_parsed_font = vector_font;
                font->m_status = Bindings::FontFaceLoadStatus::Loaded;
                WebIDL::resolve_promise(font->realm(), font->m_font_status_promise, font);

                // FIXME: For each FontFaceSet font face is in:

                font->m_font_load_promise = nullptr;
            }));
            return {};
        });
        font->m_font_load_promise->when_rejected([font = JS::make_handle(font)](auto const& error) {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), JS::create_heap_function(font->heap(), [font = JS::NonnullGCPtr(*font), error = Error::copy(error)] {
                HTML::TemporaryExecutionContext context(HTML::relevant_settings_object(*font), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                // 2. Otherwise, reject font face’s [[FontStatusPromise]] with a DOMException named "SyntaxError"
                //    and set font face’s status attribute to "error".
                font->m_status = Bindings::FontFaceLoadStatus::Error;
                WebIDL::reject_promise(font->realm(), font->m_font_status_promise, WebIDL::SyntaxError::create(font->realm(), MUST(String::formatted("Failed to load font: {}", error))));

                // FIXME: For each FontFaceSet font face is in:

                font->m_font_load_promise = nullptr;
            }));
        });
    }));

    return font;
}

FontFace::FontFace(JS::Realm& realm, JS::NonnullGCPtr<WebIDL::Promise> font_status_promise, Vector<ParsedFontFace::Source> urls, ByteBuffer data, String font_family, FontFaceDescriptors const& descriptors)
    : Bindings::PlatformObject(realm)
    , m_font_status_promise(font_status_promise)
    , m_urls(move(urls))
    , m_binary_data(move(data))
{
    m_family = move(font_family);
    m_style = descriptors.style;
    m_weight = descriptors.weight;
    m_stretch = descriptors.stretch;
    m_unicode_range = descriptors.unicode_range;
    m_feature_settings = descriptors.feature_settings;
    m_variation_settings = descriptors.variation_settings;
    m_display = descriptors.display;
    m_ascent_override = descriptors.ascent_override;
    m_descent_override = descriptors.descent_override;
    m_line_gap_override = descriptors.line_gap_override;

    // FIXME: Parse from descriptor
    // FIXME: Have gettter reflect this member instead of the string
    m_unicode_ranges.empend(0x0u, 0x10FFFFu);

    if (verify_cast<JS::Promise>(*m_font_status_promise->promise()).state() == JS::Promise::State::Rejected)
        m_status = Bindings::FontFaceLoadStatus::Error;
}

FontFace::~FontFace() = default;

void FontFace::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    WEB_SET_PROTOTYPE_FOR_INTERFACE(FontFace);
}

void FontFace::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_font_status_promise);
}

JS::NonnullGCPtr<JS::Promise> FontFace::loaded() const
{
    return verify_cast<JS::Promise>(*m_font_status_promise->promise());
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-family
WebIDL::ExceptionOr<void> FontFace::set_family(String const& string)
{
    auto property = parse_property_string<CSS::PropertyID::FontFamily>(realm(), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.family setter: Invalid font descriptor"_fly_string);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-family property
    }

    m_family = property->to_string();

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-style
WebIDL::ExceptionOr<void> FontFace::set_style(String const& string)
{
    auto property = parse_property_string<CSS::PropertyID::FontStyle>(realm(), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.style setter: Invalid font descriptor"_fly_string);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-style property
    }

    m_style = property->to_string();

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-weight
WebIDL::ExceptionOr<void> FontFace::set_weight(String const& string)
{
    auto property = parse_property_string<CSS::PropertyID::FontWeight>(realm(), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.weight setter: Invalid font descriptor"_fly_string);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-weight property
    }

    m_weight = property->to_string();

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-stretch
WebIDL::ExceptionOr<void> FontFace::set_stretch(String const& string)
{
    auto property = parse_property_string<CSS::PropertyID::FontStretch>(realm(), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.stretch setter: Invalid font descriptor"_fly_string);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-stretch property
    }

    m_stretch = property->to_string();

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-unicoderange
WebIDL::ExceptionOr<void> FontFace::set_unicode_range(String const&)
{
    // FIXME: This *should* work, but the <urange> production is hard to parse
    //        from just a value string in our implementation
    return WebIDL::NotSupportedError::create(realm(), "unicode range is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-featuresettings
WebIDL::ExceptionOr<void> FontFace::set_feature_settings(String const&)
{
    return WebIDL::NotSupportedError::create(realm(), "feature settings is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-variationsettings
WebIDL::ExceptionOr<void> FontFace::set_variation_settings(String const&)
{
    return WebIDL::NotSupportedError::create(realm(), "variation settings is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-display
WebIDL::ExceptionOr<void> FontFace::set_display(String const&)
{
    return WebIDL::NotSupportedError::create(realm(), "display is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-ascentoverride
WebIDL::ExceptionOr<void> FontFace::set_ascent_override(String const&)
{
    return WebIDL::NotSupportedError::create(realm(), "ascent override is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-descentoverride
WebIDL::ExceptionOr<void> FontFace::set_descent_override(String const&)
{
    return WebIDL::NotSupportedError::create(realm(), "descent override is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-linegapoverride
WebIDL::ExceptionOr<void> FontFace::set_line_gap_override(String const&)
{
    return WebIDL::NotSupportedError::create(realm(), "line gap override is not yet implemented"_fly_string);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-load
JS::NonnullGCPtr<JS::Promise> FontFace::load()
{
    //  1. Let font face be the FontFace object on which this method was called.
    auto& font_face = *this;

    // 2. If font face’s [[Urls]] slot is null, or its status attribute is anything other than "unloaded",
    //    return font face’s [[FontStatusPromise]] and abort these steps.
    if (font_face.m_urls.is_empty() || font_face.m_status != Bindings::FontFaceLoadStatus::Unloaded)
        return font_face.loaded();

    load_font_source();

    return font_face.loaded();
}

void FontFace::load_font_source()
{
    VERIFY(!m_urls.is_empty() && m_status == Bindings::FontFaceLoadStatus::Unloaded);
    // NOTE: These steps are from the load() method, but can also be called by the user agent when the font
    //       is needed to render something on the page.

    // User agents can initiate font loads on their own, whenever they determine that a given font face is necessary
    // to render something on the page. When this happens, they must act as if they had called the corresponding
    // FontFace’s load() method described here.

    // 3. Otherwise, set font face’s status attribute to "loading", return font face’s [[FontStatusPromise]],
    //    and continue executing the rest of this algorithm asynchronously.
    m_status = Bindings::FontFaceLoadStatus::Loading;

    Web::Platform::EventLoopPlugin::the().deferred_invoke([font = JS::make_handle(this)] {
        // 4. Using the value of font face’s [[Urls]] slot, attempt to load a font as defined in [CSS-FONTS-3],
        //     as if it was the value of a @font-face rule’s src descriptor.

        // 5. When the load operation completes, successfully or not, queue a task to run the following steps synchronously:
        auto on_error = [font] {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), JS::create_heap_function(font->heap(), [font = JS::NonnullGCPtr(*font)] {
                HTML::TemporaryExecutionContext context(HTML::relevant_settings_object(*font), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

                //     1. If the attempt to load fails, reject font face’s [[FontStatusPromise]] with a DOMException whose name
                //        is "NetworkError" and set font face’s status attribute to "error".
                font->m_status = Bindings::FontFaceLoadStatus::Error;
                WebIDL::reject_promise(font->realm(), font->m_font_status_promise, WebIDL::NetworkError::create(font->realm(), "Failed to load font"_fly_string));

                // FIXME: For each FontFaceSet font face is in:
            }));
        };

        auto on_load = [font](FontLoader const& loader) {
            // FIXME: We are assuming that the font loader will live as long as the document! This is an unsafe capture
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), JS::create_heap_function(font->heap(), [font = JS::NonnullGCPtr(*font), &loader] {
                HTML::TemporaryExecutionContext context(HTML::relevant_settings_object(*font), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

                // 2. Otherwise, font face now represents the loaded font; fulfill font face’s [[FontStatusPromise]] with font face
                //    and set font face’s status attribute to "loaded".
                font->m_parsed_font = loader.vector_font();
                font->m_status = Bindings::FontFaceLoadStatus::Loaded;
                WebIDL::resolve_promise(font->realm(), font->m_font_status_promise, font);

                // FIXME: For each FontFaceSet font face is in:
            }));
        };

        // FIXME: We should probably put the 'font cache' on the WindowOrWorkerGlobalScope instead of tying it to the document's style computer
        auto& global = HTML::relevant_global_object(*font);
        if (is<HTML::Window>(global)) {
            auto& window = static_cast<HTML::Window&>(global);
            auto& style_computer = const_cast<StyleComputer&>(window.document()->style_computer());

            // FIXME: The ParsedFontFace is kind of expensive to create. We should be using a shared sub-object for the data
            ParsedFontFace parsed_font_face { font->m_family, font->m_weight.to_number<int>(), 0 /* FIXME: slope */, font->m_urls, font->m_unicode_ranges };
            if (auto loader = style_computer.load_font_face(parsed_font_face, move(on_load), move(on_error)); loader.has_value())
                loader->start_loading_next_url();
        } else {
            // FIXME: Don't know how to load fonts in workers! They don't have a StyleComputer
            dbgln("FIXME: Worker font loading not implemented");
        }
    });
}

}
