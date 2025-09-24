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
#include <LibWeb/CSS/CSSMatrixComponent.h>
#include <LibWeb/CSS/CSSPerspective.h>
#include <LibWeb/CSS/CSSRotate.h>
#include <LibWeb/CSS/CSSScale.h>
#include <LibWeb/CSS/CSSSkew.h>
#include <LibWeb/CSS/CSSSkewX.h>
#include <LibWeb/CSS/CSSSkewY.h>
#include <LibWeb/CSS/CSSTransformComponent.h>
#include <LibWeb/CSS/CSSTranslate.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/Transformation.h>
#include <LibWeb/Geometry/DOMMatrix.h>

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
        auto resolve_to_number = [](ValueComparingNonnullRefPtr<StyleValue const> const& value) -> Optional<double> {
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
        auto resolve_to_string = [mode](StyleValue const& value) -> String {
            Optional<double> raw_value;

            if (value.is_number())
                raw_value = value.as_number().number();
            if (value.is_percentage())
                raw_value = value.as_percentage().percentage().as_fraction();
            if (value.is_calculated()) {
                if (value.as_calculated().resolves_to_number()) {
                    if (auto resolved = value.as_calculated().resolve_number({}); resolved.has_value())
                        raw_value = *resolved;
                }
                if (value.as_calculated().resolves_to_percentage()) {
                    if (auto resolved = value.as_calculated().resolve_percentage({}); resolved.has_value())
                        raw_value = resolved->as_fraction();
                }
            }

            if (!raw_value.has_value())
                return value.to_string(mode);

            return serialize_a_number(*raw_value);
        };

        auto x_value = resolve_to_string(m_properties.values[0]);
        auto y_value = resolve_to_string(m_properties.values[1]);
        Optional<String> z_value;
        if (m_properties.values.size() == 3 && (!m_properties.values[2]->is_number() || m_properties.values[2]->as_number().number() != 1))
            z_value = resolve_to_string(m_properties.values[2]);

        StringBuilder builder;
        builder.append(x_value);
        if (x_value != y_value || (z_value.has_value() && *z_value != "1"sv)) {
            builder.append(" "sv);
            builder.append(y_value);
        }
        if (z_value.has_value() && *z_value != "1"sv) {
            builder.append(" "sv);
            builder.append(z_value.value());
        }
        return builder.to_string_without_validation();
    }
    if (m_properties.property == PropertyID::Translate) {
        auto resolve_to_string = [mode](StyleValue const& value) -> Optional<String> {
            auto string_value = value.to_string(mode);

            if (string_value == "0px"_string)
                return {};

            return string_value;
        };

        auto x_value = resolve_to_string(m_properties.values[0]);
        auto y_value = resolve_to_string(m_properties.values[1]);
        Optional<String> z_value;
        if (m_properties.values.size() == 3 && (!m_properties.values[2]->is_length() || m_properties.values[2]->as_length().length() != Length::make_px(0)))
            z_value = resolve_to_string(m_properties.values[2]);

        StringBuilder builder;
        builder.append(x_value.value_or("0px"_string));
        if (y_value.has_value() || z_value.has_value()) {
            builder.append(" "sv);
            builder.append(y_value.value_or("0px"_string));
        }
        if (z_value.has_value()) {
            builder.append(" "sv);
            builder.append(z_value.value());
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

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-transform-function
GC::Ref<CSSTransformComponent> TransformationStyleValue::reify_a_transform_function(JS::Realm& realm) const
{
    auto reify_numeric_argument = [&](size_t index) {
        return GC::Ref { as<CSSNumericValue>(*m_properties.values[index]->reify(realm, {})) };
    };
    auto reify_0 = [&] { return CSSUnitValue::create(realm, 0, "number"_fly_string); };
    auto reify_1 = [&] { return CSSUnitValue::create(realm, 1, "number"_fly_string); };
    auto reify_0px = [&] { return CSSUnitValue::create(realm, 0, "px"_fly_string); };
    auto reify_0deg = [&] { return CSSUnitValue::create(realm, 0, "deg"_fly_string); };

    // To reify a <transform-function> func, perform the appropriate set of steps below, based on func:
    switch (m_properties.transform_function) {
    // -> matrix()
    // -> matrix3d()
    //    1. Return a new CSSMatrixComponent object, whose matrix internal slot is set to a 4x4 matrix representing the
    //       same information as func, and whose is2D internal slot is true if func is matrix(), and false otherwise.
    case TransformFunction::Matrix:
    case TransformFunction::Matrix3d: {
        auto transform_as_matrix = MUST(to_transformation().to_matrix({}));
        auto matrix = Geometry::DOMMatrix::create(realm);
        matrix->set_m11(transform_as_matrix[0, 0]);
        matrix->set_m12(transform_as_matrix[1, 0]);
        matrix->set_m13(transform_as_matrix[2, 0]);
        matrix->set_m14(transform_as_matrix[3, 0]);
        matrix->set_m21(transform_as_matrix[0, 1]);
        matrix->set_m22(transform_as_matrix[1, 1]);
        matrix->set_m23(transform_as_matrix[2, 1]);
        matrix->set_m24(transform_as_matrix[3, 1]);
        matrix->set_m31(transform_as_matrix[0, 2]);
        matrix->set_m32(transform_as_matrix[1, 2]);
        matrix->set_m33(transform_as_matrix[2, 2]);
        matrix->set_m34(transform_as_matrix[3, 2]);
        matrix->set_m41(transform_as_matrix[0, 3]);
        matrix->set_m42(transform_as_matrix[1, 3]);
        matrix->set_m43(transform_as_matrix[2, 3]);
        matrix->set_m44(transform_as_matrix[3, 3]);

        auto is_2d = m_properties.transform_function == TransformFunction::Matrix ? CSSTransformComponent::Is2D::Yes : CSSTransformComponent::Is2D::No;
        return CSSMatrixComponent::create(realm, is_2d, matrix);
    }

    // -> translate()
    // -> translateX()
    // -> translateY()
    // -> translate3d()
    // -> translateZ()
    //    1. Return a new CSSTranslate object, whose x, y, and z internal slots are set to the reification of the
    //       specified x/y/z offsets, or the reification of 0px if not specified in func, and whose is2D internal slot
    //       is true if func is translate(), translateX(), or translateY(), and false otherwise.
    case TransformFunction::Translate: {
        // NB: Default y to 0px if it's not specified.
        auto y = m_properties.values.size() > 1 ? reify_numeric_argument(1) : reify_0px();
        return CSSTranslate::create(realm, CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), y, reify_0px());
    }
    case TransformFunction::TranslateX:
        return CSSTranslate::create(realm, CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), reify_0px(), reify_0px());
    case TransformFunction::TranslateY:
        return CSSTranslate::create(realm, CSSTransformComponent::Is2D::Yes, reify_0px(), reify_numeric_argument(0), reify_0px());
    case TransformFunction::Translate3d:
        return CSSTranslate::create(realm, CSSTransformComponent::Is2D::No, reify_numeric_argument(0), reify_numeric_argument(1), reify_numeric_argument(2));
    case TransformFunction::TranslateZ:
        return CSSTranslate::create(realm, CSSTransformComponent::Is2D::No, reify_0px(), reify_0px(), reify_numeric_argument(0));

    // -> scale()
    // -> scaleX()
    // -> scaleY()
    // -> scale3d()
    // -> scaleZ()
    //    1. Return a new CSSScale object, whose x, y, and z internal slots are set to the specified x/y/z scales, or
    //       to 1 if not specified in func and whose is2D internal slot is true if func is scale(), scaleX(), or
    //       scaleY(), and false otherwise.
    case TransformFunction::Scale: {
        // NB: Default y to a copy of x if it's not specified.
        auto y = m_properties.values.size() > 1 ? reify_numeric_argument(1) : reify_numeric_argument(0);
        return CSSScale::create(realm, CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), y, reify_1());
    }
    case TransformFunction::ScaleX:
        return CSSScale::create(realm, CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), reify_1(), reify_1());
    case TransformFunction::ScaleY:
        return CSSScale::create(realm, CSSTransformComponent::Is2D::Yes, reify_1(), reify_numeric_argument(0), reify_1());
    case TransformFunction::Scale3d:
        return CSSScale::create(realm, CSSTransformComponent::Is2D::No, reify_numeric_argument(0), reify_numeric_argument(1), reify_numeric_argument(2));
    case TransformFunction::ScaleZ:
        return CSSScale::create(realm, CSSTransformComponent::Is2D::No, reify_1(), reify_1(), reify_numeric_argument(0));

    // -> rotate()
    // -> rotate3d()
    // -> rotateX()
    // -> rotateY()
    // -> rotateZ()
    //    1. Return a new CSSRotate object, whose angle internal slot is set to the reification of the specified angle,
    //       and whose x, y, and z internal slots are set to the specified rotation axis coordinates, or the implicit
    //       axis coordinates if not specified in func and whose is2D internal slot is true if func is rotate(), and
    //       false otherwise.
    case TransformFunction::Rotate:
        return CSSRotate::create(realm, CSSTransformComponent::Is2D::Yes, reify_0(), reify_0(), reify_1(), reify_numeric_argument(0));
    case TransformFunction::Rotate3d:
        return CSSRotate::create(realm, CSSTransformComponent::Is2D::No, reify_numeric_argument(0), reify_numeric_argument(1), reify_numeric_argument(2), reify_numeric_argument(3));
    case TransformFunction::RotateX:
        return CSSRotate::create(realm, CSSTransformComponent::Is2D::No, reify_1(), reify_0(), reify_0(), reify_numeric_argument(0));
    case TransformFunction::RotateY:
        return CSSRotate::create(realm, CSSTransformComponent::Is2D::No, reify_0(), reify_1(), reify_0(), reify_numeric_argument(0));
    case TransformFunction::RotateZ:
        return CSSRotate::create(realm, CSSTransformComponent::Is2D::No, reify_0(), reify_0(), reify_1(), reify_numeric_argument(0));

    // -> skew()
    //    1. Return a new CSSSkew object, whose ax and ay internal slots are set to the reification of the specified x
    //       and y angles, or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
    case TransformFunction::Skew: {
        // NB: Default y to 0deg if it's not specified.
        auto y = m_properties.values.size() > 1 ? reify_numeric_argument(1) : reify_0deg();
        return CSSSkew::create(realm, reify_numeric_argument(0), y);
    }

    // -> skewX()
    //    1. Return a new CSSSkewX object, whose ax internal slot is set to the reification of the specified x angle,
    //       or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
    case TransformFunction::SkewX:
        return CSSSkewX::create(realm, reify_numeric_argument(0));

    // -> skewY()
    //    1. Return a new CSSSkewY object, whose ay internal slot is set to the reification of the specified y angle,
    //       or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
    case TransformFunction::SkewY:
        return CSSSkewY::create(realm, reify_numeric_argument(0));

    // -> perspective()
    //    1. Return a new CSSPerspective object, whose length internal slot is set to the reification of the specified
    //       length (see reify a numeric value if it is a length, and reify an identifier if it is the keyword none)
    //       and whose is2D internal slot is false.
    case TransformFunction::Perspective: {
        CSSPerspectiveValueInternal length = [&]() -> CSSPerspectiveValueInternal {
            auto reified = m_properties.values[0]->reify(realm, {});
            if (auto* keyword = as_if<CSSKeywordValue>(*reified))
                return GC::Ref { *keyword };
            if (auto* numeric = as_if<CSSNumericValue>(*reified))
                return GC::Ref { *numeric };
            VERIFY_NOT_REACHED();
        }();
        return CSSPerspective::create(realm, length);
    }
    }
    VERIFY_NOT_REACHED();
}

bool TransformationStyleValue::Properties::operator==(Properties const& other) const
{
    return property == other.property
        && transform_function == other.transform_function
        && values.span() == other.values.span();
}

}
