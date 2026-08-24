/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSFunctionDeclarations.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/EnvironmentVariable.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/QueryValueType.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS::Parser {

static bool should_verify_rust_query_parser()
{
    auto* value = getenv("LIBWEB_VERIFY_RUST_QUERY_PARSER");
    return value && StringView { value, strlen(value) } == "1"sv;
}

Vector<NonnullRefPtr<MediaQuery>> Parser::parse_as_media_query_list()
{
    auto media_queries = RustQueryParser::parse_media_query_list(*this, m_source);
    if (should_verify_rust_query_parser()) {
        auto cpp_media_queries = parse_a_media_query_list(token_stream());
        auto rust_serialized = serialize_a_media_query_list(media_queries);
        auto cpp_serialized = serialize_a_media_query_list(cpp_media_queries);
        if (rust_serialized != cpp_serialized)
            warnln("Rust media query parser mismatch: Rust `{}`, C++ `{}`", rust_serialized, cpp_serialized);
    }
    return media_queries;
}

template<typename T>
Vector<NonnullRefPtr<MediaQuery>> Parser::parse_a_media_query_list(TokenStream<T>& tokens)
{
    // https://www.w3.org/TR/mediaqueries-4/#mq-list

    // AD-HOC: Ignore whitespace-only queries
    // to make `@media {..}` equivalent to `@media all {..}`
    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return {};

    auto comma_separated_lists = parse_a_comma_separated_list_of_component_values(tokens);

    AK::Vector<NonnullRefPtr<MediaQuery>> media_queries;
    for (auto& media_query_parts : comma_separated_lists) {
        auto stream = TokenStream(media_query_parts);
        media_queries.append(parse_media_query(stream));
    }

    return media_queries;
}

RefPtr<MediaQuery> Parser::parse_as_media_query()
{
    // https://www.w3.org/TR/cssom-1/#parse-a-media-query
    auto media_query_list = parse_as_media_query_list();
    if (media_query_list.is_empty())
        return MediaQuery::create_not_all();
    if (media_query_list.size() == 1)
        return media_query_list.first();
    return nullptr;
}

// `<media-query>`, https://www.w3.org/TR/mediaqueries-4/#typedef-media-query
NonnullRefPtr<MediaQuery> Parser::parse_media_query(TokenStream<ComponentValue>& tokens)
{
    // `<media-query> = <media-condition>
    //                | [ not | only ]? <media-type> [ and <media-condition-without-or> ]?`

    // `[ not | only ]?`, Returns whether to negate the query
    auto parse_initial_modifier = [](auto& tokens) -> Optional<bool> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        auto& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};

        auto ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("not"sv)) {
            transaction.commit();
            return true;
        }
        if (ident.equals_ignoring_ascii_case("only"sv)) {
            transaction.commit();
            return false;
        }
        return {};
    };

    auto invalid_media_query = [&](String&& description) {
        // "A media query that does not match the grammar in the previous section must be replaced by `not all`
        // during parsing." - https://www.w3.org/TR/mediaqueries-5/#error-handling
        ErrorReporter::the().report(InvalidQueryError {
            .query_type = "@media"_utf16_fly_string,
            .value_string = tokens.dump_string(),
            .description = move(description),
        });
        return MediaQuery::create_not_all();
    };

    auto media_query = MediaQuery::create();
    tokens.discard_whitespace();

    // `<media-condition>`
    if (auto media_condition = parse_media_condition(tokens)) {
        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return invalid_media_query("Trailing tokens after <media-condition>"_string);
        media_query->m_media_condition = media_condition.release_nonnull();
        return media_query;
    }

    // `[ not | only ]?`
    if (auto modifier = parse_initial_modifier(tokens); modifier.has_value()) {
        media_query->m_negated = modifier.value();
        tokens.discard_whitespace();
    }

    // `<media-type>`
    if (auto media_type = parse_media_type(tokens); media_type.has_value()) {
        media_query->m_media_type = media_type.release_value();
        tokens.discard_whitespace();
    } else {
        // https://drafts.csswg.org/mediaqueries-4/#error-handling
        // A media query that does not match the grammar in the previous section must be replaced by not all during parsing.
        return invalid_media_query("Doesn't match `<media-query>`"_string);
    }

    if (!tokens.has_next_token())
        return media_query;

    // `[ and <media-condition-without-or> ]?`
    if (auto const& maybe_and = tokens.consume_a_token(); maybe_and.is_ident("and"_utf16)) {
        if (auto media_condition = parse_media_condition(tokens)) {
            // "or" is disallowed at the top level
            if (is<BooleanOrExpression>(*media_condition))
                return invalid_media_query("Contains top-level `or`"_string);

            tokens.discard_whitespace();
            if (tokens.has_next_token())
                return invalid_media_query("Trailing tokens after `<media-condition-without-or>`"_string);
            media_query->m_media_condition = move(media_condition);
            return media_query;
        }
        return invalid_media_query("Missing `<media-condition>` after `and`"_string);
    }

    return invalid_media_query("Trailing tokens after `<media-query>`"_string);
}

// `<media-condition>`, https://www.w3.org/TR/mediaqueries-4/#typedef-media-condition
OwnPtr<BooleanExpression> Parser::parse_media_condition(TokenStream<ComponentValue>& tokens)
{
    return parse_boolean_expression(tokens, MatchResult::Unknown, [this](TokenStream<ComponentValue>& outer_tokens) -> OwnPtr<BooleanExpression> {
        auto transaction = outer_tokens.begin_transaction();

        outer_tokens.discard_whitespace();

        if (!(outer_tokens.next_token().is_block() && outer_tokens.next_token().block().is_paren()))
            return nullptr;

        auto const& block = outer_tokens.consume_a_token().block();

        TokenStream inner_tokens { block.value };

        if (auto maybe_media_feature = parse_media_feature(inner_tokens)) {
            transaction.commit();
            return maybe_media_feature;
        }

        return nullptr;
    });
}

enum class FeatureNameType : u8 {
    Normal,
    Min,
    Max,
};

template<typename FeatureID>
struct FeatureName {
    FeatureNameType type;
    FeatureID id;
};

// `<mf-lt> = '<' '='?
//  <mf-gt> = '>' '='?
//  <mf-eq> = '='
//  <mf-comparison> = <mf-lt> | <mf-gt> | <mf-eq>`
Optional<FeatureComparison> parse_feature_comparison(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto& first = tokens.consume_a_token();
    if (first.is(Token::Type::Delim)) {
        auto first_delim = first.token().delim();
        if (first_delim == '=') {
            transaction.commit();
            return FeatureComparison::Equal;
        }
        if (first_delim == '<') {
            auto& second = tokens.next_token();
            if (second.is_delim('=')) {
                tokens.discard_a_token();
                transaction.commit();
                return FeatureComparison::LessThanOrEqual;
            }
            transaction.commit();
            return FeatureComparison::LessThan;
        }
        if (first_delim == '>') {
            auto& second = tokens.next_token();
            if (second.is_delim('=')) {
                tokens.discard_a_token();
                transaction.commit();
                return FeatureComparison::GreaterThanOrEqual;
            }
            transaction.commit();
            return FeatureComparison::GreaterThan;
        }
    }

    return {};
}

template<typename Feature, typename FeatureID, typename FeatureNameFromString, typename ParseFeatureValue, typename AllowsRangeSyntax>
static OwnPtr<Feature> parse_query_feature(TokenStream<ComponentValue>& inner_tokens, FeatureNameFromString feature_name_from_string, ParseFeatureValue parse_feature_value, AllowsRangeSyntax allows_range_syntax)
{
    auto transaction = inner_tokens.begin_transaction();

    // `<mf-name> = <ident>`
    auto parse_feature_name = [&](auto& tokens, bool allow_min_max_prefix) -> Optional<FeatureName<FeatureID>> {
        auto transaction = tokens.begin_transaction();
        auto& token = tokens.consume_a_token();
        if (token.is(Token::Type::Ident)) {
            auto name = token.token().ident();
            if (auto id = feature_name_from_string(name); id.has_value()) {
                transaction.commit();
                return FeatureName<FeatureID> { FeatureNameType::Normal, id.value() };
            }

            if (allow_min_max_prefix && (name.starts_with_ignoring_ascii_case("min-"sv) || name.starts_with_ignoring_ascii_case("max-"sv))) {
                auto adjusted_name = name.view().substring_view(4);
                if (auto id = feature_name_from_string(adjusted_name); id.has_value() && allows_range_syntax(id.value())) {
                    transaction.commit();
                    return FeatureName<FeatureID> {
                        name.starts_with_ignoring_ascii_case("min-"sv) ? FeatureNameType::Min : FeatureNameType::Max,
                        id.value()
                    };
                }
            }
        }
        return {};
    };

    auto parse_feature_boolean = [&](auto& tokens) -> OwnPtr<Feature> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        if (auto maybe_name = parse_feature_name(tokens, false); maybe_name.has_value()) {
            tokens.discard_whitespace();
            if (!tokens.has_next_token()) {
                transaction.commit();
                return Feature::boolean(maybe_name->id);
            }
        }

        return {};
    };

    auto parse_feature_plain = [&](auto& tokens) -> OwnPtr<Feature> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        if (auto maybe_name = parse_feature_name(tokens, true); maybe_name.has_value()) {
            tokens.discard_whitespace();
            if (tokens.consume_a_token().is(Token::Type::Colon)) {
                tokens.discard_whitespace();
                if (auto maybe_value = parse_feature_value(maybe_name->id, tokens); maybe_value.has_value()) {
                    tokens.discard_whitespace();
                    if (!tokens.has_next_token()) {
                        transaction.commit();
                        switch (maybe_name->type) {
                        case FeatureNameType::Normal:
                            return Feature::plain(maybe_name->id, maybe_value.release_value());
                        case FeatureNameType::Min:
                            return Feature::min(maybe_name->id, maybe_value.release_value());
                        case FeatureNameType::Max:
                            return Feature::max(maybe_name->id, maybe_value.release_value());
                        }
                        VERIFY_NOT_REACHED();
                    }
                }
            }
        }

        return {};
    };

    // `<mf-range> = <mf-name> <mf-comparison> <mf-value>
    //             | <mf-value> <mf-comparison> <mf-name>
    //             | <mf-value> <mf-lt> <mf-name> <mf-lt> <mf-value>
    //             | <mf-value> <mf-gt> <mf-name> <mf-gt> <mf-value>`
    auto parse_feature_range = [&](auto& tokens) -> OwnPtr<Feature> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        // `<mf-name> <mf-comparison> <mf-value>`
        // NOTE: We have to check for <mf-name> first, since all <mf-name>s will also parse as <mf-value>.
        if (auto maybe_name = parse_feature_name(tokens, false); maybe_name.has_value() && allows_range_syntax(maybe_name->id)) {
            tokens.discard_whitespace();
            if (auto maybe_comparison = parse_feature_comparison(tokens); maybe_comparison.has_value()) {
                tokens.discard_whitespace();
                if (auto maybe_value = parse_feature_value(maybe_name->id, tokens); maybe_value.has_value()) {
                    tokens.discard_whitespace();
                    if (!tokens.has_next_token() && !maybe_value->is_ident()) {
                        transaction.commit();
                        return Feature::half_range(maybe_name->id, maybe_comparison.release_value(), maybe_value.release_value());
                    }
                }
            }
        }

        //  `<mf-value> <mf-comparison> <mf-name>
        // | <mf-value> <mf-lt> <mf-name> <mf-lt> <mf-value>
        // | <mf-value> <mf-gt> <mf-name> <mf-gt> <mf-value>`
        // NOTE: To parse the first value, we need to first find and parse the <mf-name> so we know what value types to parse.
        //       To allow for <mf-value> to be any number of tokens long, we scan forward until we find a comparison, and then
        //       treat the next non-whitespace token as the <mf-name>, which should be correct as long as they don't add a value
        //       type that can include a comparison in it. :^)
        Optional<FeatureName<FeatureID>> maybe_name;
        {
            // This transaction is never committed, we just use it to rewind automatically.
            auto temp_transaction = tokens.begin_transaction();
            while (tokens.has_next_token() && !maybe_name.has_value()) {
                if (auto maybe_comparison = parse_feature_comparison(tokens); maybe_comparison.has_value()) {
                    // We found a comparison, so the next non-whitespace token should be the <mf-name>
                    tokens.discard_whitespace();
                    maybe_name = parse_feature_name(tokens, false);
                    break;
                }
                tokens.discard_a_token();
                tokens.discard_whitespace();
            }
        }

        if (maybe_name.has_value() && allows_range_syntax(maybe_name->id)) {
            if (auto maybe_left_value = parse_feature_value(maybe_name->id, tokens); maybe_left_value.has_value()) {
                tokens.discard_whitespace();
                if (auto maybe_left_comparison = parse_feature_comparison(tokens); maybe_left_comparison.has_value()) {
                    tokens.discard_whitespace();
                    tokens.discard_a_token(); // The <mf-name> which we already parsed above.
                    tokens.discard_whitespace();

                    if (!tokens.has_next_token()) {
                        transaction.commit();
                        return Feature::half_range(maybe_left_value.release_value(), maybe_left_comparison.release_value(), maybe_name->id);
                    }

                    if (auto maybe_right_comparison = parse_feature_comparison(tokens); maybe_right_comparison.has_value()) {
                        tokens.discard_whitespace();
                        if (auto maybe_right_value = parse_feature_value(maybe_name->id, tokens); maybe_right_value.has_value()) {
                            tokens.discard_whitespace();
                            // For this to be valid, the following must be true:
                            // - Comparisons must either both be >/>= or both be </<=.
                            // - Neither comparison can be `=`.
                            // - Neither value can be an ident.
                            auto left_comparison = maybe_left_comparison.release_value();
                            auto right_comparison = maybe_right_comparison.release_value();

                            if (!tokens.has_next_token()
                                && feature_comparisons_match(left_comparison, right_comparison)
                                && left_comparison != FeatureComparison::Equal
                                && !maybe_left_value->is_ident() && !maybe_right_value->is_ident()) {
                                transaction.commit();
                                return Feature::range(maybe_left_value.release_value(), left_comparison, maybe_name->id, right_comparison, maybe_right_value.release_value());
                            }
                        }
                    }
                }
            }
        }

        return {};
    };

    if (auto maybe_feature_boolean = parse_feature_boolean(inner_tokens)) {
        inner_tokens.discard_whitespace();
        if (inner_tokens.has_next_token())
            return nullptr;
        transaction.commit();
        return maybe_feature_boolean.release_nonnull();
    }

    if (auto maybe_feature_plain = parse_feature_plain(inner_tokens)) {
        inner_tokens.discard_whitespace();
        if (inner_tokens.has_next_token())
            return nullptr;
        transaction.commit();
        return maybe_feature_plain.release_nonnull();
    }

    if (auto maybe_feature_range = parse_feature_range(inner_tokens)) {
        inner_tokens.discard_whitespace();
        if (inner_tokens.has_next_token())
            return nullptr;
        transaction.commit();
        return maybe_feature_range.release_nonnull();
    }

    return {};
}

// `<media-feature>`, https://drafts.csswg.org/mediaqueries-5/#typedef-media-feature
OwnPtr<MediaFeature> Parser::parse_media_feature(TokenStream<ComponentValue>& inner_tokens)
{
    return parse_query_feature<MediaFeature, MediaFeatureID>(
        inner_tokens,
        [](Utf16View name) { return media_feature_id_from_string(name); },
        [this](MediaFeatureID id, auto& tokens) { return parse_media_feature_value(id, tokens); },
        [](MediaFeatureID id) { return media_feature_type_is_range(id); });
}

// `<size-feature>`, https://drafts.csswg.org/css-conditional-5/#size-container
OwnPtr<SizeFeature> Parser::parse_size_feature(TokenStream<ComponentValue>& inner_tokens)
{
    return parse_query_feature<SizeFeature, SizeFeatureID>(
        inner_tokens,
        [](Utf16View name) { return size_feature_id_from_string(name); },
        [this](SizeFeatureID id, auto& tokens) { return parse_size_feature_value(id, tokens); },
        [](SizeFeatureID id) { return size_feature_type_is_range(id); });
}

Optional<MediaQuery::MediaType> Parser::parse_media_type(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& token = tokens.consume_a_token();

    if (!token.is(Token::Type::Ident))
        return {};

    // https://drafts.csswg.org/mediaqueries-3/#error-handling
    // "However, an exception is made for media types ‘layer’, ‘not’, ‘and’, ‘only’, and ‘or’. Even though they do match
    // the IDENT production, they must not be treated as unknown media types, but rather trigger the malformed query clause."
    if (token.is_ident("layer"_utf16) || token.is_ident("not"_utf16) || token.is_ident("and"_utf16) || token.is_ident("only"_utf16) || token.is_ident("or"_utf16))
        return {};

    transaction.commit();

    auto const& ident = token.token().ident();
    return MediaQuery::MediaType {
        .name = ident,
        .known_type = media_type_from_string(ident),
    };
}

static bool is_feature_value_token(ComponentValue const& component_value)
{
    if (!component_value.is_token())
        return true;
    switch (component_value.token().type()) {
    case Token::Type::Ident:
    case Token::Type::Function:
    case Token::Type::AtKeyword:
    case Token::Type::Hash:
    case Token::Type::String:
    case Token::Type::BadString:
    case Token::Type::Url:
    case Token::Type::BadUrl:
    case Token::Type::Number:
    case Token::Type::Percentage:
    case Token::Type::Dimension:
    case Token::Type::Whitespace:
    case Token::Type::Comma:
        return true;
    case Token::Type::Delim:
        // FIXME: What list of delimiters should we actually allow here?
        return !first_is_one_of(component_value.token().delim(), static_cast<u32>('<'), static_cast<u32>('>'), static_cast<u32>('='));
    case Token::Type::Invalid:
    case Token::Type::EndOfFile:
    case Token::Type::CDO:
    case Token::Type::CDC:
    case Token::Type::Colon:
    case Token::Type::Semicolon:
    case Token::Type::OpenSquare:
    case Token::Type::CloseSquare:
    case Token::Type::OpenParen:
    case Token::Type::CloseParen:
    case Token::Type::OpenCurly:
    case Token::Type::CloseCurly:
        return false;
    }
    VERIFY_NOT_REACHED();
}

template<typename FeatureID, typename FeatureAcceptsKeyword, typename FeatureAcceptsType>
Optional<FeatureValue> Parser::parse_feature_value(FeatureID feature, TokenStream<ComponentValue>& tokens, FeatureAcceptsKeyword feature_accepts_keyword, FeatureAcceptsType feature_accepts_type)
{
    {
        auto transaction = tokens.begin_transaction();
        auto value = [&](FeatureID feature, TokenStream<ComponentValue>& tokens) -> Optional<FeatureValue> {
            auto context_guard = push_temporary_value_parsing_context(SpecialContext::MediaCondition);

            // One branch for each member of the QueryValueType enum:
            // Identifiers
            if (tokens.next_token().is(Token::Type::Ident)) {
                auto transaction = tokens.begin_transaction();
                tokens.discard_whitespace();
                auto keyword = parse_keyword_value(tokens);
                if (keyword && feature_accepts_keyword(feature, keyword->to_keyword())) {
                    transaction.commit();
                    return FeatureValue(FeatureValue::Type::Ident, keyword.release_nonnull());
                }
            }

            // Boolean (<mq-boolean> in the spec: a 1 or 0)
            if (feature_accepts_type(feature, QueryValueType::Boolean)) {
                auto transaction = tokens.begin_transaction();
                tokens.discard_whitespace();
                if (auto integer = parse_integer_value(tokens, infinite_integer_range)) {
                    if (integer->is_calculated() || first_is_one_of(integer->as_integer().integer(), 0, 1)) {
                        transaction.commit();
                        return FeatureValue(FeatureValue::Type::Integer, integer.release_nonnull());
                    }
                }
            }

            // Integer
            if (feature_accepts_type(feature, QueryValueType::Integer)) {
                auto transaction = tokens.begin_transaction();
                if (auto integer = parse_integer_value(tokens, infinite_integer_range)) {
                    transaction.commit();
                    return FeatureValue(FeatureValue::Type::Integer, integer.release_nonnull());
                }
            }

            // Length
            if (feature_accepts_type(feature, QueryValueType::Length)) {
                auto transaction = tokens.begin_transaction();
                tokens.discard_whitespace();
                if (auto length = parse_length_value(tokens, infinite_range)) {
                    transaction.commit();
                    return FeatureValue(FeatureValue::Type::Length, length.release_nonnull());
                }

                // https://drafts.csswg.org/mediaqueries-5/#typedef-mf-value
                // <mf-value> = <number> | <dimension> | <ident> | <ratio>
                //
                // https://drafts.csswg.org/css-values-4/#lengths
                // "For zero lengths the unit identifier is optional"
                //
                // https://drafts.csswg.org/css-values-4/#zero-value
                // "Values of '0' can be written without units, even if the
                // value type doesn't allow 'unitless zeroes'."
                if (tokens.has_next_token()) {
                    if (auto number = parse_number_value(tokens, infinite_range); number && number->is_calculated() && number->as_calculated().resolves_to_number()) {
                        if (auto resolved_number = number->as_calculated().resolve_number({}); resolved_number.has_value() && *resolved_number == 0) {
                            transaction.commit();
                            return FeatureValue(FeatureValue::Type::Length, LengthStyleValue::create(Length::make_px(0)));
                        }
                    }
                }
            }

            // Ratio
            if (feature_accepts_type(feature, QueryValueType::Ratio)) {
                auto transaction = tokens.begin_transaction();
                tokens.discard_whitespace();
                if (auto ratio = parse_ratio_value(tokens)) {
                    transaction.commit();
                    return FeatureValue(FeatureValue::Type::Ratio, ratio.release_nonnull());
                }
            }

            // Resolution
            if (feature_accepts_type(feature, QueryValueType::Resolution)) {
                auto transaction = tokens.begin_transaction();
                tokens.discard_whitespace();
                if (auto resolution = parse_resolution_value(tokens, infinite_range)) {
                    transaction.commit();
                    return FeatureValue(FeatureValue::Type::Resolution, resolution.release_nonnull());
                }
            }

            return {};
        }(feature, tokens);

        if (value.has_value()) {
            tokens.discard_whitespace();

            // Only returned the value if there are no trailing tokens.
            // Otherwise, the transaction gets reverted and we consume all the value tokens below.
            if (!is_feature_value_token(tokens.next_token())) {
                transaction.commit();
                return value.release_value();
            }
        }
    }

    // Parsing failed somehow, so wrap all the tokens into an "unknown" FeatureValue if possible.

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    Vector<ComponentValue> unknown_tokens;

    // Consume any tokens that could be part of a value.
    while (tokens.has_next_token()) {
        if (is_feature_value_token(tokens.next_token())) {
            unknown_tokens.append(tokens.consume_a_token());
        } else {
            break;
        }
    }

    if (!unknown_tokens.is_empty()) {
        transaction.commit();
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<mf-value>"_utf16_fly_string,
            .value_string = MUST(String::join(""sv, unknown_tokens)),
            .description = "Unrecognized type"_string,
        });
        // NB: We only use this for serialization so the substitution function presence is irrelevant and we can just
        //     set it to empty.
        return FeatureValue(FeatureValue::Type::Unknown, move(UnresolvedStyleValue::create(move(unknown_tokens), {})));
    }

    return {};
}

// `<mf-value>`, https://www.w3.org/TR/mediaqueries-4/#typedef-mf-value
Optional<FeatureValue> Parser::parse_media_feature_value(MediaFeatureID feature, TokenStream<ComponentValue>& tokens)
{
    return parse_feature_value(
        feature,
        tokens,
        [](MediaFeatureID feature, Keyword keyword) { return media_feature_accepts_keyword(feature, keyword); },
        [](MediaFeatureID feature, QueryValueType type) { return media_feature_accepts_type(feature, type); });
}

Optional<FeatureValue> Parser::parse_size_feature_value(SizeFeatureID feature, TokenStream<ComponentValue>& tokens)
{
    auto size_feature_accepts_keyword = [](SizeFeatureID feature, Keyword keyword) {
        return feature == SizeFeatureID::Orientation && first_is_one_of(keyword, Keyword::Landscape, Keyword::Portrait);
    };
    auto size_feature_accepts_type = [](SizeFeatureID feature, QueryValueType type) {
        switch (type) {
        case QueryValueType::Length:
            return first_is_one_of(feature,
                SizeFeatureID::BlockSize,
                SizeFeatureID::Height,
                SizeFeatureID::InlineSize,
                SizeFeatureID::Width);
        case QueryValueType::Ratio:
            return feature == SizeFeatureID::AspectRatio;
        default:
            return false;
        }
    };
    return parse_feature_value(feature, tokens, size_feature_accepts_keyword, size_feature_accepts_type);
}

template<typename NestedDeclarationsRule>
GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule(AtRule const& rule, Nested nested)
{
    m_rule_context.append(RuleContext::AtMedia);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtMedia);
    };

    // https://drafts.csswg.org/css-conditional-3/#at-media
    // @media <media-query-list> {
    // <rule-list>
    // }
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@media"_utf16_fly_string,
            .prelude = MUST(String::join(""sv, rule.prelude)),
            .description = "Expected a block."_string,
        });
        return nullptr;
    }

    auto media_query_list = RustQueryParser::parse_media_query_list(*this, rule.prelude_text);
    if (should_verify_rust_query_parser()) {
        auto media_query_tokens = TokenStream { rule.prelude };
        auto cpp_media_query_list = parse_a_media_query_list(media_query_tokens);
        auto rust_serialized = serialize_a_media_query_list(media_query_list);
        auto cpp_serialized = serialize_a_media_query_list(cpp_media_query_list);
        if (rust_serialized != cpp_serialized)
            warnln("Rust media query parser mismatch: Rust `{}`, C++ `{}`", rust_serialized, cpp_serialized);
    }
    auto media_list = MediaList::create(move(media_query_list));

    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule<NestedDeclarationsRule>(rule, nested))
                    child_rules.append(*child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(NestedDeclarationsRule::create(*this, declarations));
            });
    }
    auto rule_list = CSSRuleList::create(child_rules);
    return CSSMediaRule::create(media_list, rule_list);
}

template<typename T>
RefPtr<Supports> Parser::parse_a_supports(TokenStream<T>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto component_values = parse_a_list_of_component_values(tokens);
    TokenStream<ComponentValue> token_stream { component_values };
    auto maybe_condition = parse_supports_condition(token_stream);
    token_stream.discard_whitespace();
    if (maybe_condition && !token_stream.has_next_token()) {
        transaction.commit();
        return Supports::create(maybe_condition.release_nonnull());
    }

    return {};
}

// https://drafts.csswg.org/css-values-5/#typedef-boolean-expr
OwnPtr<BooleanExpression> Parser::parse_boolean_expression(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed, ParseTest parse_test)
{
    // <boolean-expr[ <test> ]> = not <boolean-expr-group> | <boolean-expr-group>
    //                            [ [ and <boolean-expr-group> ]*
    //                            | [ or <boolean-expr-group> ]* ]

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto const& peeked_token = tokens.next_token();
    // `not <boolean-expr-group>`
    if (peeked_token.is_ident("not"_utf16)) {
        tokens.discard_a_token();
        tokens.discard_whitespace();

        if (auto child = parse_boolean_expression_group(tokens, result_for_general_enclosed, parse_test)) {
            tokens.discard_whitespace();
            transaction.commit();
            return BooleanNotExpression::create(child.release_nonnull());
        }
        return {};
    }

    // `<boolean-expr-group>
    //   [ [ and <boolean-expr-group> ]*
    //   | [ or <boolean-expr-group> ]* ]`
    Vector<NonnullOwnPtr<BooleanExpression>> children;
    enum class Combinator : u8 {
        And,
        Or,
    };
    Optional<Combinator> combinator;
    auto as_combinator = [](auto& token) -> Optional<Combinator> {
        if (!token.is(Token::Type::Ident))
            return {};
        auto ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("and"sv))
            return Combinator::And;
        if (ident.equals_ignoring_ascii_case("or"sv))
            return Combinator::Or;
        return {};
    };

    while (tokens.has_next_token()) {
        if (!children.is_empty()) {
            // Expect `and` or `or` here
            auto maybe_combinator = as_combinator(tokens.consume_a_token());
            if (!maybe_combinator.has_value())
                return {};
            if (!combinator.has_value()) {
                combinator = maybe_combinator.value();
            } else if (maybe_combinator != combinator) {
                return {};
            }
        }

        tokens.discard_whitespace();

        if (auto child = parse_boolean_expression_group(tokens, result_for_general_enclosed, parse_test)) {
            children.append(child.release_nonnull());
        } else {
            return {};
        }

        tokens.discard_whitespace();
    }

    if (children.is_empty())
        return {};

    transaction.commit();
    if (children.size() == 1)
        return children.take_first();

    VERIFY(combinator.has_value());
    switch (*combinator) {
    case Combinator::And:
        return BooleanAndExpression::create(move(children));
    case Combinator::Or:
        return BooleanOrExpression::create(move(children));
    }
    VERIFY_NOT_REACHED();
}

OwnPtr<BooleanExpression> Parser::parse_boolean_expression_group(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed, ParseTest parse_test)
{
    // <boolean-expr-group> = <test> | ( <boolean-expr[ <test> ]> ) | <general-enclosed>

    // `( <boolean-expr[ <test> ]> )`
    auto const& first_token = tokens.next_token();
    if (first_token.is_block() && first_token.block().is_paren()) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_a_token();
        tokens.discard_whitespace();

        TokenStream child_tokens { first_token.block().value };
        if (auto expression = parse_boolean_expression(child_tokens, result_for_general_enclosed, parse_test)) {
            if (child_tokens.has_next_token())
                return {};
            transaction.commit();
            return BooleanExpressionInParens::create(expression.release_nonnull());
        }
    }

    // `<test>`
    if (auto test = parse_test(tokens))
        return test.release_nonnull();

    // `<general-enclosed>`
    if (auto general_enclosed = parse_general_enclosed(tokens, result_for_general_enclosed))
        return general_enclosed.release_nonnull();

    return {};
}

// https://drafts.csswg.org/css-conditional-3/#typedef-supports-condition
OwnPtr<BooleanExpression> Parser::parse_supports_condition(TokenStream<ComponentValue>& tokens)
{
    m_rule_context.append(RuleContext::SupportsCondition);
    auto maybe_condition = parse_boolean_expression(tokens, MatchResult::False, [this](auto& tokens) { return parse_supports_feature(tokens); });
    m_rule_context.take_last();

    return maybe_condition;
}

static bool at_rule_is_supported(Utf16FlyString const& name)
{
    // https://drafts.csswg.org/css-conditional-5/#support-definition-at-rules
    // A CSS processor supports an at-rule if it would accept an at-rule beginning with that
    // at-keyword within any context. @charset is intentionally excluded: it is not a valid at-rule.
    if (name.equals_ignoring_ascii_case("charset"sv))
        return false;

    if (name.equals_ignoring_ascii_case("container"sv)
        || name.equals_ignoring_ascii_case("counter-style"sv)
        || name.equals_ignoring_ascii_case("font-face"sv)
        || name.equals_ignoring_ascii_case("font-feature-values"sv)
        || name.equals_ignoring_ascii_case("function"sv)
        || name.equals_ignoring_ascii_case("import"sv)
        || name.equals_ignoring_ascii_case("keyframes"sv)
        || name.equals_ignoring_ascii_case("-webkit-keyframes"sv)
        || name.equals_ignoring_ascii_case("layer"sv)
        || name.equals_ignoring_ascii_case("media"sv)
        || name.equals_ignoring_ascii_case("namespace"sv)
        || name.equals_ignoring_ascii_case("page"sv)
        || name.equals_ignoring_ascii_case("property"sv)
        || name.equals_ignoring_ascii_case("scope"sv)
        || name.equals_ignoring_ascii_case("supports"sv))
        return true;

    if (CSSFontFeatureValuesRule::is_font_feature_value_type_at_keyword(name))
        return true;

    if (is_margin_rule_name(name))
        return true;

    return false;
}

// https://drafts.csswg.org/css-conditional-5/#typedef-supports-feature
OwnPtr<BooleanExpression> Parser::parse_supports_feature(TokenStream<ComponentValue>& tokens)
{
    // <supports-feature> = <supports-selector-fn> | <supports-font-tech-fn>
    //                    | <supports-font-format-fn> | <supports-at-rule-fn> | <supports-env-fn>
    //                    | <supports-decl>
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();

    // `<supports-decl> = ( <declaration> )`
    if (first_token.is_block() && first_token.block().is_paren()) {
        TokenStream block_tokens { first_token.block().value };
        if (auto declaration = parse_supports_declaration(block_tokens)) {
            transaction.commit();
            return BooleanExpressionInParens::create(declaration.release_nonnull<BooleanExpression>());
        }
    }

    // `<supports-selector-fn> = selector( <complex-selector> )`
    if (first_token.is_function("selector"_utf16)) {
        // FIXME: Parsing and then converting back to a string is weird.
        Utf16StringBuilder builder;
        for (auto const& item : first_token.function().value)
            item.serialize_to(builder);
        transaction.commit();
        TokenStream selector_tokens { first_token.function().value };
        auto maybe_selectors = parse_a_selector_list(selector_tokens, SelectorType::Standalone);
        // A CSS processor is considered to support a CSS selector if it accepts that all aspects of that selector,
        // recursively, (rather than considering any of its syntax to be unknown or invalid) and that selector doesn’t
        // contain unknown -webkit- pseudo-elements.
        // https://drafts.csswg.org/css-conditional-4/#dfn-support-selector
        bool matches = !maybe_selectors.is_error() && maybe_selectors.value().size() == 1
            && !maybe_selectors.value().first()->contains_unknown_webkit_pseudo_element();
        return Supports::Selector::create(builder.to_string(), matches);
    }

    // `<supports-font-tech-fn> = font-tech( <font-tech> )`
    if (first_token.is_function("font-tech"_utf16)) {
        TokenStream tech_tokens { first_token.function().value };
        tech_tokens.discard_whitespace();
        auto tech_token = tech_tokens.consume_a_token();
        tech_tokens.discard_whitespace();
        if (tech_tokens.has_next_token() || !tech_token.is(Token::Type::Ident))
            return {};

        transaction.commit();
        auto tech_name = tech_token.token().ident();
        bool matches = font_tech_is_supported(tech_name);
        return Supports::FontTech::create(move(tech_name), matches);
    }

    // `<supports-font-format-fn> = font-format( <font-format> )`
    if (first_token.is_function("font-format"_utf16)) {
        TokenStream format_tokens { first_token.function().value };
        format_tokens.discard_whitespace();
        auto format_token = format_tokens.consume_a_token();
        format_tokens.discard_whitespace();
        if (format_tokens.has_next_token() || !format_token.is(Token::Type::Ident))
            return {};

        transaction.commit();
        auto format_name = format_token.token().ident();
        bool matches = font_format_is_supported(format_name);
        return Supports::FontFormat::create(move(format_name), matches);
    }

    // `<supports-at-rule-fn> = at-rule( <at-keyword-token> )`
    if (first_token.is_function("at-rule"_utf16)) {
        TokenStream at_rule_tokens { first_token.function().value };
        at_rule_tokens.discard_whitespace();
        auto at_rule_token = at_rule_tokens.consume_a_token();
        at_rule_tokens.discard_whitespace();
        if (at_rule_tokens.has_next_token() || !at_rule_token.is(Token::Type::AtKeyword))
            return {};

        transaction.commit();
        auto at_rule_name = at_rule_token.token().at_keyword();
        bool matches = at_rule_is_supported(at_rule_name);
        return Supports::AtRule::create(move(at_rule_name), matches);
    }

    // `<supports-env-fn> = env( <ident> )`
    if (first_token.is_function("env"_utf16)) {
        TokenStream format_tokens { first_token.function().value };
        format_tokens.discard_whitespace();
        auto variable_token = format_tokens.consume_a_token();
        format_tokens.discard_whitespace();
        if (format_tokens.has_next_token() || !variable_token.is(Token::Type::Ident))
            return {};

        transaction.commit();
        auto variable_name = variable_token.token().ident();
        // https://drafts.csswg.org/css-conditional-5/#support-definition-env
        // A CSS processor is considered to support an environment variable if the <ident> is a supported environment
        // variable.
        bool matches = environment_variable_from_string(variable_name).has_value();
        return Supports::FontFormat::create(move(variable_name), matches);
    }

    return {};
}

// https://drafts.csswg.org/css-conditional-5/#typedef-supports-decl
OwnPtr<Supports::Declaration> Parser::parse_supports_declaration(TokenStream<ComponentValue>& tokens)
{
    // `<supports-decl> = ( <declaration> )`
    // NB: Here, we only care about the <declaration> part.
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    if (auto declaration = consume_a_declaration(tokens, Nested::No, SaveOriginalText::Yes); declaration.has_value()) {
        tokens.discard_whitespace();
        if (!tokens.has_next_token()) {
            transaction.commit();
            return Supports::Declaration::create(declaration->original_full_text.release_value(), convert_to_style_property(*declaration).has_value());
        }
    }
    return {};
}

OwnPtr<BooleanExpression> Parser::parse_container_query_condition(TokenStream<ComponentValue>& tokens)
{
    // https://drafts.csswg.org/css-conditional-5/#container-rule
    // As with media queries, <general-enclosed> evaluates to unknown.
    return parse_boolean_expression(tokens, MatchResult::Unknown, [this](auto& tokens) {
        return parse_container_query_feature(tokens);
    });
}

OwnPtr<BooleanExpression> Parser::parse_style_query(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed)
{
    return parse_boolean_expression(tokens, result_for_general_enclosed, [this](auto& tokens) {
        return parse_style_feature(tokens);
    });
}

static bool next_token_is_feature_comparison(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    return parse_feature_comparison(tokens).has_value();
}

static bool contains_feature_comparison(Vector<ComponentValue> const& component_values)
{
    TokenStream tokens { component_values };
    while (tokens.has_next_token()) {
        if (next_token_is_feature_comparison(tokens))
            return true;
        tokens.discard_a_token();
    }
    return false;
}

static ReadonlySpan<ComponentValue> trim_style_range_value_tokens(ReadonlySpan<ComponentValue> tokens)
{
    auto start = 0uz;
    while (start < tokens.size() && tokens[start].is(Token::Type::Whitespace))
        ++start;

    auto end = tokens.size();
    while (end > start && tokens[end - 1].is(Token::Type::Whitespace))
        --end;

    return tokens.slice(start, end - start);
}

static Optional<StyleFeature::StyleRangeValue> parse_style_range_value(ReadonlySpan<ComponentValue> tokens)
{
    auto trimmed_tokens = trim_style_range_value_tokens(tokens);
    if (trimmed_tokens.is_empty())
        return {};

    if (trimmed_tokens.size() == 1 && trimmed_tokens.first().is(Token::Type::Ident)) {
        auto const& ident = trimmed_tokens.first().token().ident();
        if (is_a_custom_property_name_string(ident)) {
            auto property = PropertyNameAndID::from_name(ident);
            if (property.has_value())
                return StyleFeature::StyleRangeValue { property.release_value() };
        }
    }

    TokenStream value_tokens { trimmed_tokens };
    if (!Parser::parse_declaration_value_as_span(value_tokens).has_value() || !value_tokens.is_empty())
        return {};

    return StyleFeature::StyleRangeValue { Vector<ComponentValue> { trimmed_tokens } };
}

static Optional<StyleFeature::StyleRangeValue> parse_style_range_value_until_comparison(TokenStream<ComponentValue>& tokens)
{
    auto start_index = tokens.current_index();
    while (tokens.has_next_token()) {
        if (next_token_is_feature_comparison(tokens))
            break;
        tokens.discard_a_token();
    }

    return parse_style_range_value(tokens.tokens_since(start_index));
}

// https://drafts.csswg.org/css-conditional-5/#typedef-style-feature
OwnPtr<BooleanExpression> Parser::parse_style_feature(TokenStream<ComponentValue>& tokens)
{
    // <style-feature> = <style-feature-plain> | <style-feature-boolean> | <style-range>

    auto parse_style_feature_name = [](Utf16FlyString const& name) -> Optional<PropertyNameAndID> {
        // The <style-feature-name> can be either a supported CSS property or a valid <custom-property-name>.
        // NB: This is the same as what's allowed by PropertyNameAndID.
        return PropertyNameAndID::from_name(name);
    };

    // <style-range> = <style-range-value> <mf-comparison> <style-range-value>
    //               | <style-range-value> <mf-lt> <style-range-value> <mf-lt> <style-range-value>
    //               | <style-range-value> <mf-gt> <style-range-value> <mf-gt> <style-range-value>
    {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        if (auto maybe_left = parse_style_range_value_until_comparison(tokens); maybe_left.has_value()) {
            if (auto maybe_left_comparison = parse_feature_comparison(tokens); maybe_left_comparison.has_value()) {
                if (auto maybe_middle = parse_style_range_value_until_comparison(tokens); maybe_middle.has_value()) {
                    tokens.discard_whitespace();
                    if (!tokens.has_next_token()) {
                        transaction.commit();
                        return StyleFeature::create_range(maybe_left.release_value(), maybe_left_comparison.release_value(), maybe_middle.release_value());
                    }

                    if (auto maybe_right_comparison = parse_feature_comparison(tokens); maybe_right_comparison.has_value()) {
                        if (auto maybe_right = parse_style_range_value_until_comparison(tokens); maybe_right.has_value()) {
                            tokens.discard_whitespace();

                            auto left_comparison = maybe_left_comparison.release_value();
                            auto right_comparison = maybe_right_comparison.release_value();

                            if (!tokens.has_next_token()
                                && feature_comparisons_match(left_comparison, right_comparison)
                                && left_comparison != FeatureComparison::Equal) {
                                transaction.commit();
                                return StyleFeature::create_range(
                                    maybe_left.release_value(),
                                    left_comparison,
                                    maybe_middle.release_value(),
                                    right_comparison,
                                    maybe_right.release_value());
                            }
                        }
                    }
                }
            }
        }
    }

    // <style-feature-plain> = <style-feature-name> : <style-feature-value>
    {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        m_rule_context.append(RuleContext::Style);
        auto declaration = consume_a_declaration(tokens, Nested::No, SaveOriginalText::Yes);
        m_rule_context.take_last();
        if (declaration.has_value()) {
            tokens.discard_whitespace();
            if (tokens.has_next_token())
                return nullptr;

            // The <style-feature-value> production matches any valid <declaration-value> as long as it doesn't contain
            // <mf-lt>, <mf-gt> and <mf-eq> tokens.
            if (declaration->value.contains([](ComponentValue const& value) {
                    return value.is(Token::Type::Delim)
                        && first_is_one_of(static_cast<char>(value.token().delim()), '<', '>', '=');
                })) {
                return nullptr;
            }

            auto style_feature_name = parse_style_feature_name(declaration->name);
            if (!style_feature_name.has_value())
                return nullptr;
            if (contains_feature_comparison(declaration->value))
                return nullptr;

            transaction.commit();
            return StyleFeature::create_plain(style_feature_name.release_value(), move(declaration->value), move(declaration->original_value_text));
        }
    }

    // <style-feature-boolean> = <style-feature-name>
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& token = tokens.consume_a_token();
    tokens.discard_whitespace();
    if (tokens.has_next_token() || !token.is(Token::Type::Ident))
        return nullptr;

    auto style_feature_name = parse_style_feature_name(token.token().ident());
    if (!style_feature_name.has_value())
        return nullptr;

    transaction.commit();
    return StyleFeature::create_boolean(style_feature_name.release_value());
}

OwnPtr<BooleanExpression> Parser::parse_container_query_feature(TokenStream<ComponentValue>& tokens)
{
    // https://drafts.csswg.org/css-conditional-5/#typedef-query-in-parens
    // <query-in-parens> = ( <container-query> )
    //                   | ( <size-feature> )
    //                   | style( <style-query> )
    //                   | scroll-state( <scroll-state-query> )
    //                   | <general-enclosed>

    // https://drafts.csswg.org/css-anchor-position-2/#container-rule-anchored
    // <query-in-parens> = ...
    //                   | anchored( <anchored-query> )

    // NB: Spec isn't yet in terms of `<boolean-condition>`, so this is the closest definition to what we want.
    //     `( <container-query> )` and `<general-enclosed>` are handled by parse_boolean_expression() already.

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // `( <size-feature> )`
    if (tokens.next_token().is_block() && tokens.next_token().block().is_paren()) {
        auto const& block = tokens.consume_a_token().block();
        TokenStream inner_tokens { block.value };
        if (auto size_feature = parse_size_feature(inner_tokens)) {
            inner_tokens.discard_whitespace();
            if (inner_tokens.has_next_token()) {
                ErrorReporter::the().report(InvalidQueryError {
                    .query_type = "@container"_utf16_fly_string,
                    .value_string = tokens.dump_string(),
                    .description = "Trailing tokens in size feature."_string });
                return nullptr;
            }

            transaction.commit();
            return size_feature;
        }

        ErrorReporter::the().report(InvalidQueryError {
            .query_type = "@container"_utf16_fly_string,
            .value_string = tokens.dump_string(),
            .description = "Failed to parse parenthesis block as a size feature."_string });
        return nullptr;
    }

    // `style( <style-query> )`
    if (tokens.next_token().is_function("style"sv)) {
        auto const& function = tokens.consume_a_token().function();
        TokenStream inner_tokens { function.value };
        if (auto style_query = parse_style_query(inner_tokens, MatchResult::Unknown)) {
            inner_tokens.discard_whitespace();
            if (inner_tokens.has_next_token()) {
                ErrorReporter::the().report(InvalidQueryError {
                    .query_type = "@container"_utf16_fly_string,
                    .value_string = function.to_string().to_well_formed_utf8(),
                    .description = "Trailing tokens in style()."_string });
                return nullptr;
            }

            transaction.commit();
            return StyleQueryFunction::create(style_query.release_nonnull());
        }

        ErrorReporter::the().report(InvalidQueryError {
            .query_type = "@container"_utf16_fly_string,
            .value_string = function.to_string().to_well_formed_utf8(),
            .description = "Failed to parse style()."_string });
        return nullptr;
    }

    // FIXME: `scroll-state( <scroll-state-query> )`
    // FIXME: `anchored( <anchored-query> )`

    ErrorReporter::the().report(InvalidQueryError {
        .query_type = "@container"_utf16_fly_string,
        .value_string = tokens.dump_string(),
        .description = MUST(String::formatted("Unexpected token in query feature: {}.", tokens.next_token().to_debug_string())) });
    return nullptr;
}

RefPtr<ContainerQuery> Parser::parse_container_query(TokenStream<ComponentValue>& tokens)
{
    if (auto condition = parse_container_query_condition(tokens))
        return ContainerQuery::create(condition.release_nonnull());
    return nullptr;
}

// https://drafts.csswg.org/mediaqueries-5/#typedef-general-enclosed
OwnPtr<GeneralEnclosed> Parser::parse_general_enclosed(TokenStream<ComponentValue>& tokens, MatchResult result)
{
    // <general-enclosed> = [ <function-token> <any-value>? ) ] | [ ( <any-value>? ) ]
    //
    // https://drafts.csswg.org/css-syntax-3/#typedef-any-value
    // "The <any-value> production is identical to <declaration-value>",
    // and <declaration-value> does not contain "<<bad-string-token>>,
    // <<bad-url-token>>, unmatched <<)-token>>, <<]-token>>, or
    // <<}-token>>".
    auto contains_only_any_value = [](auto const& values, auto&& contains_only_any_value) -> bool {
        for (auto const& value : values) {
            if (value.is_function()) {
                if (!contains_only_any_value(value.function().value, contains_only_any_value))
                    return false;
                continue;
            }

            if (value.is_block()) {
                if (!contains_only_any_value(value.block().value, contains_only_any_value))
                    return false;
                continue;
            }

            if (!value.is_token())
                continue;

            switch (value.token().type()) {
            case Token::Type::Invalid:
            case Token::Type::EndOfFile:
            case Token::Type::BadString:
            case Token::Type::BadUrl:
                // NB: Functions and blocks are emitted as component values, so any remaining bracket tokens are unmatched.
            case Token::Type::Function:
            case Token::Type::OpenCurly:
            case Token::Type::OpenParen:
            case Token::Type::OpenSquare:
            case Token::Type::CloseCurly:
            case Token::Type::CloseParen:
            case Token::Type::CloseSquare:
                return false;
            default:
                break;
            }
        }

        return true;
    };

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();
    auto serialize_general_enclosed = [](ComponentValue const& component_value) {
        auto original_source_text = component_value.original_source_text();
        if (!original_source_text.is_empty())
            return original_source_text;
        return component_value.to_string();
    };

    // `[ <function-token> <any-value>? ) ]`
    if (first_token.is_function()) {
        if (!contains_only_any_value(first_token.function().value, contains_only_any_value))
            return {};
        transaction.commit();
        return GeneralEnclosed::create(serialize_general_enclosed(first_token), result);
    }

    // `( <any-value>? )`
    if (first_token.is_block() && first_token.block().is_paren()) {
        if (!contains_only_any_value(first_token.block().value, contains_only_any_value))
            return {};
        transaction.commit();
        return GeneralEnclosed::create(serialize_general_enclosed(first_token), result);
    }

    return {};
}

template GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule<CSSNestedDeclarations>(AtRule const&, Parser::Nested);
template GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule<CSSFunctionDeclarations>(AtRule const&, Parser::Nested);

template RefPtr<Supports> Parser::parse_a_supports(TokenStream<ComponentValue>&);
template RefPtr<Supports> Parser::parse_a_supports(TokenStream<Token>&);

}
