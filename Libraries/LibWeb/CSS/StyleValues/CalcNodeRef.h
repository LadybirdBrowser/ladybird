/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/NumericRange.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-4/#calc-context
// Contains the context available at parse-time.
struct CalculationContext {
    Optional<ValueType> percentages_resolve_as {};
    bool resolve_numbers_as_integers = false;
    // FIXME: Once calc() parsing knows the target numeric type, pass a single NumericRange instead of the full accepted range set.
    NumericRangesByValueType accepted_ranges_by_type {};

    static CalculationContext for_property(PropertyNameAndID const&);
};

// A numeric leaf of a calculation, with its kind numbered as the Rust core numbers it.
struct NumericLeaf {
    enum class Kind : u8 {
        Number,
        Angle,
        Flex,
        Frequency,
        Length,
        Percentage,
        Resolution,
        Time,
    };

    static Optional<NumericLeaf> from_typed_om_unit(double value, Utf16FlyString const& unit);

    Utf16FlyString typed_om_unit() const;
    ValueType value_type() const;
    NonnullRefPtr<StyleValue const> to_style_value() const;

    Kind kind;
    double value;
    u8 unit_or_number_type { 0 };
};

// Move-only owner of one strong reference to a Rust calculation node. This is
// how C++ builds calculation trees: every factory transfers its argument
// handles into the new node, mirroring the FFI construction surface, so a
// fully built tree is always owned by exactly one CalcNodeRef.
class CalcNodeRef {
    AK_MAKE_NONCOPYABLE(CalcNodeRef);

public:
    CalcNodeRef(CalcNodeRef&& other)
        : m_node(exchange(other.m_node, nullptr))
    {
    }

    CalcNodeRef& operator=(CalcNodeRef&& other)
    {
        CalcNodeRef moved(move(other));
        swap(m_node, moved.m_node);
        return *this;
    }

    ~CalcNodeRef()
    {
        if (m_node)
            StyleValueFFI::rust_calc_node_release(m_node);
    }

    // The borrowed node pointer; ownership stays with this ref.
    StyleValueFFI::CalcNode const* node() const { return m_node; }

    // Transfers ownership of the strong reference to the caller.
    StyleValueFFI::CalcNode const* release() { return exchange(m_node, nullptr); }

    // Adopts a transferred handle, for example one returned by the Rust
    // simplification entry points.
    static CalcNodeRef adopt(StyleValueFFI::CalcNode const* node) { return CalcNodeRef { node }; }

    static CalcNodeRef numeric(NumericLeaf const&);

    static CalcNodeRef sum(Vector<CalcNodeRef>);
    static CalcNodeRef product(Vector<CalcNodeRef>);
    static CalcNodeRef min(Vector<CalcNodeRef>);
    static CalcNodeRef max(Vector<CalcNodeRef>);

    static CalcNodeRef negate(CalcNodeRef);
    static CalcNodeRef invert(CalcNodeRef);

    static CalcNodeRef clamp(CalcNodeRef minimum, CalcNodeRef center, CalcNodeRef maximum);

    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The type of the calculation this node roots, determined by the Rust
    // core; nothing when the tree does not type-check.
    Optional<NumericType> determine_type(CalculationContext const&) const;

private:
    explicit CalcNodeRef(StyleValueFFI::CalcNode const* node)
        : m_node(node)
    {
        VERIFY(m_node);
    }

    StyleValueFFI::CalcNode const* m_node { nullptr };
};

ValueComparingNonnullRefPtr<StyleValue const> create_calculated_style_value(CalcNodeRef root, NumericType const& resolved_type, CalculationContext const&);

}
