/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS::Parser {

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_descriptor_value(AtRuleID at_rule_id, DescriptorID descriptor_id, TokenStream<ComponentValue>& unprocessed_tokens)
{
    if (!at_rule_supports_descriptor(at_rule_id, descriptor_id)) {
        ErrorReporter::the().report(UnknownPropertyError {
            .rule_name = to_string(at_rule_id),
            .property_name = to_string(descriptor_id),
        });
        return ParseError::SyntaxError;
    }

    auto context_guard = push_temporary_value_parsing_context(DescriptorContext { at_rule_id, descriptor_id });

    Vector<ComponentValue> component_values;
    while (unprocessed_tokens.has_next_token()) {
        if (unprocessed_tokens.peek_token().is(Token::Type::Semicolon))
            break;

        auto const& token = unprocessed_tokens.consume_a_token();
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
            [&](PropertyID property_id) -> RefPtr<StyleValue const> {
                auto value_or_error = parse_css_value(property_id, tokens);
                if (value_or_error.is_error())
                    return nullptr;
                auto value_for_property = value_or_error.release_value();
                // Descriptors don't accept the following, which properties do:
                // - CSS-wide keywords
                // - Arbitrary substitution functions (so, UnresolvedStyleValue)
                if (value_for_property->is_css_wide_keyword() || value_for_property->is_unresolved())
                    return nullptr;
                return value_for_property;
            },
            [&](DescriptorMetadata::ValueType value_type) -> RefPtr<StyleValue const> {
                switch (value_type) {
                case DescriptorMetadata::ValueType::CropOrCross: {
                    // crop || cross
                    auto first = parse_keyword_value(tokens);
                    tokens.discard_whitespace();
                    auto second = parse_keyword_value(tokens);

                    if (!first)
                        return nullptr;

                    RefPtr<StyleValue const> crop;
                    RefPtr<StyleValue const> cross;

                    if (first->to_keyword() == Keyword::Crop)
                        crop = first;
                    else if (first->to_keyword() == Keyword::Cross)
                        cross = first;
                    else
                        return nullptr;

                    if (!second)
                        return first.release_nonnull();

                    if (crop.is_null() && second->to_keyword() == Keyword::Crop)
                        crop = second.release_nonnull();
                    else if (cross.is_null() && second->to_keyword() == Keyword::Cross)
                        cross = second.release_nonnull();
                    else
                        return nullptr;

                    return StyleValueList::create(StyleValueVector { crop.release_nonnull(), cross.release_nonnull() }, StyleValueList::Separator::Space);
                }
                case DescriptorMetadata::ValueType::FamilyName:
                    return parse_family_name_value(tokens);
                case DescriptorMetadata::ValueType::FontSrcList: {
                    // "If a component value is parsed correctly and is of a font format or font tech that the UA
                    // supports, add it to the list of supported sources. If parsing a component value results in a
                    // parsing error or its format or tech are unsupported, do not add it to the list of supported
                    // sources.
                    // If there are no supported entries at the end of this process, the value for the src descriptor
                    // is a parse error.
                    // These parsing rules allow for graceful fallback of fonts for user agents which don’t support a
                    // particular font tech or font format."
                    // https://drafts.csswg.org/css-fonts-4/#font-face-src-parsing
                    auto source_lists = parse_a_comma_separated_list_of_component_values(tokens);
                    StyleValueVector valid_sources;
                    for (auto const& source_list : source_lists) {
                        TokenStream source_tokens { source_list };
                        if (auto font_source = parse_font_source_value(source_tokens); font_source && !source_tokens.has_next_token())
                            valid_sources.append(font_source.release_nonnull());
                    }
                    if (valid_sources.is_empty())
                        return nullptr;
                    return StyleValueList::create(move(valid_sources), StyleValueList::Separator::Comma);
                }
                case DescriptorMetadata::ValueType::Length:
                    return parse_length_value(tokens);
                case DescriptorMetadata::ValueType::OptionalDeclarationValue: {
                    // `component_values` already has what we want. Just skip through its tokens so code below knows we consumed them.
                    while (tokens.has_next_token())
                        tokens.discard_a_token();
                    return UnresolvedStyleValue::create(move(component_values));
                }
                case DescriptorMetadata::ValueType::PageSize: {
                    // https://drafts.csswg.org/css-page-3/#page-size-prop
                    // <length [0,∞]>{1,2} | auto | [ <page-size> || [ portrait | landscape ] ]

                    // auto
                    if (auto value = parse_all_as_single_keyword_value(tokens, Keyword::Auto))
                        return value.release_nonnull();

                    // <length [0,∞]>{1,2}
                    if (auto first_length = parse_length_value(tokens)) {
                        if (first_length->is_length() && first_length->as_length().raw_value() < 0)
                            return nullptr;

                        tokens.discard_whitespace();

                        if (auto second_length = parse_length_value(tokens)) {
                            if (second_length->is_length() && second_length->as_length().raw_value() < 0)
                                return nullptr;

                            return StyleValueList::create(StyleValueVector { first_length.release_nonnull(), second_length.release_nonnull() }, StyleValueList::Separator::Space);
                        }

                        return first_length.release_nonnull();
                    }

                    // [ <page-size> || [ portrait | landscape ] ]
                    RefPtr<StyleValue const> page_size;
                    RefPtr<StyleValue const> orientation;
                    if (auto first_keyword = parse_keyword_value(tokens)) {
                        if (first_is_one_of(first_keyword->to_keyword(), Keyword::Landscape, Keyword::Portrait)) {
                            orientation = first_keyword.release_nonnull();
                        } else if (keyword_to_page_size(first_keyword->to_keyword()).has_value()) {
                            page_size = first_keyword.release_nonnull();
                        } else {
                            return nullptr;
                        }
                    } else {
                        return nullptr;
                    }

                    tokens.discard_whitespace();

                    if (auto second_keyword = parse_keyword_value(tokens)) {
                        if (orientation.is_null() && first_is_one_of(second_keyword->to_keyword(), Keyword::Landscape, Keyword::Portrait)) {
                            orientation = second_keyword.release_nonnull();
                        } else if (page_size.is_null() && keyword_to_page_size(second_keyword->to_keyword()).has_value()) {
                            page_size = second_keyword.release_nonnull();
                        } else {
                            return nullptr;
                        }

                        // Portrait is considered the default orientation, so don't include it.
                        if (orientation->to_keyword() == Keyword::Portrait)
                            return page_size.release_nonnull();

                        return StyleValueList::create(StyleValueVector { page_size.release_nonnull(), orientation.release_nonnull() }, StyleValueList::Separator::Space);
                    }

                    return page_size ? page_size.release_nonnull() : orientation.release_nonnull();
                }
                case DescriptorMetadata::ValueType::PositivePercentage: {
                    if (auto percentage_value = parse_percentage_value(tokens)) {
                        if (percentage_value->is_percentage()) {
                            if (percentage_value->as_percentage().raw_value() < 0)
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
                }
                case DescriptorMetadata::ValueType::String:
                    return parse_string_value(tokens);
                case DescriptorMetadata::ValueType::UnicodeRangeTokens: {
                    return parse_comma_separated_value_list(tokens, [this](auto& tokens) -> RefPtr<StyleValue const> {
                        return parse_unicode_range_value(tokens);
                    });
                }
                }
                return nullptr;
            });
        if (!parsed_style_value || tokens.has_next_token())
            continue;
        transaction.commit();
        return parsed_style_value.release_nonnull();
    }

    ErrorReporter::the().report(InvalidPropertyError {
        .rule_name = to_string(at_rule_id),
        .property_name = to_string(descriptor_id),
        .value_string = tokens.dump_string(),
        .description = "Failed to parse."_string,
    });

    return ParseError::SyntaxError;
}

Optional<Descriptor> Parser::convert_to_descriptor(AtRuleID at_rule_id, Declaration const& declaration)
{
    auto descriptor_id = descriptor_id_from_string(at_rule_id, declaration.name);
    if (!descriptor_id.has_value())
        return {};

    auto value_token_stream = TokenStream(declaration.value);
    auto value = parse_descriptor_value(at_rule_id, descriptor_id.value(), value_token_stream);
    if (value.is_error())
        return {};

    return Descriptor { *descriptor_id, value.release_value() };
}

}
