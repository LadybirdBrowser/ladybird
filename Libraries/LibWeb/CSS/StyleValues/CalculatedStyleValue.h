/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/NumericRange.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

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
    static ValueComparingNonnullRefPtr<CalculatedStyleValue const> create(CalcNodeRef root, NumericType resolved_type, CalculationContext context);
    bool equals(StyleValue const& other) const;
    CalculationContext calculation_context() const;

    bool contains_percentage() const;

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

    static StyleValueFFI::StyleValueData const* make_calculated_data_from_rust_root(StyleValueFFI::CalcNode const*, NumericType const&, CalculationContext const&);

    NumericType resolved_type() const;
};

}
