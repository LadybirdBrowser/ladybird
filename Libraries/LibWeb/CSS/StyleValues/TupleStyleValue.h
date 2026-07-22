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

    StyleValueTuple tuple() const
    {
        return m_values;
    }

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    // FIXME: Support tokenization and reification

    bool properties_equal(TupleStyleValue const& other) const { return tuple() == other.tuple(); }

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
    friend class StyleValue;

    explicit TupleStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Tuple, data)
    {
        auto const& values = data->tuple.values;
        m_values.ensure_capacity(values.length);
        for (size_t i = 0; i < values.length; ++i) {
            auto* child_data = static_cast<StyleValueFFI::StyleValueData const*>(values.pointer[i].pointer);
            if (child_data)
                m_values.unchecked_append(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data)));
            else
                m_values.unchecked_append(nullptr);
        }
    }

    explicit TupleStyleValue(StyleValueTuple values)
        : StyleValueWithDefaultOperators(Type::Tuple, make_tuple_data(values))
        , m_values(move(values))
    {
    }

    static StyleValueFFI::StyleValueData const* make_tuple_data(StyleValueTuple const& values)
    {
        Vector<StyleValueFFI::StyleValueData const*> pointers;
        pointers.ensure_capacity(values.size());
        for (auto const& value : values) {
            if (value)
                pointers.unchecked_append(StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
            else
                pointers.unchecked_append(nullptr);
        }
        return StyleValueFFI::rust_style_value_create_tuple(pointers.data(), pointers.size());
    }

    StyleValueTuple m_values;
};

}
