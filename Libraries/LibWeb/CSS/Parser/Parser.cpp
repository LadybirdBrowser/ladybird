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

#include <LibURL/Parser.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSFunctionDeclarations.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/StyleValueRustFFI.h>
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

Parser Parser::create(ParsingParams const& context, StringView input)
{
    return Parser { context, Utf16String::from_utf8(input) };
}

Parser Parser::create(ParsingParams const& context, Utf16View input)
{
    return Parser { context, Utf16String::from_utf16(input) };
}

Parser::Parser(ParsingParams const& context, Utf16String source)
    : m_document(context.document)
    , m_parsing_mode(context.mode)
    , m_is_ua_style_sheet(context.is_ua_style_sheet)
    , m_source(move(source))
    , m_value_context(move(context.value_context))
    , m_rule_context(move(context.rule_context))
    , m_declared_namespaces(move(context.declared_namespaces))
{
}

GC::RootVector<GC::Ref<CSSRule>> Parser::convert_rules(Vector<Rule> const& raw_rules)
{
    GC::RootVector<GC::Ref<CSSRule>> rules;
    for (auto const& raw_rule : raw_rules) {
        if (auto rule = convert_to_rule<CSSNestedDeclarations>(raw_rule, Nested::No))
            rules.append(*rule);
    }
    return rules;
}

GC::RootVector<GC::Ref<CSSRule>> Parser::parse_as_stylesheet_contents()
{
    auto rules = RustSyntaxParser::parse_stylesheet(*this);
    return convert_rules(rules);
}

// https://drafts.csswg.org/css-syntax/#parse-a-css-stylesheet
GC::Ref<CSS::CSSStyleSheet> Parser::parse_as_css_stylesheet(Optional<::URL::URL> location, GC::Ptr<MediaList> media_list)
{
    // To parse a CSS stylesheet, first parse a stylesheet.
    auto rules = RustSyntaxParser::parse_stylesheet(*this);
    auto rule_list = CSSRuleList::create(convert_rules(rules));
    if (!media_list)
        media_list = MediaList::create({});
    return CSSStyleSheet::create(rule_list, *media_list, move(location));
}

CSSRule* Parser::parse_as_css_rule(bool nested)
{
    auto nested_mode = nested ? Nested::Yes : Nested::No;
    auto rule = RustSyntaxParser::parse_rule(*this, m_rule_context, nested ? RuleNesting::Yes : RuleNesting::No);
    if (!rule.has_value())
        return {};
    return convert_to_rule<CSSNestedDeclarations>(*rule, nested_mode).ptr();
}

GC::Ptr<CSSKeyframeRule> Parser::parse_as_keyframe_rule()
{
    m_rule_context.append(RuleContext::AtKeyframes);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtKeyframes);
    };

    auto items = RustSyntaxParser::parse_block_contents(*this, m_rule_context);
    if (items.size() != 1 || !items.first().has<Rule>())
        return {};
    auto const& rule = items.first().get<Rule>();
    if (!rule.has<QualifiedRule>())
        return {};
    auto const& qualified_rule = rule.get<QualifiedRule>();
    if (qualified_rule.kind == ValueParserFFI::FfiRuleKind::Invalid)
        return {};
    VERIFY(qualified_rule.kind == ValueParserFFI::FfiRuleKind::Qualified);
    return convert_to_keyframe_rule(qualified_rule);
}

Vector<Percentage> Parser::parse_as_keyframe_selectors()
{
    auto prelude = RustSyntaxParser::parse_keyframe_selectors(*this);
    if (prelude.kind != ParsedRulePreludeKind::KeyframeSelectors)
        return {};
    Vector<Percentage> selectors;
    selectors.ensure_capacity(prelude.items.size());
    for (auto const& item : prelude.items)
        selectors.unchecked_append(Percentage { item.number_value });
    return selectors;
}

// https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
Parser::PropertiesAndCustomProperties Parser::parse_as_property_declaration_block()
{
    auto expand_shorthands = [&](Vector<StyleProperty>& properties) -> Vector<StyleProperty> {
        Vector<StyleProperty> expanded_properties;
        for (auto& property : properties) {
            if (property_is_shorthand(property.property_id)) {
                StyleComputer::for_each_property_expanding_shorthands(property.property_id, *property.value, [&](PropertyID longhand_property_id, StyleValue const& longhand_value) {
                    expanded_properties.append(CSS::StyleProperty {
                        .important = property.important,
                        .property_id = longhand_property_id,
                        .value = longhand_value,
                    });
                });
            } else {
                expanded_properties.append(property);
            }
        }
        return expanded_properties;
    };

    // 1. Let declarations be the returned declarations from invoking parse a block’s contents with string.
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context, PreservePropertySourceText::Yes);
    // 2. Let parsed declarations be a new empty list.
    PropertiesAndCustomProperties parsed_declarations;

    // 3. For each item declaration in declarations, follow these substeps:
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (rule_or_list.has<Rule>())
            continue;

        auto& rule_declarations = rule_or_list.get<Vector<Declaration>>();
        for (auto const& declaration : rule_declarations) {
            // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
            //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
            //    parsed declaration be null.
            // 2. If parsed declaration is not null, append it to parsed declarations.
            extract_property(declaration, parsed_declarations);
        }
    }
    parsed_declarations.properties = expand_shorthands(parsed_declarations.properties);

    // 4. Return parsed declarations.
    return parsed_declarations;
}

Vector<DevToolsStyleDeclaration> Parser::parse_as_devtools_property_declaration_block()
{
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context, PreservePropertySourceText::Yes);

    Vector<DevToolsStyleDeclaration> parsed_declarations;
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (auto* rule_declarations = rule_or_list.get_pointer<Vector<Declaration>>()) {
            for (auto const& declaration : *rule_declarations) {
                VERIFY(declaration.name.has_value());
                VERIFY(declaration.value_text.has_value());

                parsed_declarations.append(DevToolsStyleDeclaration {
                    .name = *declaration.name,
                    .value = *declaration.value_text,
                    .important = declaration.important,
                    .is_custom_property = declaration.parsed_property_id == PropertyID::Custom,
                    .is_name_valid = declaration.rejection == ValueParserFFI::FfiDeclarationRejection::None || declaration.rejection == ValueParserFFI::FfiDeclarationRejection::InvalidValue,
                    .is_valid = declaration.property.has_value(),
                });
            }
        }
    }

    return parsed_declarations;
}

Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const& parsing_params, StringView declaration_block)
{
    auto devtools_parsing_params = parsing_params;
    if (devtools_parsing_params.rule_context.is_empty())
        devtools_parsing_params.rule_context.append(RuleContext::Style);

    auto parser = Parser::create(devtools_parsing_params, declaration_block);
    return parser.parse_as_devtools_property_declaration_block();
}

Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const& parsing_params, Utf16View declaration_block)
{
    auto devtools_parsing_params = parsing_params;
    if (devtools_parsing_params.rule_context.is_empty())
        devtools_parsing_params.rule_context.append(RuleContext::Style);

    auto parser = Parser::create(devtools_parsing_params, declaration_block);
    return parser.parse_as_devtools_property_declaration_block();
}

// https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
Vector<Descriptor> Parser::parse_as_descriptor_declaration_block(AtRuleID at_rule_id)
{
    auto context_type = [at_rule_id] {
        switch (at_rule_id) {
        case AtRuleID::FontFace:
            return RuleContext::AtFontFace;
        case AtRuleID::Function:
            return RuleContext::AtFunction;
        case AtRuleID::Page:
            return RuleContext::AtPage;
        case AtRuleID::Property:
            return RuleContext::AtProperty;
        case AtRuleID::CounterStyle:
            // NB: We don't actually have a `CSSDescriptors` for `@counter-style` so this function shouldn't ever be
            //     called with `AtRuleID::CounterStyle`.
            VERIFY_NOT_REACHED();
        }
        VERIFY_NOT_REACHED();
    }();

    // 1. Let declarations be the returned declarations from invoking parse a block’s contents with string.
    m_rule_context.append(context_type);
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context);
    m_rule_context.take_last();

    // 2. Let parsed declarations be a new empty list.
    Vector<Descriptor> parsed_declarations;

    // 3. For each item declaration in declarations, follow these substeps:
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (rule_or_list.has<Rule>())
            continue;

        auto& rule_declarations = rule_or_list.get<Vector<Declaration>>();
        for (auto const& declaration : rule_declarations) {
            // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
            //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
            //    parsed declaration be null.
            // 2. If parsed declaration is not null, append it to parsed declarations.
            if (declaration.descriptor_name_and_id.has_value() && declaration.parsed_value)
                parsed_declarations.append({ declaration.descriptor_name_and_id.value(), NonnullRefPtr { *declaration.parsed_value } });
        }
    }

    // 4. Return parsed declarations.
    return parsed_declarations;
}

void Parser::extract_property(Declaration const& declaration, PropertiesAndCustomProperties& dest)
{
    if (!declaration.property.has_value())
        return;
    auto property = declaration.property->property;
    if (property.property_id == PropertyID::Custom)
        dest.custom_properties.set(declaration.property->name, property);
    else
        dest.properties.append(move(property));
}

GC::Ref<CSSStyleProperties> Parser::convert_to_style_declaration(Vector<Declaration> const& declarations)
{
    PropertiesAndCustomProperties properties;
    PropertiesAndCustomProperties& dest = properties;
    for (auto const& declaration : declarations) {
        extract_property(declaration, dest);
    }
    return CSSStyleProperties::create(move(properties.properties), move(properties.custom_properties));
}

RefPtr<StyleValue const> Parser::parse_as_css_value(PropertyID property_id)
{
    auto parsed_value = parse_css_value_from_source(property_id, m_source);
    if (parsed_value.is_error())
        return nullptr;
    return parsed_value.release_value();
}

RefPtr<StyleValue const> Parser::parse_css_value_from_source(ParsingParams const& context, Utf16View source, PropertyID property_id)
{
    Parser parser { context, {} };
    auto parsed_value = parser.parse_css_value_from_source(property_id, source);
    if (parsed_value.is_error())
        return nullptr;
    return parsed_value.release_value();
}

RefPtr<StyleValue const> Parser::parse_as_descriptor_value(AtRuleID at_rule_id, DescriptorNameAndID const& descriptor_name_and_id)
{
    return RustSyntaxParser::parse_descriptor(*this, at_rule_id, descriptor_name_and_id);
}

RefPtr<StyleValue const> Parser::parse_as_type(ValueType value_type)
{
    return parse_primitive_value_from_source(value_type, m_source);
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
NonnullRefPtr<StyleValue const> Parser::parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img)
{
    if (!element.has_attribute(HTML::AttributeNames::sizes))
        return LengthStyleValue::create(Length(100, LengthUnit::Vw));
    auto auto_width = sizes_attribute_auto_width(img);
    auto context = make_parse_context(ParseContextMode::Value);
    Optional<MediaEnvironmentSnapshot> media_environment;
    if (m_document)
        media_environment.emplace(*m_document);
    auto ffi_environment = media_environment.map([](auto const& environment) { return environment.ffi_environment(); });
    auto const* parsed = ValueParserFFI::rust_parse_sizes_attribute(ffi_utf16_view(m_source), &context.context, ffi_environment.has_value() ? &*ffi_environment : nullptr, auto_width.has_value() ? &*auto_width : nullptr);
    VERIFY(parsed);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

DOM::Document const* Parser::document() const
{
    return m_document.ptr();
}

HTML::Window const* Parser::window() const
{
    if (!m_document)
        return nullptr;
    return m_document->window().ptr();
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
