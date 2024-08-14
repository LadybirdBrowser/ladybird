/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/TransformFunctions.h>

namespace Web::CSS {

class TransformationStyleValue final : public StyleValueWithDefaultOperators<TransformationStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TransformationStyleValue> create(CSS::TransformFunction transform_function, StyleValueVector&& values)
    {
        return adopt_ref(*new (nothrow) TransformationStyleValue(transform_function, move(values)));
    }
    virtual ~TransformationStyleValue() override = default;

    CSS::TransformFunction transform_function() const { return m_properties.transform_function; }
    StyleValueVector values() const { return m_properties.values; }

    virtual String to_string() const override;

    bool properties_equal(TransformationStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    TransformationStyleValue(CSS::TransformFunction transform_function, StyleValueVector&& values)
        : StyleValueWithDefaultOperators(Type::Transformation)
        , m_properties { .transform_function = transform_function, .values = move(values) }
    {
    }

    struct Properties {
        CSS::TransformFunction transform_function;
        StyleValueVector values;
        bool operator==(Properties const& other) const;
    } m_properties;
};

}
