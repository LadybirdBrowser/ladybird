/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Steffen T. Larssen <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransformationStyleValue.h"
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/Transformation.h>

namespace Web::CSS {

Transformation TransformationStyleValue::to_transformation() const
{
    auto function_metadata = transform_function_metadata(m_properties.transform_function);
    Vector<TransformValue> values;
    size_t argument_index = 0;
    for (auto& transformation_value : m_properties.values) {
        auto const function_type = function_metadata.parameters[argument_index].type;

        if (transformation_value->is_calculated()) {
            auto& calculated = transformation_value->as_calculated();
            if (function_type == TransformFunctionParameterType::NumberPercentage
                && (calculated.resolves_to_number() || calculated.resolves_to_percentage())) {
                values.append(NumberPercentage { calculated });
            } else if (calculated.resolves_to_length_percentage()) {
                values.append(LengthPercentage { calculated });
            } else if (calculated.resolves_to_number()) {
                values.append(NumberPercentage { calculated });
            } else if (calculated.resolves_to_angle()) {
                values.append(AngleOrCalculated { calculated });
            } else {
                dbgln("FIXME: Unsupported calc value in transform! {}", calculated.to_string(SerializationMode::Normal));
            }
        } else if (transformation_value->is_length()) {
            values.append({ transformation_value->as_length().length() });
        } else if (transformation_value->is_percentage()) {
            if (function_type == TransformFunctionParameterType::NumberPercentage) {
                values.append(NumberPercentage { transformation_value->as_percentage().percentage() });
            } else {
                values.append(LengthPercentage { transformation_value->as_percentage().percentage() });
            }
        } else if (transformation_value->is_number()) {
            values.append({ Number(Number::Type::Number, transformation_value->as_number().number()) });
        } else if (transformation_value->is_angle()) {
            values.append({ transformation_value->as_angle().angle() });
        } else {
            dbgln("FIXME: Unsupported value in transform! {}", transformation_value->to_string(SerializationMode::Normal));
        }
        argument_index++;
    }
    return Transformation { m_properties.transform_function, move(values) };
}

String TransformationStyleValue::to_string(SerializationMode mode) const
{
    // https://drafts.csswg.org/css-transforms-2/#individual-transform-serialization
    if (m_properties.property == PropertyID::Rotate) {
        auto resolve_to_number = [](ValueComparingNonnullRefPtr<CSSStyleValue const> const& value) -> Optional<double> {
            if (value->is_number())
                return value->as_number().number();
            if (value->is_calculated() && value->as_calculated().resolves_to_number())
                return value->as_calculated().resolve_number({});

            VERIFY_NOT_REACHED();
        };

        // NOTE: Serialize simple rotations directly.
        switch (m_properties.transform_function) {
            // If the axis is parallel with the x or y axes, it must serialize as the appropriate keyword.
        case TransformFunction::RotateX:
            return MUST(String::formatted("x {}", m_properties.values[0]->to_string(mode)));
        case TransformFunction::RotateY:
            return MUST(String::formatted("y {}", m_properties.values[0]->to_string(mode)));

            // If a rotation about the z axis (that is, in 2D) is specified, the property must serialize as just an <angle>.
        case TransformFunction::Rotate:
        case TransformFunction::RotateZ:
            return m_properties.values[0]->to_string(mode);

        default:
            break;
        }

        auto& rotation_x = m_properties.values[0];
        auto& rotation_y = m_properties.values[1];
        auto& rotation_z = m_properties.values[2];
        auto& angle = m_properties.values[3];

        auto x_value = resolve_to_number(rotation_x).value_or(0);
        auto y_value = resolve_to_number(rotation_y).value_or(0);
        auto z_value = resolve_to_number(rotation_z).value_or(0);

        // If the axis is parallel with the x or y axes, it must serialize as the appropriate keyword.
        if (x_value > 0.0 && y_value == 0 && z_value == 0)
            return MUST(String::formatted("x {}", angle->to_string(mode)));

        if (x_value == 0 && y_value > 0.0 && z_value == 0)
            return MUST(String::formatted("y {}", angle->to_string(mode)));

        // If a rotation about the z axis (that is, in 2D) is specified, the property must serialize as just an <angle>.
        if (x_value == 0 && y_value == 0 && z_value > 0.0)
            return angle->to_string(mode);

        // It must serialize as the keyword none if and only if none was originally specified.
        // NOTE: This is handled by returning a keyword from the parser.

        // If any other rotation is specified, the property must serialize with an axis specified.
        return MUST(String::formatted("{} {} {} {}", rotation_x->to_string(mode), rotation_y->to_string(mode), rotation_z->to_string(mode), angle->to_string(mode)));
    }
    if (m_properties.property == PropertyID::Scale) {
        auto resolve_to_string = [mode](CSSStyleValue const& value) -> String {
            if (value.is_number()) {
                return MUST(String::formatted("{}", value.as_number().number()));
            }
            if (value.is_percentage()) {
                return MUST(String::formatted("{}", value.as_percentage().percentage().as_fraction()));
            }
            return value.to_string(mode);
        };

        auto x_value = resolve_to_string(m_properties.values[0]);
        auto y_value = resolve_to_string(m_properties.values[1]);
        // FIXME: 3D scaling

        StringBuilder builder;
        builder.append(x_value);
        if (x_value != y_value) {
            builder.append(" "sv);
            builder.append(y_value);
        }
        return builder.to_string_without_validation();
    }
    if (m_properties.property == PropertyID::Translate) {
        auto resolve_to_string = [mode](CSSStyleValue const& value) -> Optional<String> {
            if (value.is_length()) {
                if (value.as_length().length().raw_value() == 0)
                    return {};
            }
            if (value.is_percentage()) {
                if (value.as_percentage().percentage().value() == 0)
                    return {};
            }
            return value.to_string(mode);
        };

        auto x_value = resolve_to_string(m_properties.values[0]);
        auto y_value = resolve_to_string(m_properties.values[1]);
        // FIXME: 3D translation

        StringBuilder builder;
        builder.append(x_value.value_or("0px"_string));
        if (y_value.has_value()) {
            builder.append(" "sv);
            builder.append(y_value.value());
        }

        return builder.to_string_without_validation();
    }

    StringBuilder builder;
    builder.append(CSS::to_string(m_properties.transform_function));
    builder.append('(');
    for (size_t i = 0; i < m_properties.values.size(); ++i) {
        auto const& value = m_properties.values[i];

        // https://www.w3.org/TR/css-transforms-2/#individual-transforms
        // A <percentage> is equivalent to a <number>, for example scale: 100% is equivalent to scale: 1.
        // Numbers are used during serialization of specified and computed values.
        if ((m_properties.transform_function == CSS::TransformFunction::Scale
                || m_properties.transform_function == CSS::TransformFunction::Scale3d
                || m_properties.transform_function == CSS::TransformFunction::ScaleX
                || m_properties.transform_function == CSS::TransformFunction::ScaleY
                || m_properties.transform_function == CSS::TransformFunction::ScaleZ)
            && value->is_percentage()) {
            builder.append(String::number(value->as_percentage().percentage().as_fraction()));
        } else {
            builder.append(value->to_string(mode));
        }

        if (i != m_properties.values.size() - 1)
            builder.append(", "sv);
    }
    builder.append(')');

    return MUST(builder.to_string());
}

bool TransformationStyleValue::Properties::operator==(Properties const& other) const
{
    return property == other.property
        && transform_function == other.transform_function
        && values.span() == other.values.span();
}

}
