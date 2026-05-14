/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/FeatureQuery.h>

namespace Web::CSS {

String FeatureValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    m_value->serialize(builder, mode);
    return MUST(builder.to_string());
}

StringView string_from_feature_comparison(FeatureComparison comparison)
{
    switch (comparison) {
    case FeatureComparison::Equal:
        return "="sv;
    case FeatureComparison::LessThan:
        return "<"sv;
    case FeatureComparison::LessThanOrEqual:
        return "<="sv;
    case FeatureComparison::GreaterThan:
        return ">"sv;
    case FeatureComparison::GreaterThanOrEqual:
        return ">="sv;
    }
    VERIFY_NOT_REACHED();
}

bool feature_comparisons_match(FeatureComparison a, FeatureComparison b)
{
    switch (a) {
    case FeatureComparison::Equal:
        return b == FeatureComparison::Equal;
    case FeatureComparison::LessThan:
    case FeatureComparison::LessThanOrEqual:
        return b == FeatureComparison::LessThan || b == FeatureComparison::LessThanOrEqual;
    case FeatureComparison::GreaterThan:
    case FeatureComparison::GreaterThanOrEqual:
        return b == FeatureComparison::GreaterThan || b == FeatureComparison::GreaterThanOrEqual;
    }
    VERIFY_NOT_REACHED();
}

MatchResult compare_feature_values(FeatureValue const& left, FeatureComparison comparison, FeatureValue const& right, ComputationContext const& computation_context)
{
    if (left.is_unknown() || right.is_unknown())
        return MatchResult::Unknown;

    if (!left.is_same_type(right))
        return MatchResult::False;

    if (left.is_ident()) {
        if (comparison == FeatureComparison::Equal)
            return as_match_result(left.ident() == right.ident());
        return MatchResult::False;
    }

    if (left.is_integer()) {
        switch (comparison) {
        case FeatureComparison::Equal:
            return as_match_result(left.integer(computation_context) == right.integer(computation_context));
        case FeatureComparison::LessThan:
            return as_match_result(left.integer(computation_context) < right.integer(computation_context));
        case FeatureComparison::LessThanOrEqual:
            return as_match_result(left.integer(computation_context) <= right.integer(computation_context));
        case FeatureComparison::GreaterThan:
            return as_match_result(left.integer(computation_context) > right.integer(computation_context));
        case FeatureComparison::GreaterThanOrEqual:
            return as_match_result(left.integer(computation_context) >= right.integer(computation_context));
        }
        VERIFY_NOT_REACHED();
    }

    if (left.is_length()) {
        auto left_px = left.length(computation_context).absolute_length_to_px();
        auto right_px = right.length(computation_context).absolute_length_to_px();

        switch (comparison) {
        case FeatureComparison::Equal:
            return as_match_result(left_px == right_px);
        case FeatureComparison::LessThan:
            return as_match_result(left_px < right_px);
        case FeatureComparison::LessThanOrEqual:
            return as_match_result(left_px <= right_px);
        case FeatureComparison::GreaterThan:
            return as_match_result(left_px > right_px);
        case FeatureComparison::GreaterThanOrEqual:
            return as_match_result(left_px >= right_px);
        }

        VERIFY_NOT_REACHED();
    }

    if (left.is_ratio()) {
        auto left_decimal = left.ratio(computation_context).value();
        auto right_decimal = right.ratio(computation_context).value();

        switch (comparison) {
        case FeatureComparison::Equal:
            return as_match_result(left_decimal == right_decimal);
        case FeatureComparison::LessThan:
            return as_match_result(left_decimal < right_decimal);
        case FeatureComparison::LessThanOrEqual:
            return as_match_result(left_decimal <= right_decimal);
        case FeatureComparison::GreaterThan:
            return as_match_result(left_decimal > right_decimal);
        case FeatureComparison::GreaterThanOrEqual:
            return as_match_result(left_decimal >= right_decimal);
        }
        VERIFY_NOT_REACHED();
    }

    if (left.is_resolution()) {
        auto left_dppx = left.resolution(computation_context).to_dots_per_pixel();
        auto right_dppx = right.resolution(computation_context).to_dots_per_pixel();

        switch (comparison) {
        case FeatureComparison::Equal:
            return as_match_result(left_dppx == right_dppx);
        case FeatureComparison::LessThan:
            return as_match_result(left_dppx < right_dppx);
        case FeatureComparison::LessThanOrEqual:
            return as_match_result(left_dppx <= right_dppx);
        case FeatureComparison::GreaterThan:
            return as_match_result(left_dppx > right_dppx);
        case FeatureComparison::GreaterThanOrEqual:
            return as_match_result(left_dppx >= right_dppx);
        }
        VERIFY_NOT_REACHED();
    }

    VERIFY_NOT_REACHED();
}

}
