/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS::Parser {

Parser::ParseErrorOr<NonnullRefPtr<CSSStyleValue>> Parser::parse_descriptor_value(AtRuleID at_rule_id, DescriptorID descriptor_id, TokenStream<ComponentValue>& unprocessed_tokens)
{
    if (!at_rule_supports_descriptor(at_rule_id, descriptor_id)) {
        dbgln_if(CSS_PARSER_DEBUG, "Unsupported descriptor '{}' in '{}'", to_string(descriptor_id), to_string(at_rule_id));
        return ParseError::SyntaxError;
    }

    auto context_guard = push_temporary_value_parsing_context(DescriptorContext { at_rule_id, descriptor_id });

    Vector<ComponentValue> component_values;
    while (unprocessed_tokens.has_next_token()) {
        if (unprocessed_tokens.peek_token().is(Token::Type::Semicolon))
            break;

        // FIXME: Stop removing whitespace here. It's just for compatibility with the property-parsing code.
        auto const& token = unprocessed_tokens.consume_a_token();
        if (token.is(Token::Type::Whitespace))
            continue;

        component_values.append(token);
    }

    TokenStream tokens { component_values };
    auto metadata = get_descriptor_metadata(at_rule_id, descriptor_id);
    for (auto const& option : metadata.syntax) {
        auto transaction = tokens.begin_transaction();
        auto parsed_style_value = option.visit(
            [&](Keyword keyword) {
                return parse_all_as_single_keyword_value(tokens, keyword);
            },
            [&](PropertyID property_id) -> RefPtr<CSSStyleValue> {
                auto value_for_property = parse_css_value_for_property(property_id, tokens);
                if (!value_for_property)
                    return nullptr;
                // Descriptors don't accept the following, which properties do:
                // - CSS-wide keywords
                // - Shorthands
                // - Arbitrary substitution functions (so, UnresolvedStyleValue)
                if (value_for_property->is_css_wide_keyword() || value_for_property->is_shorthand() || value_for_property->is_unresolved())
                    return nullptr;
                return value_for_property.release_nonnull();
            },
            [&](DescriptorMetadata::ValueType value_type) -> RefPtr<CSSStyleValue> {
                switch (value_type) {
                case DescriptorMetadata::ValueType::FamilyName:
                    return parse_family_name_value(tokens);
                case DescriptorMetadata::ValueType::FontSrcList:
                    return parse_comma_separated_value_list(tokens, [this](auto& tokens) -> RefPtr<CSSStyleValue> {
                        return parse_font_source_value(tokens);
                    });
                case DescriptorMetadata::ValueType::OptionalDeclarationValue: {
                    // FIXME: This is for an @property's initial value. Figure out what this should actually do once we need it.
                    StringBuilder initial_value_sb;
                    while (tokens.has_next_token())
                        initial_value_sb.append(tokens.consume_a_token().to_string());
                    return StringStyleValue::create(initial_value_sb.to_fly_string_without_validation());
                }
                case DescriptorMetadata::ValueType::PositivePercentage:
                    if (auto percentage_value = parse_percentage_value(tokens)) {
                        if (percentage_value->is_percentage()) {
                            if (percentage_value->as_percentage().value() < 0)
                                return nullptr;
                            return percentage_value.release_nonnull();
                        }
                        // All calculations in descriptors must be resolvable at parse-time.
                        if (percentage_value->is_calculated()) {
                            auto percentage = percentage_value->as_calculated().resolve_percentage({});
                            if (percentage.has_value() && percentage->value() >= 0)
                                return PercentageStyleValue::create(percentage.release_value());
                            return nullptr;
                        }
                    }
                    return nullptr;
                case DescriptorMetadata::ValueType::String:
                    return parse_string_value(tokens);
                case DescriptorMetadata::ValueType::UnicodeRangeTokens:
                    return parse_comma_separated_value_list(tokens, [this](auto& tokens) -> RefPtr<CSSStyleValue> {
                        return parse_unicode_range_value(tokens);
                    });
                }
                return nullptr;
            });
        if (!parsed_style_value || tokens.has_next_token())
            continue;
        transaction.commit();
        return parsed_style_value.release_nonnull();
    }

    if constexpr (CSS_PARSER_DEBUG) {
        dbgln("Failed to parse descriptor '{}' in '{}'", to_string(descriptor_id), to_string(at_rule_id));
        tokens.dump_all_tokens();
    }

    return ParseError::SyntaxError;
}

Optional<Descriptor> Parser::convert_to_descriptor(AtRuleID at_rule_id, Declaration const& declaration)
{
    auto descriptor_id = descriptor_id_from_string(at_rule_id, declaration.name);
    if (!descriptor_id.has_value())
        return {};

    auto value_token_stream = TokenStream(declaration.value);
    auto value = parse_descriptor_value(at_rule_id, descriptor_id.value(), value_token_stream);
    if (value.is_error()) {
        if (value.error() == ParseError::SyntaxError) {
            if constexpr (CSS_PARSER_DEBUG) {
                dbgln("Unable to parse value for CSS @{} descriptor '{}'.", to_string(at_rule_id), declaration.name);
                value_token_stream.dump_all_tokens();
            }
        }
        return {};
    }

    return Descriptor { *descriptor_id, value.release_value() };
}

}
