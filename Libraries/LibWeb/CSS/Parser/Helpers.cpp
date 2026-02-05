/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/HTML/Window.h>

namespace Web {

GC::Ref<JS::Realm> internal_css_realm()
{
    static GC::Root<JS::Realm> realm;
    static GC::Root<HTML::Window> window;
    static OwnPtr<JS::ExecutionContext> execution_context;
    if (!realm) {
        execution_context = Bindings::create_a_new_javascript_realm(
            Bindings::main_thread_vm(),
            [&](JS::Realm& realm) -> JS::Object* {
                window = HTML::Window::create(realm);
                return window;
            },
            [&](JS::Realm&) -> JS::Object* {
                return window;
            });

        realm = *execution_context->realm;
        auto intrinsics = realm->create<Bindings::Intrinsics>(*realm);
        auto host_defined = make<Bindings::HostDefined>(intrinsics);
        realm->set_host_defined(move(host_defined));
    }
    return *realm;
}

GC::Ref<CSS::CSSStyleSheet> parse_css_stylesheet(CSS::Parser::ParsingParams const& context, StringView css, Optional<::URL::URL> location, GC::Ptr<CSS::MediaList> media_list)
{
    if (css.is_empty()) {
        auto rule_list = CSS::CSSRuleList::create(*context.realm);
        if (!media_list)
            media_list = CSS::MediaList::create(*context.realm, {});
        auto style_sheet = CSS::CSSStyleSheet::create(*context.realm, rule_list, *media_list, location);
        style_sheet->set_source_text({});
        return style_sheet;
    }
    auto style_sheet = CSS::Parser::Parser::create(context, css).parse_as_css_stylesheet(location, move(media_list));
    // FIXME: Avoid this copy
    style_sheet->set_source_text(MUST(String::from_utf8(css)));
    return style_sheet;
}

CSS::Parser::Parser::PropertiesAndCustomProperties parse_css_property_declaration_block(CSS::Parser::ParsingParams const& context, StringView css)
{
    if (css.is_empty())
        return {};
    return CSS::Parser::Parser::create(context, css).parse_as_property_declaration_block();
}

Vector<CSS::Descriptor> parse_css_descriptor_declaration_block(CSS::Parser::ParsingParams const& parsing_params, CSS::AtRuleID at_rule_id, StringView css)
{
    if (css.is_empty())
        return {};
    return CSS::Parser::Parser::create(parsing_params, css).parse_as_descriptor_declaration_block(at_rule_id);
}

RefPtr<CSS::StyleValue const> parse_css_value(CSS::Parser::ParsingParams const& context, StringView string, CSS::PropertyID property_id)
{
    if (string.is_empty())
        return nullptr;
    return CSS::Parser::Parser::create(context, string).parse_as_css_value(property_id);
}

RefPtr<CSS::StyleValue const> parse_css_type(CSS::Parser::ParsingParams const& context, StringView string, CSS::ValueType value_type)
{
    if (string.is_empty())
        return nullptr;
    return CSS::Parser::Parser::create(context, string).parse_as_type(value_type);
}

RefPtr<CSS::StyleValue const> parse_css_descriptor(CSS::Parser::ParsingParams const& parsing_params, CSS::AtRuleID at_rule_id, CSS::DescriptorID descriptor_id, StringView string)
{
    if (string.is_empty())
        return nullptr;
    return CSS::Parser::Parser::create(parsing_params, string).parse_as_descriptor_value(at_rule_id, descriptor_id);
}

CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingParams const& context, StringView css_text)
{
    return CSS::Parser::Parser::create(context, css_text).parse_as_css_rule();
}

Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    return CSS::Parser::Parser::create(context, selector_text).parse_as_selector();
}

Optional<CSS::SelectorList> parse_selector_for_nested_style_rule(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    auto parser = CSS::Parser::Parser::create(context, selector_text);

    auto maybe_selectors = parser.parse_as_relative_selector(CSS::Parser::Parser::SelectorParsingMode::Standard);
    if (!maybe_selectors.has_value())
        return {};

    return adapt_nested_relative_selector_list(*maybe_selectors);
}

Optional<CSS::PageSelectorList> parse_page_selector_list(CSS::Parser::ParsingParams const& params, StringView selector_text)
{
    return CSS::Parser::Parser::create(params, selector_text).parse_as_page_selector_list();
}

Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    return CSS::Parser::Parser::create(context, selector_text).parse_as_pseudo_element_selector();
}

RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingParams const& context, StringView string)
{
    return CSS::Parser::Parser::create(context, string).parse_as_media_query();
}

Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingParams const& context, StringView string)
{
    return CSS::Parser::Parser::create(context, string).parse_as_media_query_list();
}

RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingParams const& context, StringView string)
{
    if (string.is_empty())
        return {};
    return CSS::Parser::Parser::create(context, string).parse_as_supports();
}

Vector<CSS::Parser::ComponentValue> parse_component_values_list(CSS::Parser::ParsingParams const& parsing_params, StringView string)
{
    return CSS::Parser::Parser::create(parsing_params, string).parse_as_list_of_component_values();
}

// https://drafts.csswg.org/css-syntax/#css-decode-bytes
ErrorOr<String> css_decode_bytes(Optional<StringView> const& environment_encoding, Optional<String> mime_type_charset, ByteBuffer const& encoded_string)
{
    // https://drafts.csswg.org/css-syntax/#determine-the-fallback-encoding
    auto determine_the_fallback_encoding = [&mime_type_charset, &environment_encoding, &encoded_string]() -> StringView {
        // 1. If HTTP or equivalent protocol provides an encoding label (e.g. via the charset parameter of the Content-Type header) for the stylesheet,
        //    get an encoding from encoding label. If that does not return failure, return it.
        if (mime_type_charset.has_value()) {
            if (auto encoding = TextCodec::get_standardized_encoding(mime_type_charset.value()); encoding.has_value())
                return encoding.value();
        }
        // 2. Otherwise, check stylesheet’s byte stream. If the first 1024 bytes of the stream begin with the hex sequence
        // 40 63 68 61 72 73 65 74 20 22 XX* 22 3B
        // where each XX byte is a value between 0x16 and 0x21 inclusive or a value between 0x23 and 0x7F inclusive,
        // then get an encoding from a string formed out of the sequence of XX bytes, interpreted as ASCII.
        auto check_stylesheets_byte_stream = [&encoded_string]() -> Optional<StringView> {
            size_t scan_length = min(encoded_string.size(), 1024);
            auto pattern_start = "@charset \""sv;
            auto pattern_end = "\";"sv;

            if (scan_length < pattern_start.length())
                return {};

            StringView buffer_view = encoded_string.bytes().slice(0, scan_length);
            if (!buffer_view.starts_with(pattern_start))
                return {};

            auto encoding_start = pattern_start.length();
            auto end_index = buffer_view.find(pattern_end, encoding_start);
            if (!end_index.has_value())
                return {};

            size_t encoding_length = end_index.value() - encoding_start;
            auto encoding_view = buffer_view.substring_view(encoding_start, encoding_length);

            for (char c : encoding_view) {
                u8 byte = static_cast<u8>(c);
                if ((byte < 0x01 || byte > 0x21) && (byte < 0x23 || byte > 0x7F)) {
                    return {};
                }
            }

            return TextCodec::get_standardized_encoding(encoding_view);
        };
        // If the return value was utf-16be or utf-16le, return utf-8; if it was anything else except failure, return it.
        auto byte_stream_value = check_stylesheets_byte_stream();
        if (byte_stream_value.has_value() && (byte_stream_value == "UTF-16BE"sv || byte_stream_value == "UTF-16LE"))
            return "utf-8"sv;
        if (byte_stream_value.has_value())
            return byte_stream_value.value();

        // 3. Otherwise, if an environment encoding is provided by the referring document, return it.
        if (environment_encoding.has_value())
            return environment_encoding.value();

        // 4. Otherwise, return utf-8.
        return "utf-8"sv;
    };

    // 1. Determine the fallback encoding of stylesheet, and let fallback be the result.
    auto fallback = determine_the_fallback_encoding();
    auto decoder = TextCodec::decoder_for(fallback);
    if (!decoder.has_value()) {
        // If we don't support the encoding yet, let's error out instead of trying to decode it as something it's most likely not.
        dbgln("FIXME: Style sheet encoding '{}' is not supported yet", fallback);
        return Error::from_string_literal("No Decoder found");
    }
    // 2. Decode stylesheet’s stream of bytes with fallback encoding fallback, and return the result.
    return TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, encoded_string);
}

}
