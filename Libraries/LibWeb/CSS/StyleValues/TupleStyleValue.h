/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class TupleStyleValue final : public StyleValueWithDefaultOperators<TupleStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TupleStyleValue const> create(StyleValueTuple values)
    {
        return adopt_ref(*new (nothrow) TupleStyleValue(move(values)));
    }
    virtual ~TupleStyleValue() override = default;

    StyleValueTuple const& tuple() const { return m_tuple; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    // FIXME: Support tokenization and reification

    bool properties_equal(TupleStyleValue const& other) const { return m_tuple == other.m_tuple; }

    virtual bool is_computationally_independent() const override
    {
        return all_of(m_tuple, [](auto& value) { return value->is_computationally_independent(); });
    }

    struct Indices {
        struct FontVariantEastAsian {
            static constexpr size_t Variant = 0;
            static constexpr size_t Width = 1;
            static constexpr size_t Ruby = 2;
        };

        struct FontVariantLigatures {
            static constexpr size_t Common = 0;
            static constexpr size_t Discretionary = 1;
            static constexpr size_t Historical = 2;
            static constexpr size_t Contextual = 3;
        };

        struct FontVariantNumeric {
            static constexpr size_t Figure = 0;
            static constexpr size_t Spacing = 1;
            static constexpr size_t Fraction = 2;
            static constexpr size_t Ordinal = 3;
            static constexpr size_t SlashedZero = 4;
        };

        struct ScrollFunction {
            static constexpr size_t Scroller = 0;
            static constexpr size_t Axis = 1;
        };

        struct ViewFunction {
            static constexpr size_t Axis = 0;
            static constexpr size_t Inset = 1;
        };
    };

private:
    explicit TupleStyleValue(StyleValueTuple values)
        : StyleValueWithDefaultOperators(Type::Tuple)
        , m_tuple(move(values))
    {
    }

    StyleValueTuple m_tuple;
};

}
