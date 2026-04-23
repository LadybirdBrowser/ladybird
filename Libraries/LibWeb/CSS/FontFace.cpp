/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/Promise.h>
#include <LibGC/Heap.h>
#include <LibGfx/Font/FontSupport.h>
#include <LibGfx/Font/Typeface.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/FontFace.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/FontLoading.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

// In order to avoid conflicts with the old WinIE style of @font-face, if there is no format specified,
// we check to see if the URL ends with .eot. We will not try to load those.
// This matches the behavior of other engines (Blink, WebKit).
static bool is_unsupported_source(ParsedFontFace::Source const& source)
{
    if (!source.local_or_url.has<URL>())
        return false;
    if (source.format.has_value())
        return !font_format_is_supported(source.format.value());
    return source.local_or_url.get<URL>().url().ends_with_bytes(".eot"sv);
}

static FontWeightRange compute_weight_range(StyleValue const& value)
{
    if (value.to_keyword() == Keyword::Auto || value.to_keyword() == Keyword::Normal)
        return { 400, 400 };

    auto& weight_values = value.as_value_list().values();
    if (weight_values.size() == 1) {
        auto one_weight = static_cast<int>(StyleComputer::compute_font_weight(weight_values[0], {})->as_number().number());
        return { one_weight, one_weight };
    }
    if (weight_values.size() == 2) {
        auto first = static_cast<int>(StyleComputer::compute_font_weight(weight_values[0], {})->as_number().number());
        auto second = static_cast<int>(StyleComputer::compute_font_weight(weight_values[1], {})->as_number().number());
        return { min(first, second), max(first, second) };
    }
    return { 400, 400 };
}

static int compute_slope(StyleValue const& value)
{
    if (value.to_keyword() == Keyword::Auto || value.to_keyword() == Keyword::Normal)
        return 0;

    return StyleComputer::compute_font_style(value)->as_font_style().to_font_slope();
}

static int compute_width(StyleValue const& value)
{
    if (value.to_keyword() == Keyword::Auto || value.to_keyword() == Keyword::Normal)
        return 100;

    return static_cast<int>(StyleComputer::compute_font_width(value)->as_percentage().raw_value());
}

static NonnullRefPtr<Core::Promise<NonnullRefPtr<Gfx::Typeface const>>> load_vector_font([[maybe_unused]] JS::Realm& realm, ByteBuffer data)
{
    auto promise = Core::Promise<NonnullRefPtr<Gfx::Typeface const>>::construct();

    if (!requires_off_thread_vector_font_preparation(data)) {
        auto result = try_load_vector_font(data);
        if (result.is_error()) {
            promise->reject(result.release_error());
            return promise;
        }

        promise->resolve(result.release_value());
        return promise;
    }

    prepare_vector_font_data_off_thread(move(data), [promise](auto prepared_font_data) {
        if (prepared_font_data.is_error()) {
            promise->reject(prepared_font_data.release_error());
            return;
        }

        auto prepared = prepared_font_data.release_value();
        auto result = try_load_vector_font(prepared.data, prepared.mime_type_essence);
        if (result.is_error()) {
            promise->reject(result.release_error());
            return;
        }

        promise->resolve(result.release_value());
    });

    return promise;
}

GC_DEFINE_ALLOCATOR(FontFace);

// https://drafts.csswg.org/css-font-loading/#font-face-constructor
GC::Ref<FontFace> FontFace::construct_impl(JS::Realm& realm, String family, FontFaceSource source, FontFaceDescriptors const& descriptors)
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
    auto try_set_descriptor = [&](DescriptorID descriptor_id, String const& string, auto setter_impl) {
        auto result = parse_css_descriptor(parsing_params, AtRuleID::FontFace, DescriptorNameAndID::from_id(descriptor_id), string);
        if (!result) {
            font_face->reject_status_promise(WebIDL::SyntaxError::create(realm, Utf16String::formatted("FontFace constructor: Invalid {}", to_string(descriptor_id))));
            return;
        }
        (font_face.ptr()->*setter_impl)(result.release_nonnull());
    };
    try_set_descriptor(DescriptorID::FontFamily, family, &FontFace::set_family_impl);
    try_set_descriptor(DescriptorID::FontStyle, descriptors.style, &FontFace::set_style_impl);
    try_set_descriptor(DescriptorID::FontWeight, descriptors.weight, &FontFace::set_weight_impl);
    try_set_descriptor(DescriptorID::FontWidth, descriptors.stretch, &FontFace::set_stretch_impl);
    try_set_descriptor(DescriptorID::UnicodeRange, descriptors.unicode_range, &FontFace::set_unicode_range_impl);
    try_set_descriptor(DescriptorID::FontFeatureSettings, descriptors.feature_settings, &FontFace::set_feature_settings_impl);
    try_set_descriptor(DescriptorID::FontVariationSettings, descriptors.variation_settings, &FontFace::set_variation_settings_impl);
    try_set_descriptor(DescriptorID::FontDisplay, descriptors.display, &FontFace::set_display_impl);
    try_set_descriptor(DescriptorID::AscentOverride, descriptors.ascent_override, &FontFace::set_ascent_override_impl);
    try_set_descriptor(DescriptorID::DescentOverride, descriptors.descent_override, &FontFace::set_descent_override_impl);
    try_set_descriptor(DescriptorID::LineGapOverride, descriptors.line_gap_override, &FontFace::set_line_gap_override_impl);
    RefPtr<StyleValue const> parsed_source;
    if (auto* source_string = source.get_pointer<String>()) {
        parsed_source = parse_css_descriptor(parsing_params, AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::Src), *source_string);
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
        font_face->m_urls.remove_all_matching(is_unsupported_source);
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
        HTML::TemporaryExecutionContext context(font_face->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 1.  Set font face’s status attribute to "loading".
        font_face->m_status = Bindings::FontFaceLoadStatus::Loading;

        // 2. For each FontFaceSet font face is in:
        for (auto& font_face_set : font_face->m_containing_sets) {
            // 1. If the FontFaceSet’s [[LoadingFonts]] list is empty, switch the FontFaceSet to loading.
            if (font_face_set->loading_fonts().is_empty())
                font_face_set->switch_to_loading();

            // 2. Append font face to the FontFaceSet’s [[LoadingFonts]] list.
            font_face_set->loading_fonts().append(font_face);
        }

        // 3. Asynchronously, attempt to parse the data in it as a font.
        //    When this is completed, successfully or not, queue a task to run the following steps synchronously:
        font_face->m_font_load_promise = load_vector_font(realm, move(font_face->m_binary_data));

        font_face->m_font_load_promise->when_resolved([font = GC::make_root(font_face)](auto const& vector_font) -> ErrorOr<void> {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*font), GC::create_function(font->heap(), [font = GC::Ref(*font), vector_font] {
                HTML::TemporaryExecutionContext context(font->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                // 1. If the load was successful, font face now represents the parsed font;
                //    fulfill font face’s [[FontStatusPromise]] with font face, and set its status attribute to "loaded".

                // FIXME: Are we supposed to set the properties of the FontFace based on the loaded vector font?
                font->m_parsed_font = vector_font;
                font->m_status = Bindings::FontFaceLoadStatus::Loaded;
                WebIDL::resolve_promise(font->realm(), font->m_font_status_promise, font);

                if (auto font_computer = font->font_computer(); font_computer.has_value())
                    font_computer->register_font_face(*font);

                // For each FontFaceSet font face is in:
                for (auto& font_face_set : font->m_containing_sets) {
                    // 1. Add font face to the FontFaceSet’s [[LoadedFonts]] list.
                    font_face_set->loaded_fonts().append(font);

                    // 2. Remove font face from the FontFaceSet’s [[LoadingFonts]] list. If font was the last item in
                    //    that list (and so the list is now empty), switch the FontFaceSet to loaded.
                    font_face_set->loading_fonts().remove_all_matching([font](auto const& entry) { return entry == font; });
                    if (font_face_set->loading_fonts().is_empty())
                        font_face_set->switch_to_loaded();
                }

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

                // For each FontFaceSet font face is in:
                for (auto& font_face_set : font->m_containing_sets) {
                    // 1. Add font face to the FontFaceSet’s [[FailedFonts]] list.
                    font_face_set->failed_fonts().append(font);

                    // 2. Remove font face from the FontFaceSet’s [[LoadingFonts]] list. If font was the last item in
                    //    that list (and so the list is now empty), switch the FontFaceSet to loaded.
                    font_face_set->loading_fonts().remove_all_matching([font](auto const& entry) { return entry == font; });
                    if (font_face_set->loading_fonts().is_empty())
                        font_face_set->switch_to_loaded();
                }

                font->m_font_load_promise = nullptr;
            }));
        });
    }));

    return font_face;
}

// https://drafts.csswg.org/css-font-loading/#font-face-css-connection
GC::Ref<FontFace> FontFace::create_css_connected(JS::Realm& realm, CSSFontFaceRule& rule)
{
    HTML::TemporaryExecutionContext execution_context { realm };

    auto font_face = realm.create<FontFace>(realm, WebIDL::create_promise(realm));

    font_face->m_css_font_face_rule = &rule;
    font_face->reparse_connected_css_font_face_rule_descriptors();

    if (auto src_value = rule.descriptors()->descriptor(DescriptorNameAndID::from_id(DescriptorID::Src))) {
        font_face->m_urls = ParsedFontFace::sources_from_style_value(*src_value);
        font_face->m_urls.remove_all_matching(is_unsupported_source);
    }

    rule.set_css_connected_font_face(font_face);

    return font_face;
}

void FontFace::reparse_connected_css_font_face_rule_descriptors()
{
    auto const& descriptors = m_css_font_face_rule->descriptors();

    ComputationContext computation_context {
        .length_resolution_context = Length::ResolutionContext::for_document(*descriptors->parent_rule()->parent_style_sheet()->owning_document())
    };

    set_family_impl(*descriptors->descriptor(DescriptorNameAndID::from_id(DescriptorID::FontFamily)));
    set_style_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::FontStyle))->absolutized(computation_context));
    set_weight_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::FontWeight))->absolutized(computation_context));
    set_stretch_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::FontWidth)));
    set_unicode_range_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::UnicodeRange)));
    set_feature_settings_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::FontFeatureSettings)));
    set_variation_settings_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::FontVariationSettings)));
    set_display_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::FontDisplay)));
    set_ascent_override_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::AscentOverride)));
    set_descent_override_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::DescentOverride)));
    set_line_gap_override_impl(*descriptors->descriptor_or_initial_value(DescriptorNameAndID::from_id(DescriptorID::LineGapOverride)));
}

ParsedFontFace FontFace::parsed_font_face() const
{
    if (m_css_font_face_rule)
        return m_css_font_face_rule->font_face();

    // FIXME: The ParsedFontFace is kind of expensive to create. We should be using a shared sub-object for the data
    return ParsedFontFace {
        // Create a dummy CSSFontFaceRule so that we load relative to the document's base URL
        CSSFontFaceRule::create(realm(), CSSFontFaceDescriptors::create(realm(), {})),
        m_family,
        m_cached_weight_range,
        m_cached_slope,
        m_cached_width,
        m_urls,
        m_unicode_ranges,
        {},                // FIXME: ascent_override
        {},                // FIXME: descent_override
        {},                // FIXME: line_gap_override
        FontDisplay::Auto, // FIXME: font_display
        {},                // font-named-instance doesn't exist in FontFace
        {},                // font-language-override doesn't exist in FontFace
        {},                // FIXME: feature_settings
        {},                // FIXME: variation_settings
    };
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
    visitor.visit(m_css_font_face_rule);
    visitor.visit(m_font_loader);
    for (auto const& font_face_set : m_containing_sets)
        visitor.visit(font_face_set);
}

GC::Ref<WebIDL::Promise> FontFace::loaded() const
{
    return m_font_status_promise;
}

void FontFace::reject_status_promise(JS::Value reason)
{
    if (m_status != Bindings::FontFaceLoadStatus::Error) {
        WebIDL::reject_promise(realm(), m_font_status_promise, reason);
        WebIDL::mark_promise_as_handled(m_font_status_promise);
        m_status = Bindings::FontFaceLoadStatus::Error;
    }
}

Optional<FontComputer&> FontFace::font_computer() const
{
    for (auto& font_face_set : m_containing_sets) {
        auto& global = HTML::relevant_global_object(font_face_set);
        if (auto* window = as_if<HTML::Window>(global))
            return window->associated_document().font_computer();
    }
    return {};
}

void FontFace::disconnect_from_css_rule()
{
    m_css_font_face_rule = nullptr;
}

RefPtr<Gfx::FontCascadeList const> FontFace::font_with_point_size(float point_size, Gfx::FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features) const
{
    auto font_list = Gfx::FontCascadeList::create();
    if (m_font_loader) {
        if (auto font = m_font_loader->font_with_point_size(point_size, variations, shape_features))
            font_list->add(*font, m_font_loader->unicode_ranges());
    } else if (m_parsed_font) {
        font_list->add(m_parsed_font->font(point_size, variations, shape_features), m_unicode_ranges);
    }
    if (font_list->is_empty())
        return {};
    return font_list;
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-family
WebIDL::ExceptionOr<void> FontFace::set_family(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontFamily), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.family setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_family(string));

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->unregister_font_face(*this);
    }

    set_family_impl(property.release_nonnull());

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->register_font_face(*this);
    }

    return {};
}

void FontFace::set_family_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_family = string_from_style_value(value).to_string();
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-style
WebIDL::ExceptionOr<void> FontFace::set_style(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontStyle), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.style setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_style(string));

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->unregister_font_face(*this);
    }

    set_style_impl(property.release_nonnull());

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->register_font_face(*this);
    }

    return {};
}

void FontFace::set_style_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_style = value->to_string(SerializationMode::Normal);
    m_cached_slope = compute_slope(*value);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-weight
WebIDL::ExceptionOr<void> FontFace::set_weight(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontWeight), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.weight setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_weight(string));

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->unregister_font_face(*this);
    }

    set_weight_impl(property.release_nonnull());

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->register_font_face(*this);
    }

    return {};
}

void FontFace::set_weight_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_weight = value->to_string(SerializationMode::Normal);
    m_cached_weight_range = compute_weight_range(*value);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-stretch
WebIDL::ExceptionOr<void> FontFace::set_stretch(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    // NOTE: font-stretch is now an alias for font-width
    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontWidth), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.stretch setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_width(string));

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->unregister_font_face(*this);
    }

    set_stretch_impl(property.release_nonnull());

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->register_font_face(*this);
    }

    return {};
}

void FontFace::set_stretch_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_stretch = value->to_string(SerializationMode::Normal);
    m_cached_width = compute_width(*value);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-unicoderange
WebIDL::ExceptionOr<void> FontFace::set_unicode_range(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::UnicodeRange), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.unicodeRange setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_unicode_range(string));

    set_unicode_range_impl(property.release_nonnull());

    if (should_be_registered_with_font_computer()) {
        if (auto font_computer = this->font_computer(); font_computer.has_value())
            font_computer->did_load_font(FlyString(m_family));
    }

    return {};
}

void FontFace::set_unicode_range_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_unicode_range = value->to_string(SerializationMode::Normal);
    auto const& ranges = value->as_value_list().values();
    m_unicode_ranges.clear_with_capacity();
    m_unicode_ranges.ensure_capacity(ranges.size());
    for (auto const& range : ranges)
        m_unicode_ranges.unchecked_append(range->as_unicode_range().unicode_range());
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-featuresettings
WebIDL::ExceptionOr<void> FontFace::set_feature_settings(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontFeatureSettings), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.featureSettings setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_feature_settings(string));

    set_feature_settings_impl(property.release_nonnull());

    return {};
}

void FontFace::set_feature_settings_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_feature_settings = value->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-variationsettings
WebIDL::ExceptionOr<void> FontFace::set_variation_settings(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontVariationSettings), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.variationSettings setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_variation_settings(string));

    set_variation_settings_impl(property.release_nonnull());

    return {};
}

void FontFace::set_variation_settings_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_variation_settings = value->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-display
WebIDL::ExceptionOr<void> FontFace::set_display(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::FontDisplay), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.display setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_font_display(string));

    set_display_impl(property.release_nonnull());

    return {};
}

void FontFace::set_display_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_display = value->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-ascentoverride
WebIDL::ExceptionOr<void> FontFace::set_ascent_override(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::AscentOverride), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.ascentOverride setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_ascent_override(string));

    set_ascent_override_impl(property.release_nonnull());

    return {};
}

void FontFace::set_ascent_override_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_ascent_override = value->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-descentoverride
WebIDL::ExceptionOr<void> FontFace::set_descent_override(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::DescentOverride), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.descentOverride setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_descent_override(string));

    set_descent_override_impl(property.release_nonnull());

    return {};
}

void FontFace::set_descent_override_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_descent_override = value->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontface-linegapoverride
WebIDL::ExceptionOr<void> FontFace::set_line_gap_override(String const& string)
{
    // On setting, parse the string according to the grammar for the corresponding @font-face descriptor.
    // If it does not match the grammar, throw a SyntaxError; otherwise, set the attribute to the serialization of the
    // parsed value.

    auto property = parse_css_descriptor(Parser::ParsingParams(), AtRuleID::FontFace, DescriptorNameAndID::from_id(DescriptorID::LineGapOverride), string);
    if (!property)
        return WebIDL::SyntaxError::create(realm(), "FontFace.lineGapOverride setter: Invalid descriptor value"_utf16);

    if (m_css_font_face_rule)
        TRY(m_css_font_face_rule->descriptors()->set_line_gap_override(string));

    set_line_gap_override_impl(property.release_nonnull());

    return {};
}

void FontFace::set_line_gap_override_impl(NonnullRefPtr<StyleValue const> const& value)
{
    m_line_gap_override = value->to_string(SerializationMode::Normal);
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
    if (m_css_font_face_rule)
        m_css_font_face_rule->set_loading_state(CSSStyleSheet::LoadingState::Loading);

    // AD-HOC: Switch the containing FontFaceSets to "loading" for URL-backed fonts too, mirroring the step the
    //         constructor performs for BufferSource-backed fonts.
    // Spec issue: https://github.com/w3c/csswg-drafts/issues/13235
    {
        HTML::TemporaryExecutionContext context(realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        for (auto& font_face_set : m_containing_sets) {
            if (font_face_set->loading_fonts().is_empty())
                font_face_set->switch_to_loading();
            font_face_set->loading_fonts().append(*this);
        }
    }

    Web::Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this] {
        // 4. Using the value of font face’s [[Urls]] slot, attempt to load a font as defined in [CSS-FONTS-3],
        //     as if it was the value of a @font-face rule’s src descriptor.

        // 5. When the load operation completes, successfully or not, queue a task to run the following steps synchronously:
        auto on_load = GC::create_function(heap(), [this](RefPtr<Gfx::Typeface const> maybe_typeface) {
            HTML::queue_global_task(HTML::Task::Source::FontLoading, HTML::relevant_global_object(*this), GC::create_function(heap(), [this, maybe_typeface] {
                HTML::TemporaryExecutionContext context(realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                // 1. If the attempt to load fails, reject font face’s [[FontStatusPromise]] with a DOMException whose name
                //    is "NetworkError" and set font face’s status attribute to "error".
                if (!maybe_typeface) {
                    m_status = Bindings::FontFaceLoadStatus::Error;
                    if (m_css_font_face_rule)
                        m_css_font_face_rule->set_loading_state(CSSStyleSheet::LoadingState::Error);
                    WebIDL::reject_promise(realm(), m_font_status_promise, WebIDL::NetworkError::create(realm(), "Failed to load font"_utf16));

                    // For each FontFaceSet font face is in:
                    for (auto& font_face_set : m_containing_sets) {
                        // 1. Add font face to the FontFaceSet’s [[FailedFonts]] list.
                        font_face_set->failed_fonts().append(*this);

                        // 2. Remove font face from the FontFaceSet’s [[LoadingFonts]] list. If font was the last item
                        //    in that list (and so the list is now empty), switch the FontFaceSet to loaded.
                        font_face_set->loading_fonts().remove_all_matching([this](auto const& entry) { return entry == this; });
                        if (font_face_set->loading_fonts().is_empty())
                            font_face_set->switch_to_loaded();
                    }
                }

                // 2. Otherwise, font face now represents the loaded font; fulfill font face’s [[FontStatusPromise]] with font face
                //    and set font face’s status attribute to "loaded".
                else {
                    m_parsed_font = maybe_typeface;
                    m_status = Bindings::FontFaceLoadStatus::Loaded;
                    WebIDL::resolve_promise(realm(), m_font_status_promise, this);
                    if (m_css_font_face_rule)
                        m_css_font_face_rule->set_loading_state(CSSStyleSheet::LoadingState::Loaded);

                    if (auto font_computer = this->font_computer(); font_computer.has_value())
                        font_computer->register_font_face(*this);

                    // For each FontFaceSet font face is in:
                    for (auto& font_face_set : m_containing_sets) {
                        // 1. Add font face to the FontFaceSet’s [[LoadedFonts]] list.
                        font_face_set->loaded_fonts().append(*this);

                        // 2. Remove font face from the FontFaceSet’s [[LoadingFonts]] list. If font was the last item
                        //    in that list (and so the list is now empty), switch the FontFaceSet to loaded.
                        font_face_set->loading_fonts().remove_all_matching([this](auto const& entry) { return entry == this; });
                        if (font_face_set->loading_fonts().is_empty())
                            font_face_set->switch_to_loaded();
                    }
                }

                m_font_loader = nullptr;
            }));
        });

        // FIXME: We should probably put the 'font cache' on the WindowOrWorkerGlobalScope instead of tying it to the document's style computer
        auto& global = HTML::relevant_global_object(*this);
        if (auto* window = as_if<HTML::Window>(global)) {
            auto& font_computer = const_cast<FontComputer&>(window->document()->font_computer());

            if (auto loader = font_computer.load_font_face(parsed_font_face(), move(on_load))) {
                m_font_loader = loader;
                loader->start_loading_next_url();
            }
        } else {
            // FIXME: Don't know how to load fonts in workers! They don't have a StyleComputer
            dbgln("FIXME: Worker font loading not implemented");
        }
    }));

    return font_face.loaded();
}

void FontFace::add_to_set(FontFaceSet& set)
{
    m_containing_sets.set(set);
}

void FontFace::remove_from_set(FontFaceSet& set)
{
    m_containing_sets.remove(set);
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
