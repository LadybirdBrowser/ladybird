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

#include <AK/Debug.h>
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
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLImageElement.h>

static void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(CSS_PARSER_DEBUG, "Parse error (CSS) {}", location);
}

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
    bool import_rules_valid = true;
    bool namespace_rules_valid = true;

    // Interpret all of the resulting top-level qualified rules as style rules, defined below.
    GC::RootVector<GC::Ref<CSSRule>> rules;
    for (auto const& raw_rule : raw_rules) {
        auto rule = convert_to_rule<CSSNestedDeclarations>(raw_rule, Nested::No);
        // If any style rule is invalid, or any at-rule is not recognized or is invalid according to its grammar or context, it’s a parse error.
        // Discard that rule.
        if (!rule) {
            log_parse_error();
            continue;
        }

        // "Any @import rules must precede all other valid at-rules and style rules in a style sheet
        // (ignoring @charset and @layer statement rules) and must not have any other valid at-rules
        // or style rules between it and previous @import rules, or else the @import rule is invalid."
        // https://drafts.csswg.org/css-cascade-5/#at-import
        //
        // "Any @namespace rules must follow all @charset and @import rules and precede all other
        // non-ignored at-rules and style rules in a style sheet.
        // ...
        // A syntactically invalid @namespace rule (whether malformed or misplaced) must be ignored."
        // https://drafts.csswg.org/css-namespaces/#syntax
        switch (rule->type()) {
        case CSSRule::Type::LayerStatement:
            break;
        case CSSRule::Type::Import:
            if (!import_rules_valid)
                continue;
            break;
        case CSSRule::Type::Namespace:
            import_rules_valid = false;

            if (!namespace_rules_valid)
                continue;

            m_declared_namespaces.set(as<CSSNamespaceRule>(*rule).prefix());
            break;
        default:
            import_rules_valid = false;
            namespace_rules_valid = false;
            break;
        }

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

Optional<RustQueryHandle> Parser::parse_as_supports()
{
    return RustQueryParser::parse_supports_condition(*this, m_source);
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
    return convert_to_keyframe_rule(rule.get<QualifiedRule>());
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
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context);

    Vector<DevToolsStyleDeclaration> parsed_declarations;
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (auto* rule_declarations = rule_or_list.get_pointer<Vector<Declaration>>()) {
            for (auto const& declaration : *rule_declarations) {
                auto property = PropertyNameAndID::from_name(declaration.name);

                parsed_declarations.append(DevToolsStyleDeclaration {
                    .name = declaration.name,
                    .value = declaration.value_text,
                    .important = declaration.important,
                    .is_custom_property = property.has_value() && property->is_custom_property(),
                    .is_name_valid = property.has_value(),
                    .is_valid = property.has_value() && convert_to_style_property(declaration).has_value(),
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
    if (auto maybe_property_and_name = convert_to_style_property(declaration); maybe_property_and_name.has_value()) {
        auto property = maybe_property_and_name->property;
        if (property.property_id == PropertyID::Custom) {
            dest.custom_properties.set(maybe_property_and_name->name, property);
        } else {
            dest.properties.append(move(property));
        }
    }
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

Optional<StylePropertyAndName> Parser::convert_to_style_property(Declaration const& declaration)
{
    auto property = PropertyNameAndID::from_name(declaration.name);

    if (!property.has_value()) {
        if (has_ignored_vendor_prefix(declaration.name)) {
            return {};
        }
        ErrorReporter::the().report(UnknownPropertyError { .property_name = declaration.name });
        return {};
    }

    RefPtr<StyleValue const> legacy_value;
    if (declaration.name.equals_ignoring_ascii_case("-webkit-box-orient"sv)) {
        // INTEROP: -webkit-box-orient predates flex-direction and uses horizontal and vertical
        //          for the values now represented by row and column, respectively.
        auto value = declaration.value_text.trim_ascii_whitespace();
        if (value.equals_ignoring_ascii_case("horizontal"sv))
            legacy_value = KeywordStyleValue::create(Keyword::Row);
        else if (value.equals_ignoring_ascii_case("vertical"sv))
            legacy_value = KeywordStyleValue::create(Keyword::Column);
    }

    if (declaration.parsed_property_id.has_value()) {
        auto value = legacy_value ? legacy_value : declaration.parsed_value;
        if (!value) {
            ErrorReporter::the().report(InvalidPropertyError {
                .property_name = property->name(),
                .value_string = declaration.value_text.to_utf8(),
                .description = "Failed to parse."_string,
            });
            return {};
        }
        return property->is_custom_property()
            ? StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_nonnull() }, property->name() }
            : StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_nonnull() } };
    }

    auto value = legacy_value
        ? ParseErrorOr<NonnullRefPtr<StyleValue const>> { legacy_value.release_nonnull() }
        : parse_css_value_from_source(property->id(), declaration.original_value_text.value_or(declaration.value_text));
    if (value.is_error())
        return {};
    return property->is_custom_property()
        ? StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_value() }, property->name() }
        : StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_value() } };
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
NonnullRefPtr<StyleValue const> Parser::parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img)
{
    // When asked to parse a sizes attribute from an element element, with an img element or null img:

    // AD-HOC: If element has no sizes attribute, this algorithm always logs a parse error and then returns 100vw.
    //         The attribute is optional, so avoid spamming the debug log with false positives by just returning early.
    if (!element.has_attribute(HTML::AttributeNames::sizes))
        return LengthStyleValue::create(Length(100, LengthUnit::Vw));

    // 1. Let unparsed sizes list be the result of parsing a comma-separated list of component values
    //    from the value of element's sizes attribute (or the empty string, if the attribute is absent).
    auto entries = RustQueryParser::split_sizes_attribute(m_source);
    if (!entries.has_value())
        return LengthStyleValue::create(Length(100, LengthUnit::Vw));

    // 2. Let size be null.

    // 3. For each unparsed size in unparsed sizes list:
    for (size_t index = 0; index < entries->size(); ++index) {
        auto const& entry = (*entries)[index];

        // 1. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        //    If unparsed size is now empty, that is a parse error; continue.
        if (entry.size.is_empty()) {
            log_parse_error();
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "sizes attribute"_utf16_fly_string,
                .value_string = m_source.to_utf8(),
                .description = "Failed in step 3.1; all whitespace"_string,
            });
            continue;
        }

        // 2. If the last component value in unparsed size is a valid non-negative <source-size-value>,
        //    then set size to its value and remove the component value from unparsed size.
        //    Any CSS function other than the math functions is invalid.
        //    Otherwise, there is a parse error; continue.
        auto size = RustQueryParser::parse_source_size_value(*this, entry.size);
        if (!size) {
            log_parse_error();
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "sizes attribute"_utf16_fly_string,
                .value_string = m_source.to_utf8(),
                .description = MUST(String::formatted("Failed in step 3.2; couldn't parse {} as a <source-size-value>", entry.size)),
            });
            continue;
        }

        // 3. If size is auto, and img is not null, and img is being rendered, and img allows auto-sizes,
        //    then set size to the concrete object size width of img, in CSS pixels.
        // FIXME: "img is being rendered" - we just see if it has image data for now
        if (size->has_auto() && img && img->is_image_available() && img->allows_auto_sizes()) {
            // FIXME: The spec doesn't seem to tell us how to determine the concrete size of an <img>, so use the default sizing algorithm.
            //        Should this use some of the methods from FormattingContext?
            auto concrete_size = run_default_sizing_algorithm(
                img->width(), img->height(),
                { img->natural_width(), img->natural_height(), img->intrinsic_aspect_ratio() },
                // NOTE: https://html.spec.whatwg.org/multipage/rendering.html#img-contain-size
                CSSPixelSize { 300, 150 });
            size = LengthStyleValue::create(Length::make_px(concrete_size.width()));
        }

        // 4. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        //    If unparsed size is now empty:
        if (entry.condition.is_empty()) {
            // 1. If this was not the last item in unparsed sizes list, that is a parse error.
            if (index != entries->size() - 1) {
                log_parse_error();
                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "sizes attribute"_utf16_fly_string,
                    .value_string = m_source.to_utf8(),
                    .description = MUST(String::formatted("Failed in step 3.4.1; is unparsed size #{}, count {}", index, entries->size())),
                });
            }
            // 2. If size is not auto, then return size. Otherwise, continue.
            if (!size->has_auto())
                return size.release_nonnull();
            continue;
        }

        // 5. Parse the remaining component values in unparsed size as a <media-condition>.
        //    If it does not parse correctly, or it does parse correctly but the <media-condition> evaluates to false, continue.
        auto media_condition = RustQueryParser::parse_media_condition(*this, entry.condition);
        if (!media_condition.has_value())
            continue;

        // https://drafts.csswg.org/mediaqueries-5/#evaluating
        // "If the result of any of the above productions is used in any
        // context that expects a two-valued boolean, 'unknown' must be
        // converted to 'false'."
        if (!m_document)
            continue;
        if (evaluate_media_condition(*media_condition, MediaEnvironmentSnapshot { *m_document }) != MatchResult::True)
            continue;

        // 5. If size is not auto, then return size. Otherwise, continue.
        if (!size->has_auto())
            return size.release_nonnull();
    }

    // 4. Return 100vw.
    return LengthStyleValue::create(Length(100, LengthUnit::Vw));
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
