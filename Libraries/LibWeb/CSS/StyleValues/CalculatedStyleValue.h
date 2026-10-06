/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/NumericRange.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class CalcNodeRef;

// https://drafts.csswg.org/css-values-4/#calc-context
// Contains the context available at parse-time.
struct CalculationContext {
    Optional<ValueType> percentages_resolve_as {};
    bool resolve_numbers_as_integers = false;
    // FIXME: Once calc() parsing knows the target numeric type, pass a single NumericRange instead of the full accepted range set.
    NumericRangesByValueType accepted_ranges_by_type {};

    static CalculationContext for_property(PropertyNameAndID const&);
};

class CalculatedStyleValue : public StyleValue {
public:
    // The numeric value alternatives a calculation leaf can hold.
    using NumericValue = Variant<Number, Angle, Flex, Frequency, Length, Percentage, Resolution, Time>;

    static ValueComparingNonnullRefPtr<CalculatedStyleValue const> create(CalcNodeRef root, NumericType resolved_type, CalculationContext context);
    bool equals(StyleValue const& other) const;
    StyleValueFFI::CalcNode const* rust_calculation_root() const { return m_value->calculated.rust_calculation.node; }
    CalculationContext calculation_context() const;

    Optional<Angle> resolve_angle(CalculationResolutionContext const&) const;

    bool resolves_to_length() const { return resolved_type().matches_length(calculation_context().percentages_resolve_as); }
    Optional<Length> resolve_length(CalculationResolutionContext const&) const;

    bool resolves_to_percentage() const { return resolved_type().matches_percentage(); }
    Optional<Percentage> resolve_percentage(CalculationResolutionContext const&) const;

    Optional<Resolution> resolve_resolution(CalculationResolutionContext const&) const;

    bool resolves_to_time() const { return resolved_type().matches_time(calculation_context().percentages_resolve_as); }
    Optional<Time> resolve_time(CalculationResolutionContext const&) const;

    bool resolves_to_number() const { return resolved_type().matches_number(calculation_context().percentages_resolve_as); }
    Optional<double> resolve_number(CalculationResolutionContext const&) const;
    Optional<i32> resolve_integer(CalculationResolutionContext const&) const;

    bool contains_percentage() const;
    bool is_fully_simplified() const;

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;

private:
    friend class StyleValue;

    explicit CalculatedStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::Calculated, data)
    {
    }

    // Takes ownership of a transferred Rust calculation root.
    explicit CalculatedStyleValue(StyleValueFFI::CalcNode const* rust_root, NumericType resolved_type, CalculationContext context)
        : StyleValue(Type::Calculated, make_calculated_data_from_rust_root(rust_root, resolved_type, context))
    {
    }

    struct ResolvedValue {
        double value;
        Optional<NumericType> type;
    };
    // FIXME: Calculations should be simplified apart from percentages by the absolutized method prior to this method
    //        being called so we can take just the percentage_basis rather than a full CalculationResolutionContext.
    //        There are still some CalculatedStyleValues which we don't call absolutized for (i.e. sub-values of other
    //        StyleValue classes which lack their own absolutized method) which will need to be fixed beforehand.
    Optional<ResolvedValue> resolve_value(CalculationResolutionContext const&, bool apply_censoring_and_clamping = true) const;

    static StyleValueFFI::StyleValueData const* make_calculated_data_from_rust_root(StyleValueFFI::CalcNode const*, NumericType const&, CalculationContext const&);

    NumericType resolved_type() const;
};

}
