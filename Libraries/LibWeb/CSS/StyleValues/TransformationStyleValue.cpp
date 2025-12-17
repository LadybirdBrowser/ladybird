/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Steffen T. Larssen <dudedbz@gmail.com>
 * Copyright (c) 2024-2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<TransformationStyleValue const> TransformationStyleValue::identity_transformation(
    TransformFunction transform_function)
{
    // https://drafts.csswg.org/css-transforms-1/#identity-transform-function
    // A transform function that is equivalent to a identity 4x4 matrix (see Mathematical Description of Transform
    // Functions). Examples for identity transform functions are translate(0), translateX(0), translateY(0), scale(1),
    // scaleX(1), scaleY(1), rotate(0), skew(0, 0), skewX(0), skewY(0) and matrix(1, 0, 0, 1, 0, 0).

    // https://drafts.csswg.org/css-transforms-2/#identity-transform-function
    // In addition to the identity transform function in CSS Transforms, examples for identity transform functions
    // include translate3d(0, 0, 0), translateZ(0), scaleZ(1), rotate3d(1, 1, 1, 0), rotateX(0), rotateY(0), rotateZ(0)
    // and matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1). A special case is perspective: perspective(none).
    // The value of m34 becomes infinitesimal small and the transform function is therefore assumed to be equal to the
    // identity matrix.

    auto identity_parameters = [&] -> StyleValueVector {
        auto const number_zero = NumberStyleValue::create(0.);
        auto const number_one = NumberStyleValue::create(1.);

        switch (transform_function) {
        case TransformFunction::Matrix:
            return { number_one, number_zero, number_zero, number_one, number_zero, number_zero };
        case TransformFunction::Matrix3d:
            return { number_one, number_zero, number_zero, number_zero,
                number_zero, number_one, number_zero, number_zero,
                number_zero, number_zero, number_one, number_zero,
                number_zero, number_zero, number_zero, number_one };
        case TransformFunction::Perspective:
            return { KeywordStyleValue::create(Keyword::None) };
        case TransformFunction::Rotate:
        case TransformFunction::RotateX:
        case TransformFunction::RotateY:
        case TransformFunction::RotateZ:
            return { AngleStyleValue::create(Angle::make_degrees(0.)) };
        case TransformFunction::Rotate3d:
            return { number_one, number_one, number_one, AngleStyleValue::create(Angle::make_degrees(0.)) };
        case TransformFunction::Skew:
        case TransformFunction::SkewX:
        case TransformFunction::SkewY:
        case TransformFunction::Translate:
        case TransformFunction::TranslateX:
        case TransformFunction::TranslateY:
        case TransformFunction::TranslateZ:
            return { LengthStyleValue::create(Length::make_px(0.)) };
        case TransformFunction::Translate3d:
            return {
                LengthStyleValue::create(Length::make_px(0.)),
                LengthStyleValue::create(Length::make_px(0.)),
                LengthStyleValue::create(Length::make_px(0.)),
            };
        case TransformFunction::Scale:
        case TransformFunction::ScaleX:
        case TransformFunction::ScaleY:
        case TransformFunction::ScaleZ:
            return { number_one };
        case TransformFunction::Scale3d:
            return { number_one, number_one, number_one };
        }
        VERIFY_NOT_REACHED();
    };
    return create(PropertyID::Transform, transform_function, identity_parameters());
}

ErrorOr<FloatMatrix4x4> TransformationStyleValue::to_matrix(Optional<Painting::PaintableBox const&> paintable_box) const
{
    auto count = m_properties.values.size();
    auto function_metadata = transform_function_metadata(m_properties.transform_function);

    auto length_to_px = [&](Length const& length) -> ErrorOr<float> {
        if (paintable_box.has_value())
            return length.to_px(paintable_box->layout_node()).to_float();
        if (length.is_absolute())
            return length.absolute_length_to_px().to_float();
        return Error::from_string_literal("Transform contains non absolute units");
    };

    auto get_value = [&](size_t argument_index, Optional<CSSPixels> reference_length = {}) -> ErrorOr<float> {
        auto& transformation_value = *m_properties.values[argument_index];
        CalculationResolutionContext context;
        if (paintable_box.has_value())
            context.length_resolution_context = Length::ResolutionContext::for_layout_node(paintable_box->layout_node());
        if (reference_length.has_value())
            context.percentage_basis = Length::make_px(reference_length.value());

        if (transformation_value.is_calculated()) {
            auto& calculated = transformation_value.as_calculated();
            switch (function_metadata.parameters[argument_index].type) {
            case TransformFunctionParameterType::Angle: {
                if (!calculated.resolves_to_angle())
                    return Error::from_string_literal("Calculated angle parameter to transform function doesn't resolve to an angle.");
                if (auto resolved = calculated.resolve_angle(context); resolved.has_value())
                    return resolved->to_radians();
                return Error::from_string_literal("Couldn't resolve calculated angle.");
            }
            case TransformFunctionParameterType::Length:
            case TransformFunctionParameterType::LengthNone: {
                if (!calculated.resolves_to_length())
                    return Error::from_string_literal("Calculated length parameter to transform function doesn't resolve to a length.");
                if (auto resolved = calculated.resolve_length(context); resolved.has_value())
                    return length_to_px(resolved.value());
                return Error::from_string_literal("Couldn't resolve calculated length.");
            }
            case TransformFunctionParameterType::LengthPercentage: {
                if (!calculated.resolves_to_length_percentage())
                    return Error::from_string_literal("Calculated length-percentage parameter to transform function doesn't resolve to a length-percentage.");
                if (auto resolved = calculated.resolve_length(context); resolved.has_value())
                    return length_to_px(resolved.value());
                return Error::from_string_literal("Couldn't resolve calculated length-percentage.");
            }
            case TransformFunctionParameterType::Number: {
                if (!calculated.resolves_to_number())
                    return Error::from_string_literal("Calculated number parameter to transform function doesn't resolve to a number.");
                if (auto resolved = calculated.resolve_number(context); resolved.has_value())
                    return resolved.release_value();
                return Error::from_string_literal("Couldn't resolve calculated number.");
            }
            case TransformFunctionParameterType::NumberPercentage: {
                if (calculated.resolves_to_number()) {
                    if (auto resolved = calculated.resolve_number(context); resolved.has_value())
                        return calculated.resolve_number(context).value();
                    return Error::from_string_literal("Couldn't resolve calculated number.");
                }
                if (calculated.resolves_to_percentage()) {
                    if (auto resolved = calculated.resolve_percentage(context); resolved.has_value())
                        return calculated.resolve_percentage(context).value().as_fraction();
                    return Error::from_string_literal("Couldn't resolve calculated percentage.");
                }
                return Error::from_string_literal("Calculated number/percentage parameter to transform function doesn't resolve to a number or percentage.");
            }
            }
        }

        if (transformation_value.is_length())
            return length_to_px(transformation_value.as_length().length());

        if (transformation_value.is_percentage()) {
            if (function_metadata.parameters[argument_index].type == TransformFunctionParameterType::NumberPercentage) {
                return transformation_value.as_percentage().percentage().as_fraction();
            }
            if (!reference_length.has_value())
                return Error::from_string_literal("Can't resolve percentage to length without a reference value.");
            return length_to_px(Length::make_px(reference_length.value()).percentage_of(transformation_value.as_percentage().percentage()));
        }

        if (transformation_value.is_number())
            return transformation_value.as_number().number();

        if (transformation_value.is_angle())
            return transformation_value.as_angle().angle().to_radians();

        dbgln("FIXME: Unsupported value in transform! {}", transformation_value.to_string(SerializationMode::Normal));
        return Error::from_string_literal("Unsupported value in transform function");
    };

    Optional<CSSPixels> width;
    Optional<CSSPixels> height;
    if (paintable_box.has_value()) {
        auto reference_box = paintable_box->transform_reference_box();
        width = reference_box.width();
        height = reference_box.height();
    }

    switch (m_properties.transform_function) {
    case TransformFunction::Perspective:
        // https://drafts.csswg.org/css-transforms-2/#perspective
        if (count == 1) {
            if (m_properties.values.first()->to_keyword() == Keyword::None)
                return FloatMatrix4x4::identity();

            // FIXME: Add support for the 'perspective-origin' CSS property.
            auto distance = TRY(get_value(0));
            // If the depth value is less than '1px', it must be treated as '1px' for the purpose of rendering, for
            // computing the resolved value of 'transform', and when used as the endpoint of interpolation.
            // Note: The intent of the above rules on values less than '1px' is that they cover the cases where
            // the 'perspective()' function needs to be converted into a matrix.
            if (distance < 1)
                distance = 1;
            return FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, -1 / distance, 1);
        }
        break;
    case TransformFunction::Matrix:
        if (count == 6)
            return FloatMatrix4x4(TRY(get_value(0)), TRY(get_value(2)), 0, TRY(get_value(4)),
                TRY(get_value(1)), TRY(get_value(3)), 0, TRY(get_value(5)),
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::Matrix3d:
        if (count == 16)
            return FloatMatrix4x4(TRY(get_value(0)), TRY(get_value(4)), TRY(get_value(8)), TRY(get_value(12)),
                TRY(get_value(1)), TRY(get_value(5)), TRY(get_value(9)), TRY(get_value(13)),
                TRY(get_value(2)), TRY(get_value(6)), TRY(get_value(10)), TRY(get_value(14)),
                TRY(get_value(3)), TRY(get_value(7)), TRY(get_value(11)), TRY(get_value(15)));
        break;
    case TransformFunction::Translate:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, TRY(get_value(0, width)),
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        if (count == 2)
            return FloatMatrix4x4(1, 0, 0, TRY(get_value(0, width)),
                0, 1, 0, TRY(get_value(1, height)),
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::Translate3d:
        return FloatMatrix4x4(1, 0, 0, TRY(get_value(0, width)),
            0, 1, 0, TRY(get_value(1, height)),
            0, 0, 1, TRY(get_value(2)),
            0, 0, 0, 1);
        break;
    case TransformFunction::TranslateX:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, TRY(get_value(0, width)),
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::TranslateY:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, TRY(get_value(0, height)),
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::TranslateZ:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, TRY(get_value(0)),
                0, 0, 0, 1);
        break;
    case TransformFunction::Scale:
        if (count == 1)
            return FloatMatrix4x4(TRY(get_value(0)), 0, 0, 0,
                0, TRY(get_value(0)), 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        if (count == 2)
            return FloatMatrix4x4(TRY(get_value(0)), 0, 0, 0,
                0, TRY(get_value(1)), 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::Scale3d:
        if (count == 3)
            return FloatMatrix4x4(TRY(get_value(0)), 0, 0, 0,
                0, TRY(get_value(1)), 0, 0,
                0, 0, TRY(get_value(2)), 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::ScaleX:
        if (count == 1)
            return FloatMatrix4x4(TRY(get_value(0)), 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::ScaleY:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, 0,
                0, TRY(get_value(0)), 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::ScaleZ:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, TRY(get_value(0)), 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::Rotate3d:
        if (count == 4)
            return Gfx::rotation_matrix({ TRY(get_value(0)), TRY(get_value(1)), TRY(get_value(2)) }, TRY(get_value(3)));
        break;
    case TransformFunction::RotateX:
        if (count == 1)
            return Gfx::rotation_matrix({ 1.0f, 0.0f, 0.0f }, TRY(get_value(0)));
        break;
    case TransformFunction::RotateY:
        if (count == 1)
            return Gfx::rotation_matrix({ 0.0f, 1.0f, 0.0f }, TRY(get_value(0)));
        break;
    case TransformFunction::Rotate:
    case TransformFunction::RotateZ:
        if (count == 1)
            return Gfx::rotation_matrix({ 0.0f, 0.0f, 1.0f }, TRY(get_value(0)));
        break;
    case TransformFunction::Skew:
        if (count == 1)
            return FloatMatrix4x4(1, tanf(TRY(get_value(0))), 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        if (count == 2)
            return FloatMatrix4x4(1, tanf(TRY(get_value(0))), 0, 0,
                tanf(TRY(get_value(1))), 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::SkewX:
        if (count == 1)
            return FloatMatrix4x4(1, tanf(TRY(get_value(0))), 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case TransformFunction::SkewY:
        if (count == 1)
            return FloatMatrix4x4(1, 0, 0, 0,
                tanf(TRY(get_value(0))), 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    }
    dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Unhandled transformation function {} with {} arguments", CSS::to_string(m_properties.transform_function), m_properties.values.size());
    return FloatMatrix4x4::identity();
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
ErrorOr<GC::Ref<CSSTransformComponent>> TransformationStyleValue::reify_a_transform_function(JS::Realm& realm) const
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
        auto transform_as_matrix = TRY(to_matrix({}));
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

ValueComparingNonnullRefPtr<StyleValue const> TransformationStyleValue::absolutized(ComputationContext const& computation_context) const
{
    StyleValueVector absolutized_values;

    bool absolutized_values_different = false;

    for (auto const& value : m_properties.values) {
        auto const& absolutized_value = value->absolutized(computation_context);

        if (absolutized_value != value)
            absolutized_values_different = true;

        absolutized_values.append(absolutized_value);
    }

    if (!absolutized_values_different)
        return *this;

    return TransformationStyleValue::create(m_properties.property, m_properties.transform_function, move(absolutized_values));
}

bool TransformationStyleValue::Properties::operator==(Properties const& other) const
{
    return property == other.property
        && transform_function == other.transform_function
        && values.span() == other.values.span();
}

}
