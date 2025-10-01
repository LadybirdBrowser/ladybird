/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class TreeCountingFunctionStyleValue final : public StyleValue {
public:
    enum class TreeCountingFunction : u8 {
        SiblingCount,
        SiblingIndex
    };

    enum class ComputedType : u8 {
        Number,
        Integer
    };

    static ValueComparingNonnullRefPtr<TreeCountingFunctionStyleValue const> create(TreeCountingFunction function, ComputedType computed_type)
    {
        return adopt_ref(*new (nothrow) TreeCountingFunctionStyleValue(function, computed_type));
    }
    virtual ~TreeCountingFunctionStyleValue() override = default;

    virtual String to_string(SerializationMode) const override;

    size_t resolve(TreeCountingFunctionResolutionContext const&) const;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual bool equals(StyleValue const& other) const override;

private:
    TreeCountingFunctionStyleValue(TreeCountingFunction function, ComputedType computed_type)
        : StyleValue(Type::TreeCountingFunction)
        , m_function(function)
        , m_computed_type(computed_type)
    {
    }

    TreeCountingFunction m_function;
    ComputedType m_computed_type;
};

}
