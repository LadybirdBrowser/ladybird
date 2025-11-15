/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/TransformFunctions.h>

namespace Web::CSS {

class TransformationStyleValue final : public StyleValueWithDefaultOperators<TransformationStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TransformationStyleValue const> create(PropertyID property, TransformFunction transform_function, StyleValueVector&& values)
    {
        return adopt_ref(*new (nothrow) TransformationStyleValue(property, transform_function, move(values)));
    }
    virtual ~TransformationStyleValue() override = default;

    static ValueComparingNonnullRefPtr<TransformationStyleValue const> identity_transformation(TransformFunction);

    TransformFunction transform_function() const { return m_properties.transform_function; }
    StyleValueVector const& values() const { return m_properties.values; }

    Transformation to_transformation() const;

    virtual String to_string(SerializationMode) const override;
    GC::Ref<CSSTransformComponent> reify_a_transform_function(JS::Realm&) const;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(TransformationStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    TransformationStyleValue(PropertyID property, TransformFunction transform_function, StyleValueVector&& values)
        : StyleValueWithDefaultOperators(Type::Transformation)
        , m_properties { .property = property, .transform_function = transform_function, .values = move(values) }
    {
    }

    struct Properties {
        PropertyID property;
        TransformFunction transform_function;
        StyleValueVector values;
        bool operator==(Properties const& other) const;
    } m_properties;
};

}
