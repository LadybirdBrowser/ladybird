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

    virtual ~TreeCountingFunctionStyleValue() override = default;

private:
    // NB: StyleValue dispatches operations by type tag, so it may call private constructors.
    friend class StyleValue;

    TreeCountingFunction function() const { return static_cast<TreeCountingFunction>(m_value->tree_counting_function.function); }
    ComputedType computed_type() const { return static_cast<ComputedType>(m_value->tree_counting_function.computed_type); }

    explicit TreeCountingFunctionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::TreeCountingFunction, data)
    {
    }
};

}
