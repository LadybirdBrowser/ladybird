/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/Promise.h>
#include <LibGC/Heap.h>
#include <LibGfx/Font/FontSupport.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/FontFacePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

static NonnullRefPtr<Core::Promise<NonnullRefPtr<Gfx::Typeface const>>> load_vector_font(JS::Realm& realm, ByteBuffer const& data)
{
    auto promise = Core::Promise<NonnullRefPtr<Gfx::Typeface const>>::construct();

    // FIXME: 'Asynchronously' shouldn't mean 'later on the main thread'.
    //        Can we defer this to a background thread?
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&data, promise] {
        // FIXME: This should be de-duplicated with StyleComputer::FontLoader::try_load_font
        // We don't have the luxury of knowing the MIME type, so we have to try all formats.
        auto ttf = Gfx::Typeface::try_load_from_externally_owned_memory(data);
        if (!ttf.is_error()) {
            promise->resolve(ttf.release_value());
            return;
        }
        auto woff = WOFF::try_load_from_bytes(data);
        if (!woff.is_error()) {
            promise->resolve(woff.release_value());
            return;
        }
        auto woff2 = WOFF2::try_load_from_bytes(data);
        if (!woff2.is_error()) {
            promise->resolve(woff2.release_value());
            return;
        }
        promise->reject(Error::from_string_literal("Automatic format detection failed"));
    }));

    return promise;
}

GC_DEFINE_ALLOCATOR(FontFace);

// https://drafts.csswg.org/css-font-loading/#font-face-constructor
GC::Ref<FontFace> FontFace::construct_impl(JS::Realm& realm, String const& family, FontFaceSource source, FontFaceDescriptors const& descriptors)
{
    auto& vm = realm.vm();

    // 1. Let font face be a fresh FontFace object. Set font face’s status attribute to "unloaded",
    //    Set its internal [[FontStatusPromise]] slot to a fresh pending Promise object.
    auto font_face = realm.create<FontFace>(realm, WebIDL::create_promise(realm));

    //    Parse the family argument, and the members of the descriptors argument,
    //    according to the grammars of the corresponding descriptors of the CSS @font-face rule.
    //    If the source argument is a CSSOMString, parse it according to the grammar of the CSS src descriptor of the @font-face rule.
    //    If any of them fail to parse correctly, reject font face’s [[FontStatusPromise]] with a DOMException named "SyntaxError",
    //    set font face’s corresponding attributes to the empty string, and set font face’s status attribute to "error".
    //    Otherwise, set font face’s corresponding attributes to the serialization of the parsed values.

    Parser::ParsingParams parsing_params { realm };
    auto try_parse_descriptor = [&parsing_params, &font_face, &realm](DescriptorID descriptor_id, String const& string) -> String {
        auto result = parse_css_descriptor(parsing_params, AtRuleID::FontFace, descriptor_id, string);
        if (!result) {
            font_face->reject_status_promise(WebIDL::SyntaxError::create(realm, Utf16String::formatted("FontFace constructor: Invalid {}", to_string(descriptor_id))));
            return {};
        }

        if (result->is_custom_ident())
            return result->as_custom_ident().custom_ident().to_string();

        return result->to_string(SerializationMode::Normal);
    };
    font_face->m_family = try_parse_descriptor(DescriptorID::FontFamily, family);
    font_face->m_style = try_parse_descriptor(DescriptorID::FontStyle, descriptors.style);
    font_face->m_weight = try_parse_descriptor(DescriptorID::FontWeight, descriptors.weight);
    font_face->m_stretch = try_parse_descriptor(DescriptorID::FontWidth, descriptors.stretch);
    font_face->m_unicode_range = try_parse_descriptor(DescriptorID::UnicodeRange, descriptors.unicode_range);
    font_face->m_feature_settings = try_parse_descriptor(DescriptorID::FontFeatureSettings, descriptors.feature_settings);
    font_face->m_variation_settings = try_parse_descriptor(DescriptorID::FontVariationSettings, descriptors.variation_settings);
    font_face->m_display = try_parse_descriptor(DescriptorID::FontDisplay, descriptors.display);
    font_face->m_ascent_override = try_parse_descriptor(DescriptorID::AscentOverride, descriptors.ascent_override);
    font_face->m_descent_override = try_parse_descriptor(DescriptorID::DescentOverride, descriptors.descent_override);
    font_face->m_line_gap_override = try_parse_descriptor(DescriptorID::LineGapOverride, descriptors.line_gap_override);
    RefPtr<StyleValue const> parsed_source;
    if (auto* source_string = source.get_pointer<String>()) {
        parsed_source = parse_css_descriptor(parsing_params, AtRuleID::FontFace, DescriptorID::Src, *source_string);
        if (!parsed_source) {
            font_face->reject_status_promise(WebIDL::SyntaxError::create(realm, Utf16String::formatted("FontFace constructor: Invalid {}", to_string(DescriptorID::Src))));
        }
    }
    //    Return font face. If font face’s status is "error", terminate this algorithm;
    //    otherwise, complete the rest of these steps asynchronously.
    // FIXME: Do the rest of this asynchronously.
    if (font_face->status() == Bindings::FontFaceLoadStatus::Error)
        return font_face;

    // 2. If the source argument was a CSSOMString, set font face’s internal [[Urls]] slot to the string.
    //    If the source argument was a BinaryData, set font face’s internal [[Data]] slot to the passed argument.
    if (source.has<String>()) {
        font_face->m_urls = ParsedFontFace::sources_from_style_value(*parsed_source);
    } else {
        auto buffer_source = source.get<GC::Root<WebIDL::BufferSource>>();
        auto maybe_buffer = WebIDL::get_buffer_source_copy(buffer_source->raw_object());
        if (maybe_buffer.is_error()) {
            VERIFY(maybe_buffer.error().code() == ENOMEM);
            auto throw_completion = vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            font_face->reject_status_promise(throw_completion.value());
        } else {
            font_face->m_binary_data = maybe_buffer.release_value();
        }
    }

    if (font_face->m_binary_data.is_empty() && font_face->m_urls.is_empty())
        font_face->reject_status_promise(WebIDL::SyntaxError::create(realm, "FontFace constructor: Invalid font source"_utf16));

    // 3. If font face’s [[Data]] slot is not null, queue a task to run the following steps synchronously:
    if (font_face->m_binary_data.is_empty())
        return font_face;

    HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font_face), GC::create_function(vm.heap(), [&realm, font_face] {
        // 1.  Set font face’s status attribute to "loading".
        font_face->m_status = Bindings::FontFaceLoadStatus::Loading;

        // 2. FIXME: For each FontFaceSet font face is in:

        // 3. Asynchronously, attempt to parse the data in it as a font.
        //    When this is completed, successfully or not, queue a task to run the following steps synchronously:
        font_face->m_font_load_promise = load_vector_font(realm, font_face->m_binary_data);

        font_face->m_font_load_promise->when_resolved([font = GC::make_root(font_face)](auto const& vector_font) -> ErrorOr<void> {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), GC::create_function(font->heap(), [font = GC::Ref(*font), vector_font] {
                HTML::TemporaryExecutionContext context(font->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
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
        font_face->m_font_load_promise->when_rejected([font = GC::make_root(font_face)](auto const& error) {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), GC::create_function(font->heap(), [font = GC::Ref(*font), error = Error::copy(error)] {
                HTML::TemporaryExecutionContext context(font->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                // 2. Otherwise, reject font face’s [[FontStatusPromise]] with a DOMException named "SyntaxError"
                //    and set font face’s status attribute to "error".
                font->reject_status_promise(WebIDL::SyntaxError::create(font->realm(), Utf16String::formatted("Failed to load font: {}", error)));

                // FIXME: For each FontFaceSet font face is in:

                font->m_font_load_promise = nullptr;
            }));
        });
    }));

    return font_face;
}

FontFace::FontFace(JS::Realm& realm, GC::Ref<WebIDL::Promise> font_status_promise)
    : Bindings::PlatformObject(realm)
    , m_font_status_promise(font_status_promise)
{
}

FontFace::~FontFace() = default;

void FontFace::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FontFace);
    Base::initialize(realm);
}

void FontFace::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_font_status_promise);
}

GC::Ref<WebIDL::Promise> FontFace::loaded() const
{
    return m_font_status_promise;
}

void FontFace::reject_status_promise(JS::Value reason)
{
    if (m_status != Bindings::FontFaceLoadStatus::Error) {
        WebIDL::reject_promise(realm(), m_font_status_promise, reason);
        m_status = Bindings::FontFaceLoadStatus::Error;
    }
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-family
WebIDL::ExceptionOr<void> FontFace::set_family(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontFamily, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.family setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-family property
    }

    m_family = property->as_custom_ident().custom_ident().to_string();

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-style
WebIDL::ExceptionOr<void> FontFace::set_style(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontStyle, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.style setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-style property
    }

    m_style = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-weight
WebIDL::ExceptionOr<void> FontFace::set_weight(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontWeight, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.weight setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-weight property
    }

    m_weight = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-stretch
WebIDL::ExceptionOr<void> FontFace::set_stretch(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    // NOTE: font-stretch is now an alias for font-width
    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontWidth, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.stretch setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_stretch = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-unicoderange
WebIDL::ExceptionOr<void> FontFace::set_unicode_range(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::UnicodeRange, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.unicodeRange setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_unicode_range = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-featuresettings
WebIDL::ExceptionOr<void> FontFace::set_feature_settings(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontFeatureSettings, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.featureSettings setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_feature_settings = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-variationsettings
WebIDL::ExceptionOr<void> FontFace::set_variation_settings(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontVariationSettings, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.variationSettings setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_variation_settings = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-display
WebIDL::ExceptionOr<void> FontFace::set_display(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::FontDisplay, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.display setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_display = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-ascentoverride
WebIDL::ExceptionOr<void> FontFace::set_ascent_override(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::AscentOverride, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.ascentOverride setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_ascent_override = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-descentoverride
WebIDL::ExceptionOr<void> FontFace::set_descent_override(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::DescentOverride, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.descentOverride setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_descent_override = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-linegapoverride
WebIDL::ExceptionOr<void> FontFace::set_line_gap_override(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorID::LineGapOverride, string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.lineGapOverride setter: Invalid descriptor value"_utf16);

    if (m_is_css_connected) {
        // FIXME: Propagate to the CSSFontFaceRule and update the font-width property
    }

    m_line_gap_override = property->to_string(SerializationMode::Normal);

    return {};
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-load
GC::Ref<WebIDL::Promise> FontFace::load()
{
    //  1. Let font face be the FontFace object on which this method was called.
    auto& font_face = *this;

    // 2. If font face’s [[Urls]] slot is null, or its status attribute is anything other than "unloaded",
    //    return font face’s [[FontStatusPromise]] and abort these steps.
    if (font_face.m_urls.is_empty() || font_face.m_status != Bindings::FontFaceLoadStatus::Unloaded)
        return font_face.loaded();

    // 3. Otherwise, set font face’s status attribute to "loading", return font face’s [[FontStatusPromise]],
    //    and continue executing the rest of this algorithm asynchronously.
    m_status = Bindings::FontFaceLoadStatus::Loading;

    Web::Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [font = GC::make_root(this)] {
        // 4. Using the value of font face’s [[Urls]] slot, attempt to load a font as defined in [CSS-FONTS-3],
        //     as if it was the value of a @font-face rule’s src descriptor.

        // 5. When the load operation completes, successfully or not, queue a task to run the follsowing steps synchronously:
        auto on_load = [font](RefPtr<Gfx::Typeface const> const& maybe_typeface) {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), GC::create_function(font->heap(), [font = GC::Ref(*font), maybe_typeface] {
                HTML::TemporaryExecutionContext context(font->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                // 1. If the attempt to load fails, reject font face’s [[FontStatusPromise]] with a DOMException whose name
                //    is "NetworkError" and set font face’s status attribute to "error".
                if (!maybe_typeface) {
                    font->m_status = Bindings::FontFaceLoadStatus::Error;
                    WebIDL::reject_promise(font->realm(), font->m_font_status_promise, WebIDL::NetworkError::create(font->realm(), "Failed to load font"_utf16));

                    // FIXME: For each FontFaceSet font face is in:
                }

                // 2. Otherwise, font face now represents the loaded font; fulfill font face’s [[FontStatusPromise]] with font face
                //    and set font face’s status attribute to "loaded".
                else {
                    font->m_parsed_font = maybe_typeface;
                    font->m_status = Bindings::FontFaceLoadStatus::Loaded;
                    WebIDL::resolve_promise(font->realm(), font->m_font_status_promise, font);

                    // FIXME: For each FontFaceSet font face is in:
                }
            }));
        };

        // FIXME: We should probably put the 'font cache' on the WindowOrWorkerGlobalScope instead of tying it to the document's style computer
        auto& global = HTML::relevant_global_object(*font);
        if (auto* window = as_if<HTML::Window>(global)) {
            auto& style_computer = const_cast<StyleComputer&>(window->document()->style_computer());

            // FIXME: The ParsedFontFace is kind of expensive to create. We should be using a shared sub-object for the data
            ParsedFontFace parsed_font_face {
                nullptr,
                font->m_family,
                font->m_weight.to_number<int>(),
                0,                      // FIXME: slope
                Gfx::FontWidth::Normal, // FIXME: width
                font->m_urls,
                font->m_unicode_ranges,
                {},                // FIXME: ascent_override
                {},                // FIXME: descent_override
                {},                // FIXME: line_gap_override
                FontDisplay::Auto, // FIXME: font_display
                {},                // font-named-instance doesn't exist in FontFace
                {},                // font-language-override doesn't exist in FontFace
                {},                // FIXME: feature_settings
                {},                // FIXME: variation_settings
            };
            if (auto loader = style_computer.load_font_face(parsed_font_face, move(on_load)))
                loader->start_loading_next_url();
        } else {
            // FIXME: Don't know how to load fonts in workers! They don't have a StyleComputer
            dbgln("FIXME: Worker font loading not implemented");
        }
    }));

    // User agents can initiate font loads on their own, whenever they determine that a given font face is necessary
    // to render something on the page. When this happens, they must act as if they had called the corresponding
    // FontFace’s load() method described here.

    return font_face.loaded();
}

bool font_format_is_supported(FlyString const& name)
{
    // https://drafts.csswg.org/css-fonts-4/#font-format-definitions
    if (name.equals_ignoring_ascii_case("collection"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::TrueTypeCollection);
    if (name.equals_ignoring_ascii_case("embedded-opentype"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::EmbeddedOpenType);
    if (name.equals_ignoring_ascii_case("opentype"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::OpenType);
    if (name.equals_ignoring_ascii_case("svg"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::SVG);
    if (name.equals_ignoring_ascii_case("truetype"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::TrueType);
    if (name.equals_ignoring_ascii_case("woff"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::WOFF);
    if (name.equals_ignoring_ascii_case("woff2"sv))
        return Gfx::font_format_is_supported(Gfx::FontFormat::WOFF2);
    return false;
}

bool font_tech_is_supported(FontTech font_tech)
{
    // https://drafts.csswg.org/css-fonts-4/#font-tech-definitions
    switch (font_tech) {
    case FontTech::FeaturesOpentype:
        return Gfx::font_tech_is_supported(Gfx::FontTech::FeaturesOpentype);
    case FontTech::FeaturesAat:
        return Gfx::font_tech_is_supported(Gfx::FontTech::FeaturesAat);
    case FontTech::FeaturesGraphite:
        return Gfx::font_tech_is_supported(Gfx::FontTech::FeaturesGraphite);
    case FontTech::Variations:
        return Gfx::font_tech_is_supported(Gfx::FontTech::Variations);
    case FontTech::ColorColrv0:
        return Gfx::font_tech_is_supported(Gfx::FontTech::ColorColrv0);
    case FontTech::ColorColrv1:
        return Gfx::font_tech_is_supported(Gfx::FontTech::ColorColrv1);
    case FontTech::ColorSvg:
        return Gfx::font_tech_is_supported(Gfx::FontTech::ColorSvg);
    case FontTech::ColorSbix:
        return Gfx::font_tech_is_supported(Gfx::FontTech::ColorSbix);
    case FontTech::ColorCbdt:
        return Gfx::font_tech_is_supported(Gfx::FontTech::ColorCbdt);
    case FontTech::Palettes:
        return Gfx::font_tech_is_supported(Gfx::FontTech::Palettes);
    case FontTech::Incremental:
        return Gfx::font_tech_is_supported(Gfx::FontTech::Incremental);
    // https://drafts.csswg.org/css-fonts-5/#font-tech-definitions
    case FontTech::Avar2:
        return Gfx::font_tech_is_supported(Gfx::FontTech::Avar2);
    }
    return false;
}

bool font_tech_is_supported(FlyString const& name)
{
    if (auto keyword = keyword_from_string(name); keyword.has_value()) {
        if (auto font_tech = keyword_to_font_tech(*keyword); font_tech.has_value()) {
            return font_tech_is_supported(*font_tech);
        }
    }
    return false;
}

}
