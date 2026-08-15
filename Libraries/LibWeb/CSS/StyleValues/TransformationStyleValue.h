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

    TransformFunction transform_function() const { return static_cast<TransformFunction>(m_value->transformation.transform_function); }
    StyleValueVector values() const
    {
        return m_values;
    }

    bool can_be_converted_to_matrix_without_reference_box() const;
    FloatMatrix4x4 to_matrix(Optional<Painting::Paintable const&>) const;

    void serialize(StringBuilder&, SerializationMode) const;
    GC::Ptr<CSSTransformComponent> reify_a_transform_function() const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit TransformationStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Transformation, data)
    {
        auto const& values = data->transformation.values;
        m_values.ensure_capacity(values.length);
        for (size_t i = 0; i < values.length; ++i) {
            auto* child_data = static_cast<StyleValueFFI::StyleValueData const*>(values.pointer[i].pointer);
            m_values.unchecked_append(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data)));
        }
    }

    TransformationStyleValue(PropertyID property, TransformFunction transform_function, StyleValueVector&& values)
        : StyleValueWithDefaultOperators(Type::Transformation, make_transformation_data(property, transform_function, values))
        , m_values(move(values))
    {
    }

    static StyleValueFFI::StyleValueData const* make_transformation_data(PropertyID property, TransformFunction transform_function, StyleValueVector const& values)
    {
        Vector<StyleValueFFI::StyleValueData const*> pointers;
        pointers.ensure_capacity(values.size());
        for (auto const& value : values)
            pointers.unchecked_append(StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
        return StyleValueFFI::rust_style_value_create_transformation(to_underlying(property), static_cast<u8>(to_underlying(transform_function)), pointers.data(), pointers.size());
    }

    size_t size() const { return m_values.size(); }

    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i) const { return m_values[i]; }

    PropertyID property() const { return static_cast<PropertyID>(m_value->transformation.property); }

    StyleValueVector m_values;
};

// The transform functions of a computed or freshly parsed transform value:
// empty for the none keyword, the transformation elements otherwise.
Vector<NonnullRefPtr<TransformationStyleValue const>> transformations_for_style_value(StyleValue const&);

}
