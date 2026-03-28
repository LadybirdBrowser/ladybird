/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS::Parser {

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_descriptor_value(AtRuleID at_rule_id, DescriptorNameAndID const& descriptor_name_and_id, TokenStream<ComponentValue>& tokens)
{
    if (!at_rule_supports_descriptor(at_rule_id, descriptor_name_and_id.id())) {
        ErrorReporter::the().report(UnknownPropertyError {
            .rule_name = to_string(at_rule_id),
            .property_name = descriptor_name_and_id.name(),
        });
        return ParseError::SyntaxError;
    }

    auto context_guard = push_temporary_value_parsing_context(DescriptorContext { at_rule_id, descriptor_name_and_id.id() });

    auto transaction = tokens.begin_transaction();

    auto descriptor_value_start_index = tokens.current_index();
    SubstitutionFunctionsPresence substitution_functions_presence {};

    tokens.mark();
    while (tokens.has_next_token()) {
        auto const& token = tokens.consume_a_token();

        if (token.is(Token::Type::Semicolon))
            return ParseError::SyntaxError;

        if (collect_arbitrary_substitution_function_presence(token, substitution_functions_presence).is_error())
            return ParseError::SyntaxError;
    }

    auto metadata = get_descriptor_metadata(at_rule_id, descriptor_name_and_id.id());

    if (substitution_functions_presence.has_any()) {
        // https://drafts.csswg.org/css-values-5/#resolve-property
        // Unless otherwise specified, arbitrary substitution functions can be used in place of any part of any
        // property’s value (including within other functional notations); and are not valid in any other context.

        // NB: Since we are not in a property value context we only allow ASFs if they are explicitly allowed in
        //     Descriptors.json
        if (!metadata.allow_arbitrary_substitution_functions) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = MUST(String::formatted("{}/{}", to_string(at_rule_id), descriptor_name_and_id.name())),
                .value_string = tokens.dump_string(),
                .description = "ASFs are not supported in this descriptor"_string,
            });
            return ParseError::SyntaxError;
        }

        return UnresolvedStyleValue::create(Vector<ComponentValue> { tokens.tokens_since(descriptor_value_start_index) }, substitution_functions_presence);
    }

    tokens.restore_a_mark();

    Optional<ComputationContext> computation_context = m_document
        ? ComputationContext { .length_resolution_context = Length::ResolutionContext::for_document(*m_document) }
        : Optional<ComputationContext> {};

    for (auto const& option : metadata.syntax) {
        auto syntax_transaction = transaction.create_child();
        auto parsed_style_value = option.visit(
            [&](Keyword keyword) {
                return parse_all_as_single_keyword_value(tokens, keyword);
            },
            [&](PropertyID property_id) -> RefPtr<StyleValue const> {
                auto value_or_error = parse_css_value(property_id, tokens);
                if (value_or_error.is_error())
                    return nullptr;
                auto value_for_property = value_or_error.release_value();
                // Descriptors don't accept the CSS-wide keywords
                if (value_for_property->is_css_wide_keyword())
                    return nullptr;
                return value_for_property;
            },
            [&](DescriptorMetadata::ValueType value_type) -> RefPtr<StyleValue const> {
                switch (value_type) {
                case DescriptorMetadata::ValueType::CounterStyleAdditiveSymbols: {
                    // [ <integer [0,∞]> && <symbol> ]#
                    auto additive_tuples = parse_comma_separated_value_list(tokens, [this](auto& tokens) {
                        return parse_nonnegative_integer_symbol_pair_value(tokens);
                    });

                    if (!additive_tuples)
                        return nullptr;

                    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-symbols
                    // Each entry in the additive-symbols descriptor’s value defines an additive tuple, which consists
                    // of a counter symbol and an integer weight. Each weight must be a non-negative integer, and the
                    // additive tuples must be specified in order of strictly descending weight; otherwise, the
                    // declaration is invalid and must be ignored.
                    i32 previous_weight = NumericLimits<i32>::max();

                    for (auto const& tuple_style_value : additive_tuples->as_value_list().values()) {
                        auto const& weight = tuple_style_value->as_value_list().value_at(0, false);

                        i32 resolved_weight;

                        if (weight->is_integer()) {
                            resolved_weight = weight->as_integer().integer();
                        } else {
                            // FIXME: How should we actually handle calc() when we have no document to absolutize against
                            if (!computation_context.has_value())
                                return nullptr;

                            resolved_weight = weight->absolutized(computation_context.value())->as_calculated().resolve_integer({}).value();
                        }

                        if (resolved_weight >= previous_weight)
                            return nullptr;

                        previous_weight = resolved_weight;
                    }

                    return additive_tuples;
                }
                case DescriptorMetadata::ValueType::CounterStyleSystem: {
                    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-system
                    // cyclic | numeric | alphabetic | symbolic | additive | [fixed <integer>?] | [ extends <counter-style-name> ]
                    auto keyword_value = parse_keyword_value(tokens);

                    if (!keyword_value)
                        return nullptr;

                    if (auto system = keyword_to_counter_style_system(keyword_value->to_keyword()); system.has_value())
                        return CounterStyleSystemStyleValue::create(system.release_value());

                    if (keyword_value->to_keyword() == Keyword::Fixed) {
                        auto integer_value = parse_integer_value(tokens);

                        return CounterStyleSystemStyleValue::create_fixed(integer_value);
                    }

                    if (keyword_value->to_keyword() == Keyword::Extends) {
                        auto counter_style_name = parse_counter_style_name(tokens);

                        if (!counter_style_name.has_value())
                            return nullptr;

                        return CounterStyleSystemStyleValue::create_extends(counter_style_name.release_value());
                    }

                    return nullptr;
                }
                case DescriptorMetadata::ValueType::CounterStyleName: {
                    auto counter_style_name = parse_counter_style_name(tokens);

                    if (!counter_style_name.has_value())
                        return nullptr;

                    return CustomIdentStyleValue::create(counter_style_name.release_value());
                }
                case DescriptorMetadata::ValueType::CounterStyleNegative: {
                    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-negative
                    // <symbol> <symbol>?
                    auto first_symbol = parse_symbol_value(tokens);
                    auto second_symbol = parse_symbol_value(tokens);

                    if (!first_symbol)
                        return nullptr;

                    if (!second_symbol)
                        return StyleValueList::create({ first_symbol.release_nonnull() }, StyleValueList::Separator::Space);

                    return StyleValueList::create({ first_symbol.release_nonnull(), second_symbol.release_nonnull() }, StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
                }
                case DescriptorMetadata::ValueType::CounterStylePad: {
                    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-pad
                    // <integer [0,∞]> && <symbol>
                    return parse_nonnegative_integer_symbol_pair_value(tokens);
                }
                case DescriptorMetadata::ValueType::CounterStyleRange: {
                    // https://drafts.csswg.org/css-counter-styles-3/#counter-style-range
                    // [ [ <integer> | infinite ]{2} ]# | auto
                    if (auto value = parse_all_as_single_keyword_value(tokens, Keyword::Auto))
                        return value;

                    return parse_comma_separated_value_list(tokens, [&](TokenStream<ComponentValue>& tokens) -> RefPtr<StyleValue const> {
                        auto const parse_value = [&]() -> RefPtr<StyleValue const> {
                            if (auto keyword_value = parse_keyword_value(tokens); keyword_value && keyword_value->to_keyword() == Keyword::Infinite)
                                return keyword_value;

                            if (auto integer_value = parse_integer_value(tokens); integer_value)
                                return integer_value;

                            return nullptr;
                        };

                        auto const resolve_value = [&](StyleValue const& value, i32 infinite_value) -> Optional<i32> {
                            if (value.is_integer())
                                return value.as_integer().integer();

                            if (value.is_keyword() && value.as_keyword().to_keyword() == Keyword::Infinite)
                                return infinite_value;

                            // FIXME: How should we actually handle calc() when we have no document to absolutize against
                            if (!computation_context.has_value())
                                return {};

                            return value.absolutized(computation_context.value())->as_calculated().resolve_integer({}).value();
                        };

                        auto first_value = parse_value();
                        auto second_value = parse_value();

                        if (!first_value || !second_value)
                            return nullptr;

                        // If the lower bound of any range is higher than the upper bound, the entire descriptor is
                        // invalid and must be ignored.
                        auto first_int = resolve_value(*first_value, NumericLimits<i32>::min());
                        auto second_int = resolve_value(*second_value, NumericLimits<i32>::max());

                        if (!first_int.has_value() || !second_int.has_value() || first_int.value() > second_int.value())
                            return nullptr;

                        return StyleValueList::create({ first_value.release_nonnull(), second_value.release_nonnull() }, StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
                    });
                }
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
                case DescriptorMetadata::ValueType::FontWeightAbsolutePair: {
                    // <font-weight-absolute>{1,2}
                    // <font-weight-absolute> = [ normal | bold | <number [1,1000]> ]
                    // This is the same as the font-weight property, twice, without 'lighter' or 'bolder'.
                    auto parse_absolute_font_weight = [&] -> RefPtr<StyleValue const> {
                        auto value_for_property = parse_css_value_for_property(PropertyID::FontWeight, tokens);
                        if (!value_for_property)
                            return nullptr;
                        if (value_for_property->is_css_wide_keyword() || value_for_property->is_unresolved())
                            return nullptr;
                        if (first_is_one_of(value_for_property->to_keyword(), Keyword::Lighter, Keyword::Bolder))
                            return nullptr;
                        return value_for_property;
                    };
                    auto first = parse_absolute_font_weight();
                    if (!first)
                        return nullptr;
                    tokens.discard_whitespace();
                    if (!tokens.has_next_token())
                        return StyleValueList::create({ first.release_nonnull() }, StyleValueList::Separator::Space);
                    auto second = parse_absolute_font_weight();
                    if (!second)
                        return nullptr;
                    return StyleValueList::create({ first.release_nonnull(), second.release_nonnull() }, StyleValueList::Separator::Space);
                }
                case DescriptorMetadata::ValueType::Length:
                    return parse_length_value(tokens);
                case DescriptorMetadata::ValueType::OptionalDeclarationValue: {
                    tokens.discard_whitespace();

                    if (tokens.is_empty())
                        return UnresolvedStyleValue::create({}, {});

                    if (auto parsed_declaration_value = parse_declaration_value(tokens); parsed_declaration_value.has_value() && tokens.is_empty()) {
                        // NB: We know this contains no substitution functions otherwise we would have returned earlier
                        return UnresolvedStyleValue::create(parsed_declaration_value.release_value(), {});
                    }

                    return nullptr;
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
                case DescriptorMetadata::ValueType::Symbol:
                    return parse_symbol_value(tokens);
                case DescriptorMetadata::ValueType::Symbols: {
                    // <symbol>+
                    StyleValueVector symbols;
                    while (tokens.has_next_token()) {
                        auto symbol = parse_symbol_value(tokens);
                        if (!symbol)
                            break;
                        symbols.append(symbol.release_nonnull());
                    }

                    if (symbols.is_empty())
                        return nullptr;

                    return StyleValueList::create(move(symbols), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
                }
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
        syntax_transaction.commit();
        return parsed_style_value.release_nonnull();
    }

    ErrorReporter::the().report(InvalidPropertyError {
        .rule_name = to_string(at_rule_id),
        .property_name = descriptor_name_and_id.name(),
        .value_string = tokens.dump_string(),
        .description = "Failed to parse."_string,
    });

    return ParseError::SyntaxError;
}

Optional<Descriptor> Parser::convert_to_descriptor(AtRuleID at_rule_id, Declaration const& declaration)
{
    auto descriptor_name_and_id = DescriptorNameAndID::from_name(at_rule_id, declaration.name);
    if (!descriptor_name_and_id.has_value())
        return {};

    auto value_token_stream = TokenStream(declaration.value);
    auto value = parse_descriptor_value(at_rule_id, descriptor_name_and_id.value(), value_token_stream);
    if (value.is_error())
        return {};

    return Descriptor { descriptor_name_and_id.value(), value.release_value() };
}

}
