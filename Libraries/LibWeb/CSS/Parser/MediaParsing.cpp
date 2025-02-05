/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Parser/Parser.h>

namespace Web::CSS::Parser {

Vector<NonnullRefPtr<MediaQuery>> Parser::parse_as_media_query_list()
{
    return parse_a_media_query_list(m_token_stream);
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

    auto invalid_media_query = [&]() {
        // "A media query that does not match the grammar in the previous section must be replaced by `not all`
        // during parsing." - https://www.w3.org/TR/mediaqueries-5/#error-handling
        if constexpr (CSS_PARSER_DEBUG) {
            dbgln("Invalid media query:");
            tokens.dump_all_tokens();
        }
        return MediaQuery::create_not_all();
    };

    auto media_query = MediaQuery::create();
    tokens.discard_whitespace();

    // `<media-condition>`
    if (auto media_condition = parse_media_condition(tokens, MediaCondition::AllowOr::Yes)) {
        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return invalid_media_query();
        media_query->m_media_condition = move(media_condition);
        return media_query;
    }

    // `[ not | only ]?`
    if (auto modifier = parse_initial_modifier(tokens); modifier.has_value()) {
        media_query->m_negated = modifier.value();
        tokens.discard_whitespace();
    }

    // `<media-type>`
    if (auto media_type = parse_media_type(tokens); media_type.has_value()) {
        media_query->m_media_type = media_type.value();
        tokens.discard_whitespace();
    } else {
        return invalid_media_query();
    }

    if (!tokens.has_next_token())
        return media_query;

    // `[ and <media-condition-without-or> ]?`
    if (auto const& maybe_and = tokens.consume_a_token(); maybe_and.is_ident("and"sv)) {
        if (auto media_condition = parse_media_condition(tokens, MediaCondition::AllowOr::No)) {
            tokens.discard_whitespace();
            if (tokens.has_next_token())
                return invalid_media_query();
            media_query->m_media_condition = move(media_condition);
            return media_query;
        }
        return invalid_media_query();
    }

    return invalid_media_query();
}

// `<media-condition>`, https://www.w3.org/TR/mediaqueries-4/#typedef-media-condition
// `<media-condition-widthout-or>`, https://www.w3.org/TR/mediaqueries-4/#typedef-media-condition-without-or
// (We distinguish between these two with the `allow_or` parameter.)
OwnPtr<MediaCondition> Parser::parse_media_condition(TokenStream<ComponentValue>& tokens, MediaCondition::AllowOr allow_or)
{
    // `<media-not> | <media-in-parens> [ <media-and>* | <media-or>* ]`
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // `<media-not> = not <media-in-parens>`
    auto parse_media_not = [&](auto& tokens) -> OwnPtr<MediaCondition> {
        auto local_transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto& first_token = tokens.consume_a_token();
        if (first_token.is_ident("not"sv)) {
            if (auto child_condition = parse_media_condition(tokens, MediaCondition::AllowOr::Yes)) {
                local_transaction.commit();
                return MediaCondition::from_not(child_condition.release_nonnull());
            }
        }

        return {};
    };

    auto parse_media_with_combinator = [&](auto& tokens, StringView combinator) -> OwnPtr<MediaCondition> {
        auto local_transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto& first = tokens.consume_a_token();
        if (first.is_ident(combinator)) {
            tokens.discard_whitespace();
            if (auto media_in_parens = parse_media_in_parens(tokens)) {
                local_transaction.commit();
                return media_in_parens;
            }
        }

        return {};
    };

    // `<media-and> = and <media-in-parens>`
    auto parse_media_and = [&](auto& tokens) { return parse_media_with_combinator(tokens, "and"sv); };
    // `<media-or> = or <media-in-parens>`
    auto parse_media_or = [&](auto& tokens) { return parse_media_with_combinator(tokens, "or"sv); };

    // `<media-not>`
    if (auto maybe_media_not = parse_media_not(tokens)) {
        transaction.commit();
        return maybe_media_not.release_nonnull();
    }

    // `<media-in-parens> [ <media-and>* | <media-or>* ]`
    if (auto maybe_media_in_parens = parse_media_in_parens(tokens)) {
        tokens.discard_whitespace();
        // Only `<media-in-parens>`
        if (!tokens.has_next_token()) {
            transaction.commit();
            return maybe_media_in_parens.release_nonnull();
        }

        Vector<NonnullOwnPtr<MediaCondition>> child_conditions;
        child_conditions.append(maybe_media_in_parens.release_nonnull());

        // `<media-and>*`
        if (auto media_and = parse_media_and(tokens)) {
            child_conditions.append(media_and.release_nonnull());

            tokens.discard_whitespace();
            while (tokens.has_next_token()) {
                if (auto next_media_and = parse_media_and(tokens)) {
                    child_conditions.append(next_media_and.release_nonnull());
                    tokens.discard_whitespace();
                    continue;
                }
                // We failed - invalid syntax!
                return {};
            }

            transaction.commit();
            return MediaCondition::from_and_list(move(child_conditions));
        }

        // `<media-or>*`
        if (allow_or == MediaCondition::AllowOr::Yes) {
            if (auto media_or = parse_media_or(tokens)) {
                child_conditions.append(media_or.release_nonnull());

                tokens.discard_whitespace();
                while (tokens.has_next_token()) {
                    if (auto next_media_or = parse_media_or(tokens)) {
                        child_conditions.append(next_media_or.release_nonnull());
                        tokens.discard_whitespace();
                        continue;
                    }
                    // We failed - invalid syntax!
                    return {};
                }

                transaction.commit();
                return MediaCondition::from_or_list(move(child_conditions));
            }
        }
    }

    return {};
}

// `<media-feature>`, https://www.w3.org/TR/mediaqueries-4/#typedef-media-feature
Optional<MediaFeature> Parser::parse_media_feature(TokenStream<ComponentValue>& tokens)
{
    // `[ <mf-plain> | <mf-boolean> | <mf-range> ]`
    tokens.discard_whitespace();

    // `<mf-name> = <ident>`
    struct MediaFeatureName {
        enum Type {
            Normal,
            Min,
            Max
        } type;
        MediaFeatureID id;
    };
    auto parse_mf_name = [](auto& tokens, bool allow_min_max_prefix) -> Optional<MediaFeatureName> {
        auto transaction = tokens.begin_transaction();
        auto& token = tokens.consume_a_token();
        if (token.is(Token::Type::Ident)) {
            auto name = token.token().ident();
            if (auto id = media_feature_id_from_string(name); id.has_value()) {
                transaction.commit();
                return MediaFeatureName { MediaFeatureName::Type::Normal, id.value() };
            }

            if (allow_min_max_prefix && (name.starts_with_bytes("min-"sv, CaseSensitivity::CaseInsensitive) || name.starts_with_bytes("max-"sv, CaseSensitivity::CaseInsensitive))) {
                auto adjusted_name = name.bytes_as_string_view().substring_view(4);
                if (auto id = media_feature_id_from_string(adjusted_name); id.has_value() && media_feature_type_is_range(id.value())) {
                    transaction.commit();
                    return MediaFeatureName {
                        name.starts_with_bytes("min-"sv, CaseSensitivity::CaseInsensitive) ? MediaFeatureName::Type::Min : MediaFeatureName::Type::Max,
                        id.value()
                    };
                }
            }
        }
        return {};
    };

    // `<mf-boolean> = <mf-name>`
    auto parse_mf_boolean = [&](auto& tokens) -> Optional<MediaFeature> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        if (auto maybe_name = parse_mf_name(tokens, false); maybe_name.has_value()) {
            tokens.discard_whitespace();
            if (!tokens.has_next_token()) {
                transaction.commit();
                return MediaFeature::boolean(maybe_name->id);
            }
        }

        return {};
    };

    // `<mf-plain> = <mf-name> : <mf-value>`
    auto parse_mf_plain = [&](auto& tokens) -> Optional<MediaFeature> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        if (auto maybe_name = parse_mf_name(tokens, true); maybe_name.has_value()) {
            tokens.discard_whitespace();
            if (tokens.consume_a_token().is(Token::Type::Colon)) {
                tokens.discard_whitespace();
                if (auto maybe_value = parse_media_feature_value(maybe_name->id, tokens); maybe_value.has_value()) {
                    tokens.discard_whitespace();
                    if (!tokens.has_next_token()) {
                        transaction.commit();
                        switch (maybe_name->type) {
                        case MediaFeatureName::Type::Normal:
                            return MediaFeature::plain(maybe_name->id, maybe_value.release_value());
                        case MediaFeatureName::Type::Min:
                            return MediaFeature::min(maybe_name->id, maybe_value.release_value());
                        case MediaFeatureName::Type::Max:
                            return MediaFeature::max(maybe_name->id, maybe_value.release_value());
                        }
                        VERIFY_NOT_REACHED();
                    }
                }
            }
        }
        return {};
    };

    // `<mf-lt> = '<' '='?
    //  <mf-gt> = '>' '='?
    //  <mf-eq> = '='
    //  <mf-comparison> = <mf-lt> | <mf-gt> | <mf-eq>`
    auto parse_comparison = [](auto& tokens) -> Optional<MediaFeature::Comparison> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto& first = tokens.consume_a_token();
        if (first.is(Token::Type::Delim)) {
            auto first_delim = first.token().delim();
            if (first_delim == '=') {
                transaction.commit();
                return MediaFeature::Comparison::Equal;
            }
            if (first_delim == '<') {
                auto& second = tokens.next_token();
                if (second.is_delim('=')) {
                    tokens.discard_a_token();
                    transaction.commit();
                    return MediaFeature::Comparison::LessThanOrEqual;
                }
                transaction.commit();
                return MediaFeature::Comparison::LessThan;
            }
            if (first_delim == '>') {
                auto& second = tokens.next_token();
                if (second.is_delim('=')) {
                    tokens.discard_a_token();
                    transaction.commit();
                    return MediaFeature::Comparison::GreaterThanOrEqual;
                }
                transaction.commit();
                return MediaFeature::Comparison::GreaterThan;
            }
        }

        return {};
    };

    auto flip = [](MediaFeature::Comparison comparison) {
        switch (comparison) {
        case MediaFeature::Comparison::Equal:
            return MediaFeature::Comparison::Equal;
        case MediaFeature::Comparison::LessThan:
            return MediaFeature::Comparison::GreaterThan;
        case MediaFeature::Comparison::LessThanOrEqual:
            return MediaFeature::Comparison::GreaterThanOrEqual;
        case MediaFeature::Comparison::GreaterThan:
            return MediaFeature::Comparison::LessThan;
        case MediaFeature::Comparison::GreaterThanOrEqual:
            return MediaFeature::Comparison::LessThanOrEqual;
        }
        VERIFY_NOT_REACHED();
    };

    auto comparisons_match = [](MediaFeature::Comparison a, MediaFeature::Comparison b) -> bool {
        switch (a) {
        case MediaFeature::Comparison::Equal:
            return b == MediaFeature::Comparison::Equal;
        case MediaFeature::Comparison::LessThan:
        case MediaFeature::Comparison::LessThanOrEqual:
            return b == MediaFeature::Comparison::LessThan || b == MediaFeature::Comparison::LessThanOrEqual;
        case MediaFeature::Comparison::GreaterThan:
        case MediaFeature::Comparison::GreaterThanOrEqual:
            return b == MediaFeature::Comparison::GreaterThan || b == MediaFeature::Comparison::GreaterThanOrEqual;
        }
        VERIFY_NOT_REACHED();
    };

    // `<mf-range> = <mf-name> <mf-comparison> <mf-value>
    //             | <mf-value> <mf-comparison> <mf-name>
    //             | <mf-value> <mf-lt> <mf-name> <mf-lt> <mf-value>
    //             | <mf-value> <mf-gt> <mf-name> <mf-gt> <mf-value>`
    auto parse_mf_range = [&](auto& tokens) -> Optional<MediaFeature> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        // `<mf-name> <mf-comparison> <mf-value>`
        // NOTE: We have to check for <mf-name> first, since all <mf-name>s will also parse as <mf-value>.
        if (auto maybe_name = parse_mf_name(tokens, false); maybe_name.has_value() && media_feature_type_is_range(maybe_name->id)) {
            tokens.discard_whitespace();
            if (auto maybe_comparison = parse_comparison(tokens); maybe_comparison.has_value()) {
                tokens.discard_whitespace();
                if (auto maybe_value = parse_media_feature_value(maybe_name->id, tokens); maybe_value.has_value()) {
                    tokens.discard_whitespace();
                    if (!tokens.has_next_token() && !maybe_value->is_ident()) {
                        transaction.commit();
                        return MediaFeature::half_range(maybe_value.release_value(), flip(maybe_comparison.release_value()), maybe_name->id);
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
        Optional<MediaFeatureName> maybe_name;
        {
            // This transaction is never committed, we just use it to rewind automatically.
            auto temp_transaction = tokens.begin_transaction();
            while (tokens.has_next_token() && !maybe_name.has_value()) {
                if (auto maybe_comparison = parse_comparison(tokens); maybe_comparison.has_value()) {
                    // We found a comparison, so the next non-whitespace token should be the <mf-name>
                    tokens.discard_whitespace();
                    maybe_name = parse_mf_name(tokens, false);
                    break;
                }
                tokens.discard_a_token();
                tokens.discard_whitespace();
            }
        }

        // Now, we can parse the range properly.
        if (maybe_name.has_value() && media_feature_type_is_range(maybe_name->id)) {
            if (auto maybe_left_value = parse_media_feature_value(maybe_name->id, tokens); maybe_left_value.has_value()) {
                tokens.discard_whitespace();
                if (auto maybe_left_comparison = parse_comparison(tokens); maybe_left_comparison.has_value()) {
                    tokens.discard_whitespace();
                    tokens.discard_a_token(); // The <mf-name> which we already parsed above.
                    tokens.discard_whitespace();

                    if (!tokens.has_next_token()) {
                        transaction.commit();
                        return MediaFeature::half_range(maybe_left_value.release_value(), maybe_left_comparison.release_value(), maybe_name->id);
                    }

                    if (auto maybe_right_comparison = parse_comparison(tokens); maybe_right_comparison.has_value()) {
                        tokens.discard_whitespace();
                        if (auto maybe_right_value = parse_media_feature_value(maybe_name->id, tokens); maybe_right_value.has_value()) {
                            tokens.discard_whitespace();
                            // For this to be valid, the following must be true:
                            // - Comparisons must either both be >/>= or both be </<=.
                            // - Neither comparison can be `=`.
                            // - Neither value can be an ident.
                            auto left_comparison = maybe_left_comparison.release_value();
                            auto right_comparison = maybe_right_comparison.release_value();

                            if (!tokens.has_next_token()
                                && comparisons_match(left_comparison, right_comparison)
                                && left_comparison != MediaFeature::Comparison::Equal
                                && !maybe_left_value->is_ident() && !maybe_right_value->is_ident()) {
                                transaction.commit();
                                return MediaFeature::range(maybe_left_value.release_value(), left_comparison, maybe_name->id, right_comparison, maybe_right_value.release_value());
                            }
                        }
                    }
                }
            }
        }

        return {};
    };

    if (auto maybe_mf_boolean = parse_mf_boolean(tokens); maybe_mf_boolean.has_value())
        return maybe_mf_boolean.release_value();

    if (auto maybe_mf_plain = parse_mf_plain(tokens); maybe_mf_plain.has_value())
        return maybe_mf_plain.release_value();

    if (auto maybe_mf_range = parse_mf_range(tokens); maybe_mf_range.has_value())
        return maybe_mf_range.release_value();

    return {};
}

Optional<MediaQuery::MediaType> Parser::parse_media_type(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& token = tokens.consume_a_token();

    if (!token.is(Token::Type::Ident))
        return {};

    transaction.commit();

    auto ident = token.token().ident();
    return media_type_from_string(ident);
}

// `<media-in-parens>`, https://www.w3.org/TR/mediaqueries-4/#typedef-media-in-parens
OwnPtr<MediaCondition> Parser::parse_media_in_parens(TokenStream<ComponentValue>& tokens)
{
    // `<media-in-parens> = ( <media-condition> ) | ( <media-feature> ) | <general-enclosed>`
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // `( <media-condition> ) | ( <media-feature> )`
    auto const& first_token = tokens.next_token();
    if (first_token.is_block() && first_token.block().is_paren()) {
        TokenStream inner_token_stream { first_token.block().value };
        if (auto maybe_media_condition = parse_media_condition(inner_token_stream, MediaCondition::AllowOr::Yes)) {
            tokens.discard_a_token();
            transaction.commit();
            return maybe_media_condition.release_nonnull();
        }
        if (auto maybe_media_feature = parse_media_feature(inner_token_stream); maybe_media_feature.has_value()) {
            tokens.discard_a_token();
            transaction.commit();
            return MediaCondition::from_feature(maybe_media_feature.release_value());
        }
    }

    // `<general-enclosed>`
    // FIXME: We should only be taking this branch if the grammar doesn't match the above options.
    //        Currently we take it if the above fail to parse, which is different.
    //        eg, `@media (min-width: 76yaks)` is valid grammar, but does not parse because `yaks` isn't a unit.
    if (auto maybe_general_enclosed = parse_general_enclosed(tokens); maybe_general_enclosed.has_value()) {
        transaction.commit();
        return MediaCondition::from_general_enclosed(maybe_general_enclosed.release_value());
    }

    return {};
}

// `<mf-value>`, https://www.w3.org/TR/mediaqueries-4/#typedef-mf-value
Optional<MediaFeatureValue> Parser::parse_media_feature_value(MediaFeatureID media_feature, TokenStream<ComponentValue>& tokens)
{
    // Identifiers
    if (tokens.next_token().is(Token::Type::Ident)) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        auto keyword = keyword_from_string(tokens.consume_a_token().token().ident());
        if (keyword.has_value() && media_feature_accepts_keyword(media_feature, keyword.value())) {
            transaction.commit();
            return MediaFeatureValue(keyword.value());
        }
    }

    // One branch for each member of the MediaFeatureValueType enum:

    // Boolean (<mq-boolean> in the spec: a 1 or 0)
    if (media_feature_accepts_type(media_feature, MediaFeatureValueType::Boolean)) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        if (auto integer = parse_integer(tokens); integer.has_value()) {
            if (integer.value().is_calculated() || integer->value() == 0 || integer->value() == 1) {
                transaction.commit();
                return MediaFeatureValue(integer.release_value());
            }
        }
    }

    // Integer
    if (media_feature_accepts_type(media_feature, MediaFeatureValueType::Integer)) {
        auto transaction = tokens.begin_transaction();
        if (auto integer = parse_integer(tokens); integer.has_value()) {
            transaction.commit();
            return MediaFeatureValue(integer.release_value());
        }
    }

    // Length
    if (media_feature_accepts_type(media_feature, MediaFeatureValueType::Length)) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        if (auto length = parse_length(tokens); length.has_value()) {
            transaction.commit();
            return MediaFeatureValue(length.release_value());
        }
    }

    // Ratio
    if (media_feature_accepts_type(media_feature, MediaFeatureValueType::Ratio)) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        if (auto ratio = parse_ratio(tokens); ratio.has_value()) {
            transaction.commit();
            return MediaFeatureValue(ratio.release_value());
        }
    }

    // Resolution
    if (media_feature_accepts_type(media_feature, MediaFeatureValueType::Resolution)) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        if (auto resolution = parse_resolution(tokens); resolution.has_value()) {
            transaction.commit();
            return MediaFeatureValue(resolution.release_value());
        }
    }

    return {};
}

GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule(AtRule const& rule, Nested nested)
{
    auto media_query_tokens = TokenStream { rule.prelude };
    auto media_query_list = parse_a_media_query_list(media_query_tokens);
    auto media_list = MediaList::create(realm(), move(media_query_list));

    GC::RootVector<CSSRule*> child_rules { realm().heap() };
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule(rule, nested))
                    child_rules.append(child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                auto* declaration = convert_to_style_declaration(declarations);
                if (!declaration) {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: nested declarations invalid; discarding.");
                    return;
                }
                child_rules.append(CSSNestedDeclarations::create(realm(), *declaration));
            });
    }
    auto rule_list = CSSRuleList::create(realm(), child_rules);
    return CSSMediaRule::create(realm(), media_list, rule_list);
}

}
