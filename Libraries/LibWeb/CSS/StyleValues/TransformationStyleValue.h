/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Matrix4x4.h>
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

    TransformFunction transform_function() const { return static_cast<TransformFunction>(m_value->transformation.transform_function); }
    StyleValueVector values() const
    {
        auto const& list = m_value->transformation.values;
        StyleValueVector values;
        values.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i)
            values.unchecked_append(*static_cast<StyleValue const*>(list.pointer[i].pointer));
        return values;
    }

    bool can_be_converted_to_matrix_without_reference_box() const;
    FloatMatrix4x4 to_matrix(Optional<Painting::Paintable const&>) const;

    void serialize(StringBuilder&, SerializationMode) const;
    GC::Ptr<CSSTransformComponent> reify_a_transform_function(JS::Realm&) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(TransformationStyleValue const& other) const
    {
        if (property() != other.property() || transform_function() != other.transform_function() || size() != other.size())
            return false;
        for (size_t i = 0; i < size(); ++i) {
            if (value_at(i) != other.value_at(i))
                return false;
        }
        return true;
    }

private:
    TransformationStyleValue(PropertyID property, TransformFunction transform_function, StyleValueVector&& values)
        : StyleValueWithDefaultOperators(Type::Transformation, make_transformation_data(property, transform_function, move(values)))
    {
    }

    static StyleValueFFI::StyleValueData* make_transformation_data(PropertyID property, TransformFunction transform_function, StyleValueVector&& values)
    {
        // The Rust allocation takes ownership of one strong reference to each value.
        auto pointers = leak_style_value_pointers_for_rust(values);
        return StyleValueFFI::rust_style_value_create_transformation(to_underlying(property), static_cast<u8>(to_underlying(transform_function)), pointers.data(), pointers.size());
    }

    size_t size() const { return m_value->transformation.values.length; }

    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i) const
    {
        return *static_cast<StyleValue const*>(m_value->transformation.values.pointer[i].pointer);
    }

    PropertyID property() const { return static_cast<PropertyID>(m_value->transformation.property); }
};

}
