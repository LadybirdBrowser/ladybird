/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tommy van der Vorst <tommy@pixelspark.nl>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

ParsingParams::ParsingParams(ParsingMode mode)
    : mode(mode)
{
}

ParsingParams::ParsingParams(ValueParsingContext value_context)
    : value_context(Vector { move(value_context) })
{
}

ParsingParams::ParsingParams(IsUAStyleSheet is_ua_style_sheet)
    : is_ua_style_sheet(is_ua_style_sheet)
{
}

ParsingParams::ParsingParams(DOM::Document const& document, ParsingMode mode)
    : document(&document)
    , mode(mode)
{
}

Parser::Parser(ParsingParams context)
    : m_document(context.document)
    , m_parsing_mode(context.mode)
    , m_is_ua_style_sheet(context.is_ua_style_sheet)
    , m_value_context(move(context.value_context))
    , m_rule_context(move(context.rule_context))
    , m_declared_namespaces(move(context.declared_namespaces))
{
}

RustRuleList Parser::parse_as_stylesheet_contents(Utf16View source)
{
    auto parse = parse_stylesheet(source);
    return parse.native_rules();
}

// https://drafts.csswg.org/css-syntax/#parse-a-css-stylesheet
NonnullRefPtr<CSS::StyleSheetState> Parser::parse_as_css_stylesheet(Utf16View source, Optional<::URL::URL> location, RustMediaList media_list)
{
    // To parse a CSS stylesheet, first parse a stylesheet.
    auto parse = parse_stylesheet(source);
    return create_css_stylesheet(parse, move(location), move(media_list));
}

NonnullRefPtr<StyleSheetState> Parser::create_css_stylesheet(RustStyleSheetParse const& parse, Optional<::URL::URL> location, RustMediaList media_list)
{
    auto rules = parse.native_rules();
    return StyleSheetState::create(move(rules), const_cast<DOM::Document*>(m_document.ptr()), move(media_list), move(location));
}

Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const& parsing_params, StringView declaration_block)
{
    return parse_css_declaration_block_for_devtools(parsing_params, Utf16String::from_utf8(declaration_block));
}

Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const& parsing_params, Utf16View declaration_block)
{
    auto devtools_parsing_params = parsing_params;
    if (devtools_parsing_params.rule_context.is_empty())
        devtools_parsing_params.rule_context.append(RuleContext::Style);

    Parser parser { move(devtools_parsing_params) };
    return parser.parse_as_devtools_property_declaration_block(declaration_block);
}

RefPtr<StyleValue const> Parser::parse_as_css_value(Utf16View source, PropertyID property_id)
{
    auto parsed_value = parse_css_value_from_source(property_id, source);
    if (parsed_value.is_error())
        return nullptr;
    return parsed_value.release_value();
}

// https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute
static Optional<double> sizes_attribute_auto_width(HTML::HTMLImageElement const* img)
{
    // FIXME: "img is being rendered" - we just see if it has image data for now.
    if (!img || !img->is_image_available() || !img->allows_auto_sizes())
        return {};

    // FIXME: The spec doesn't seem to tell us how to determine the concrete size of an <img>, so use the default sizing algorithm.
    //        Should this use some of the methods from FormattingContext?
    auto concrete_size = run_default_sizing_algorithm(
        img->width(), img->height(),
        { img->natural_width(), img->natural_height(), img->intrinsic_aspect_ratio() },
        // NOTE: https://html.spec.whatwg.org/multipage/rendering.html#img-contain-size
        CSSPixelSize { 300, 150 });
    return concrete_size.width().to_double();
}

// AD-HOC: If element has no sizes attribute, this algorithm always logs a parse error and then returns 100vw.
//         The attribute is optional, so avoid spamming the debug log with false positives by just returning early.
NonnullRefPtr<StyleValue const> Parser::parse_as_sizes_attribute(Utf16View source, DOM::Element const& element, HTML::HTMLImageElement const* img)
{
    if (!element.has_attribute(HTML::AttributeNames::sizes))
        return LengthStyleValue::create(Length(100, LengthUnit::Vw));
    auto auto_width = sizes_attribute_auto_width(img);
    auto context = make_parse_context(ParseContextMode::Value);
    Optional<MediaEnvironmentSnapshot> media_environment;
    if (m_document)
        media_environment.emplace(*m_document);
    auto ffi_environment = media_environment.map([](auto const& environment) { return environment.ffi_environment(); });
    auto const* parsed = ValueParserFFI::rust_parse_sizes_attribute(ffi_utf16_view(source), &context.context, ffi_environment.has_value() ? &*ffi_environment : nullptr, auto_width.has_value() ? &*auto_width : nullptr);
    VERIFY(parsed);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

DOM::Document const* Parser::document() const
{
    return m_document.ptr();
}

bool Parser::in_quirks_mode() const
{
    return m_document ? m_document->in_quirks_mode() : false;
}

bool Parser::is_parsing_svg_presentation_attribute() const
{
    return m_parsing_mode == ParsingMode::SVGPresentationAttribute;
}

}
