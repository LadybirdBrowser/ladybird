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
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

// Move-only owner of one strong reference to a Rust calculation node. This is
// how C++ builds calculation trees: every factory transfers its argument
// handles into the new node, mirroring the FFI construction surface, so a
// fully built tree is always owned by exactly one CalcNodeRef.
class CalcNodeRef {
    AK_MAKE_NONCOPYABLE(CalcNodeRef);

public:
    using NumericValue = CalculatedStyleValue::NumericValue;

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

    // Retains an additional reference to a borrowed node, for example the root
    // stored in a calculated style value's data.
    static CalcNodeRef retain(StyleValueFFI::CalcNode const* node)
    {
        StyleValueFFI::rust_calc_node_retain(node);
        return CalcNodeRef { node };
    }

    static CalcNodeRef numeric(NumericValue const&);
    // https://drafts.csswg.org/css-values-4/#calc-constants
    // Returns nothing for keywords that are not calc constants.
    static Optional<CalcNodeRef> from_keyword(Keyword);
    static CalcNodeRef channel_keyword(ChannelKeyword);

    static CalcNodeRef sum(Vector<CalcNodeRef>);
    static CalcNodeRef product(Vector<CalcNodeRef>);
    static CalcNodeRef min(Vector<CalcNodeRef>);
    static CalcNodeRef max(Vector<CalcNodeRef>);
    static CalcNodeRef hypot(Vector<CalcNodeRef>);

    static CalcNodeRef negate(CalcNodeRef);
    static CalcNodeRef invert(CalcNodeRef);
    static CalcNodeRef abs(CalcNodeRef);
    static CalcNodeRef sign(CalcNodeRef);
    static CalcNodeRef sin(CalcNodeRef);
    static CalcNodeRef cos(CalcNodeRef);
    static CalcNodeRef tan(CalcNodeRef);
    static CalcNodeRef asin(CalcNodeRef);
    static CalcNodeRef acos(CalcNodeRef);
    static CalcNodeRef atan(CalcNodeRef);
    static CalcNodeRef sqrt(CalcNodeRef);
    static CalcNodeRef exp(CalcNodeRef);

    // NB: Atan2's children are ordered y then x, matching the Rust tree.
    static CalcNodeRef atan2(CalcNodeRef y, CalcNodeRef x);
    static CalcNodeRef pow(CalcNodeRef, CalcNodeRef);
    static CalcNodeRef log(CalcNodeRef, CalcNodeRef);
    static CalcNodeRef mod(CalcNodeRef, CalcNodeRef);
    static CalcNodeRef rem(CalcNodeRef, CalcNodeRef);

    static CalcNodeRef clamp(CalcNodeRef minimum, CalcNodeRef center, CalcNodeRef maximum);
    static CalcNodeRef progress(bool no_clamp, CalcNodeRef value, CalcNodeRef start, CalcNodeRef end);
    static CalcNodeRef round(RoundingStrategy, CalcNodeRef value, CalcNodeRef interval);
    static CalcNodeRef random(StyleValue const& random_value_sharing, CalcNodeRef minimum, CalcNodeRef maximum, Optional<CalcNodeRef> step);
    static CalcNodeRef non_math_function(StyleValue const& function, Optional<NumericType> const&);
    // Numeric style values become numeric leaves; a calculated value
    // contributes its own Rust tree.
    static CalcNodeRef from_style_value(StyleValue const&);

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

}
