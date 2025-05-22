/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Interpolation.h"
#include <AK/IntegralMath.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/Transformation.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

template<typename T>
static T interpolate_raw(T from, T to, float delta)
{
    if constexpr (AK::Detail::IsSame<T, double>) {
        return from + (to - from) * static_cast<double>(delta);
    } else if constexpr (AK::Detail::IsIntegral<T>) {
        auto from_float = static_cast<float>(from);
        auto to_float = static_cast<float>(to);
        auto unclamped_result = from_float + (to_float - from_float) * delta;
        return static_cast<AK::Detail::RemoveCVReference<T>>(clamp(unclamped_result, NumericLimits<T>::min(), NumericLimits<T>::max()));
    }
    return static_cast<AK::Detail::RemoveCVReference<T>>(from + (to - from) * delta);
}

static NonnullRefPtr<CSSStyleValue const> with_keyword_values_resolved(DOM::Element& element, PropertyID property_id, CSSStyleValue const& value)
{
    if (value.is_guaranteed_invalid()) {
        // At the moment, we're only dealing with "real" properties, so this behaves the same as `unset`.
        // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
        return property_initial_value(property_id);
    }

    if (!value.is_keyword())
        return value;
    switch (value.as_keyword().keyword()) {
    case Keyword::Initial:
    case Keyword::Unset:
        return property_initial_value(property_id);
    case Keyword::Inherit:
        return StyleComputer::get_inherit_value(property_id, &element);
    default:
        break;
    }
    return value;
}

static RefPtr<CSSStyleValue const> interpolate_discrete(CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (from.equals(to))
        return from;
    if (allow_discrete == AllowDiscrete::No)
        return {};
    return delta >= 0.5f ? to : from;
}

static RefPtr<CSSStyleValue const> interpolate_scale(DOM::Element& element, CalculationContext calculation_context, CSSStyleValue const& a_from, CSSStyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    if (a_from.to_keyword() == Keyword::None && a_to.to_keyword() == Keyword::None)
        return a_from;

    static auto one = TransformationStyleValue::create(PropertyID::Scale, TransformFunction::Scale, { NumberStyleValue::create(1), NumberStyleValue::create(1) });

    auto const& from = a_from.to_keyword() == Keyword::None ? *one : a_from;
    auto const& to = a_to.to_keyword() == Keyword::None ? *one : a_to;

    auto const& from_transform = from.as_transformation();
    auto const& to_transform = to.as_transformation();

    auto interpolated_x = interpolate_value(element, calculation_context, from_transform.values()[0], to_transform.values()[0], delta, allow_discrete);
    if (!interpolated_x)
        return {};
    auto interpolated_y = interpolate_value(element, calculation_context, from_transform.values()[1], to_transform.values()[1], delta, allow_discrete);
    if (!interpolated_y)
        return {};
    RefPtr<CSSStyleValue const> interpolated_z;

    if (from_transform.values().size() == 3 || to_transform.values().size() == 3) {
        static auto one_value = NumberStyleValue::create(1);
        auto from = from_transform.values().size() == 3 ? from_transform.values()[2] : one_value;
        auto to = to_transform.values().size() == 3 ? to_transform.values()[2] : one_value;
        interpolated_z = interpolate_value(element, calculation_context, from, to, delta, allow_discrete);
        if (!interpolated_z)
            return {};
    }

    StyleValueVector new_values = { *interpolated_x, *interpolated_y };
    if (interpolated_z && interpolated_z->is_number() && interpolated_z->as_number().number() != 1) {
        new_values.append(*interpolated_z);
    }

    return TransformationStyleValue::create(
        PropertyID::Scale,
        TransformFunction::Scale,
        move(new_values));
}

static RefPtr<CSSStyleValue const> interpolate_translate(DOM::Element& element, CalculationContext calculation_context, CSSStyleValue const& a_from, CSSStyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    if (a_from.to_keyword() == Keyword::None && a_to.to_keyword() == Keyword::None)
        return a_from;

    static auto zero_px = LengthStyleValue::create(Length::make_px(0));
    static auto zero = TransformationStyleValue::create(PropertyID::Translate, TransformFunction::Translate, { zero_px, zero_px });

    auto const& from = a_from.to_keyword() == Keyword::None ? *zero : a_from;
    auto const& to = a_to.to_keyword() == Keyword::None ? *zero : a_to;

    auto const& from_transform = from.as_transformation();
    auto const& to_transform = to.as_transformation();

    auto interpolated_x = interpolate_value(element, calculation_context, from_transform.values()[0], to_transform.values()[0], delta, allow_discrete);
    if (!interpolated_x)
        return {};
    auto interpolated_y = interpolate_value(element, calculation_context, from_transform.values()[1], to_transform.values()[1], delta, allow_discrete);
    if (!interpolated_y)
        return {};

    RefPtr<CSSStyleValue const> interpolated_z;

    if (from_transform.values().size() == 3 || to_transform.values().size() == 3) {
        auto from_z = from_transform.values().size() == 3 ? from_transform.values()[2] : zero_px;
        auto to_z = to_transform.values().size() == 3 ? to_transform.values()[2] : zero_px;
        interpolated_z = interpolate_value(element, calculation_context, from_z, to_z, delta, allow_discrete);
        if (!interpolated_z)
            return {};
    }

    StyleValueVector new_values = { *interpolated_x, *interpolated_y };
    if (interpolated_z && interpolated_z->is_length() && !interpolated_z->as_length().equals(zero_px)) {
        new_values.append(*interpolated_z);
    }

    return TransformationStyleValue::create(
        PropertyID::Translate,
        new_values.size() == 3 ? TransformFunction::Translate3d : TransformFunction::Translate,
        move(new_values));
}

ValueComparingRefPtr<CSSStyleValue const> interpolate_property(DOM::Element& element, PropertyID property_id, CSSStyleValue const& a_from, CSSStyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    auto from = with_keyword_values_resolved(element, property_id, a_from);
    auto to = with_keyword_values_resolved(element, property_id, a_to);

    CalculationContext calculation_context {
        .percentages_resolve_as = property_resolves_percentages_relative_to(property_id),
    };

    auto animation_type = animation_type_from_longhand_property(property_id);
    switch (animation_type) {
    case AnimationType::ByComputedValue:
        return interpolate_value(element, calculation_context, from, to, delta, allow_discrete);
    case AnimationType::None:
        return to;
    case AnimationType::RepeatableList:
        return interpolate_repeatable_list(element, calculation_context, from, to, delta, allow_discrete);
    case AnimationType::Custom: {
        if (property_id == PropertyID::Transform) {
            if (auto interpolated_transform = interpolate_transform(element, from, to, delta, allow_discrete))
                return *interpolated_transform;

            // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
            // In some cases, an animation might cause a transformation matrix to be singular or non-invertible.
            // For example, an animation in which scale moves from 1 to -1. At the time when the matrix is in
            // such a state, the transformed element is not rendered.
            return {};
        }
        if (property_id == PropertyID::BoxShadow) {
            if (auto interpolated_box_shadow = interpolate_box_shadow(element, calculation_context, from, to, delta, allow_discrete))
                return *interpolated_box_shadow;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::FontStyle) {
            auto static oblique_0deg_value = FontStyleStyleValue::create(FontStyle::Oblique, AngleStyleValue::create(Angle::make_degrees(0)));
            auto from_value = from->as_font_style().font_style() == FontStyle::Normal ? oblique_0deg_value : from;
            auto to_value = to->as_font_style().font_style() == FontStyle::Normal ? oblique_0deg_value : to;
            return interpolate_value(element, calculation_context, from_value, to_value, delta, allow_discrete);
        }

        // https://drafts.csswg.org/web-animations-1/#animating-visibility
        if (property_id == PropertyID::Visibility) {
            // For the visibility property, visible is interpolated as a discrete step where values of p between 0 and 1 map to visible and other values of p map to the closer endpoint.
            // If neither value is visible, then discrete animation is used.
            if (from->equals(to))
                return from;

            auto from_is_visible = from->to_keyword() == Keyword::Visible;
            auto to_is_visible = to->to_keyword() == Keyword::Visible;

            if (from_is_visible || to_is_visible) {
                if (delta <= 0)
                    return from;
                if (delta >= 1)
                    return to;
                return CSSKeywordValue::create(Keyword::Visible);
            }

            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        // https://drafts.csswg.org/css-contain/#content-visibility-animation
        if (property_id == PropertyID::ContentVisibility) {
            // In general, the content-visibility property’s animation type is discrete.
            // However, similar to interpolation of visibility, during interpolation between hidden and any other content-visibility value,
            // p values between 0 and 1 map to the non-hidden value.
            if (from->equals(to))
                return from;

            auto from_is_hidden = from->to_keyword() == Keyword::Hidden;
            auto to_is_hidden = to->to_keyword() == Keyword::Hidden || to->to_keyword() == Keyword::Auto;

            if (from_is_hidden || to_is_hidden) {
                auto non_hidden_value = from_is_hidden ? to : from;
                if (delta <= 0)
                    return from;
                if (delta >= 1)
                    return to;
                return non_hidden_value;
            }
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::Scale) {
            if (auto result = interpolate_scale(element, calculation_context, from, to, delta, allow_discrete))
                return result;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::Translate) {
            if (auto result = interpolate_translate(element, calculation_context, from, to, delta, allow_discrete))
                return result;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        // FIXME: Handle all custom animatable properties
        [[fallthrough]];
    }
    case AnimationType::Discrete:
    default:
        return interpolate_discrete(from, to, delta, allow_discrete);
    }
}

// https://drafts.csswg.org/css-transitions/#transitionable
bool property_values_are_transitionable(PropertyID property_id, CSSStyleValue const& old_value, CSSStyleValue const& new_value, DOM::Element& element, TransitionBehavior transition_behavior)
{
    // When comparing the before-change style and after-change style for a given property,
    // the property values are transitionable if they have an animation type that is neither not animatable nor discrete.

    auto animation_type = animation_type_from_longhand_property(property_id);
    if (animation_type == AnimationType::None || (transition_behavior != TransitionBehavior::AllowDiscrete && animation_type == AnimationType::Discrete))
        return false;

    // Even when a property is transitionable, the two values may not be. The spec uses the example of inset/non-inset shadows.
    if (transition_behavior != TransitionBehavior::AllowDiscrete && !interpolate_property(element, property_id, old_value, new_value, 0.5f, AllowDiscrete::No))
        return false;

    return true;
}

// A null return value means the interpolated matrix was not invertible or otherwise invalid
RefPtr<CSSStyleValue const> interpolate_transform(DOM::Element& element, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete)
{
    // Note that the spec uses column-major notation, so all the matrix indexing is reversed.

    static constexpr auto make_transformation = [](TransformationStyleValue const& transformation) -> Optional<Transformation> {
        Vector<TransformValue> values;

        for (auto const& value : transformation.values()) {
            switch (value->type()) {
            case CSSStyleValue::Type::Angle:
                values.append(AngleOrCalculated { value->as_angle().angle() });
                break;
            case CSSStyleValue::Type::Calculated: {
                auto& calculated = value->as_calculated();
                if (calculated.resolves_to_angle()) {
                    values.append(AngleOrCalculated { calculated });
                } else if (calculated.resolves_to_length_percentage()) {
                    values.append(LengthPercentage { calculated });
                } else if (calculated.resolves_to_number()) {
                    values.append(NumberPercentage { calculated });
                } else {
                    dbgln("Calculation `{}` inside {} transform-function is not a recognized type", calculated.to_string(SerializationMode::Normal), to_string(transformation.transform_function()));
                    return {};
                }
                break;
            }
            case CSSStyleValue::Type::Length:
                values.append(LengthPercentage { value->as_length().length() });
                break;
            case CSSStyleValue::Type::Percentage:
                values.append(LengthPercentage { value->as_percentage().percentage() });
                break;
            case CSSStyleValue::Type::Number:
                values.append(NumberPercentage { Number(Number::Type::Number, value->as_number().number()) });
                break;
            default:
                return {};
            }
        }

        return Transformation { transformation.transform_function(), move(values) };
    };

    static constexpr auto transformation_style_value_to_matrix = [](DOM::Element& element, TransformationStyleValue const& value) -> Optional<FloatMatrix4x4> {
        auto transformation = make_transformation(value);
        if (!transformation.has_value())
            return {};
        Optional<Painting::PaintableBox const&> paintable_box;
        if (auto layout_node = element.layout_node()) {
            if (auto paintable = layout_node->first_paintable(); paintable && is<Painting::PaintableBox>(paintable))
                paintable_box = *static_cast<Painting::PaintableBox*>(paintable);
        }
        if (auto matrix = transformation->to_matrix(paintable_box); !matrix.is_error())
            return matrix.value();
        return {};
    };

    static constexpr auto style_value_to_matrix = [](DOM::Element& element, CSSStyleValue const& value) -> FloatMatrix4x4 {
        if (value.is_transformation())
            return transformation_style_value_to_matrix(element, value.as_transformation()).value_or(FloatMatrix4x4::identity());

        // This encompasses both the allowed value "none" and any invalid values
        if (!value.is_value_list())
            return FloatMatrix4x4::identity();

        auto matrix = FloatMatrix4x4::identity();
        for (auto const& value_element : value.as_value_list().values()) {
            if (value_element->is_transformation()) {
                if (auto value_matrix = transformation_style_value_to_matrix(element, value_element->as_transformation()); value_matrix.has_value())
                    matrix = matrix * value_matrix.value();
            }
        }

        return matrix;
    };

    struct DecomposedValues {
        FloatVector3 translation;
        FloatVector3 scale;
        FloatVector3 skew;
        FloatVector4 rotation;
        FloatVector4 perspective;
    };
    // https://drafts.csswg.org/css-transforms-2/#decomposing-a-3d-matrix
    static constexpr auto decompose = [](FloatMatrix4x4 matrix) -> Optional<DecomposedValues> {
        // https://drafts.csswg.org/css-transforms-1/#supporting-functions
        static constexpr auto combine = [](auto a, auto b, float ascl, float bscl) {
            return FloatVector3 {
                ascl * a[0] + bscl * b[0],
                ascl * a[1] + bscl * b[1],
                ascl * a[2] + bscl * b[2],
            };
        };

        // Normalize the matrix.
        if (matrix(3, 3) == 0.f)
            return {};

        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                matrix(i, j) /= matrix(3, 3);

        // perspectiveMatrix is used to solve for perspective, but it also provides
        // an easy way to test for singularity of the upper 3x3 component.
        auto perspective_matrix = matrix;
        for (int i = 0; i < 3; i++)
            perspective_matrix(3, i) = 0.f;
        perspective_matrix(3, 3) = 1.f;

        if (!perspective_matrix.is_invertible())
            return {};

        DecomposedValues values;

        // First, isolate perspective.
        if (matrix(3, 0) != 0.f || matrix(3, 1) != 0.f || matrix(3, 2) != 0.f) {
            // rightHandSide is the right hand side of the equation.
            // Note: It is the bottom side in a row-major matrix
            FloatVector4 bottom_side = {
                matrix(3, 0),
                matrix(3, 1),
                matrix(3, 2),
                matrix(3, 3),
            };

            // Solve the equation by inverting perspectiveMatrix and multiplying
            // rightHandSide by the inverse.
            auto inverse_perspective_matrix = perspective_matrix.inverse();
            auto transposed_inverse_perspective_matrix = inverse_perspective_matrix.transpose();
            values.perspective = transposed_inverse_perspective_matrix * bottom_side;
        } else {
            // No perspective.
            values.perspective = { 0.0, 0.0, 0.0, 1.0 };
        }

        // Next take care of translation
        for (int i = 0; i < 3; i++)
            values.translation[i] = matrix(i, 3);

        // Now get scale and shear. 'row' is a 3 element array of 3 component vectors
        FloatVector3 row[3];
        for (int i = 0; i < 3; i++)
            row[i] = { matrix(0, i), matrix(1, i), matrix(2, i) };

        // Compute X scale factor and normalize first row.
        values.scale[0] = row[0].length();
        row[0].normalize();

        // Compute XY shear factor and make 2nd row orthogonal to 1st.
        values.skew[0] = row[0].dot(row[1]);
        row[1] = combine(row[1], row[0], 1.f, -values.skew[0]);

        // Now, compute Y scale and normalize 2nd row.
        values.scale[1] = row[1].length();
        row[1].normalize();
        values.skew[0] /= values.scale[1];

        // Compute XZ and YZ shears, orthogonalize 3rd row
        values.skew[1] = row[0].dot(row[2]);
        row[2] = combine(row[2], row[0], 1.f, -values.skew[1]);
        values.skew[2] = row[1].dot(row[2]);
        row[2] = combine(row[2], row[1], 1.f, -values.skew[2]);

        // Next, get Z scale and normalize 3rd row.
        values.scale[2] = row[2].length();
        row[2].normalize();
        values.skew[1] /= values.scale[2];
        values.skew[2] /= values.scale[2];

        // At this point, the matrix (in rows) is orthonormal.
        // Check for a coordinate system flip.  If the determinant
        // is -1, then negate the matrix and the scaling factors.
        auto pdum3 = row[1].cross(row[2]);
        if (row[0].dot(pdum3) < 0.f) {
            for (int i = 0; i < 3; i++) {
                values.scale[i] *= -1.f;
                row[i][0] *= -1.f;
                row[i][1] *= -1.f;
                row[i][2] *= -1.f;
            }
        }

        // Now, get the rotations out
        values.rotation[0] = 0.5f * sqrt(max(1.f + row[0][0] - row[1][1] - row[2][2], 0.f));
        values.rotation[1] = 0.5f * sqrt(max(1.f - row[0][0] + row[1][1] - row[2][2], 0.f));
        values.rotation[2] = 0.5f * sqrt(max(1.f - row[0][0] - row[1][1] + row[2][2], 0.f));
        values.rotation[3] = 0.5f * sqrt(max(1.f + row[0][0] + row[1][1] + row[2][2], 0.f));

        if (row[2][1] > row[1][2])
            values.rotation[0] = -values.rotation[0];
        if (row[0][2] > row[2][0])
            values.rotation[1] = -values.rotation[1];
        if (row[1][0] > row[0][1])
            values.rotation[2] = -values.rotation[2];

        // FIXME: This accounts for the fact that the browser coordinate system is left-handed instead of right-handed.
        //        The reason for this is that the positive Y-axis direction points down instead of up. To fix this, we
        //        invert the Y axis. However, it feels like the spec pseudo-code above should have taken something like
        //        this into account, so we're probably doing something else wrong.
        values.rotation[2] *= -1;

        return values;
    };

    // https://drafts.csswg.org/css-transforms-2/#recomposing-to-a-3d-matrix
    static constexpr auto recompose = [](DecomposedValues const& values) -> FloatMatrix4x4 {
        auto matrix = FloatMatrix4x4::identity();

        // apply perspective
        for (int i = 0; i < 4; i++)
            matrix(3, i) = values.perspective[i];

        // apply translation
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++)
                matrix(i, 3) += values.translation[j] * matrix(i, j);
        }

        // apply rotation
        auto x = values.rotation[0];
        auto y = values.rotation[1];
        auto z = values.rotation[2];
        auto w = values.rotation[3];

        // Construct a composite rotation matrix from the quaternion values
        // rotationMatrix is a identity 4x4 matrix initially
        auto rotation_matrix = FloatMatrix4x4::identity();
        rotation_matrix(0, 0) = 1.f - 2.f * (y * y + z * z);
        rotation_matrix(1, 0) = 2.f * (x * y - z * w);
        rotation_matrix(2, 0) = 2.f * (x * z + y * w);
        rotation_matrix(0, 1) = 2.f * (x * y + z * w);
        rotation_matrix(1, 1) = 1.f - 2.f * (x * x + z * z);
        rotation_matrix(2, 1) = 2.f * (y * z - x * w);
        rotation_matrix(0, 2) = 2.f * (x * z - y * w);
        rotation_matrix(1, 2) = 2.f * (y * z + x * w);
        rotation_matrix(2, 2) = 1.f - 2.f * (x * x + y * y);

        matrix = matrix * rotation_matrix;

        // apply skew
        // temp is a identity 4x4 matrix initially
        auto temp = FloatMatrix4x4::identity();
        if (values.skew[2] != 0.f) {
            temp(1, 2) = values.skew[2];
            matrix = matrix * temp;
        }

        if (values.skew[1] != 0.f) {
            temp(1, 2) = 0.f;
            temp(0, 2) = values.skew[1];
            matrix = matrix * temp;
        }

        if (values.skew[0] != 0.f) {
            temp(0, 2) = 0.f;
            temp(0, 1) = values.skew[0];
            matrix = matrix * temp;
        }

        // apply scale
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 4; j++)
                matrix(j, i) *= values.scale[i];
        }

        return matrix;
    };

    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
    static constexpr auto interpolate = [](DecomposedValues& from, DecomposedValues& to, float delta) -> DecomposedValues {
        auto product = clamp(from.rotation.dot(to.rotation), -1.0f, 1.0f);
        FloatVector4 interpolated_rotation;
        if (fabsf(product) == 1.0f) {
            interpolated_rotation = from.rotation;
        } else {
            auto theta = acos(product);
            auto w = sin(delta * theta) / sqrtf(1.0f - product * product);

            for (int i = 0; i < 4; i++) {
                from.rotation[i] *= cos(delta * theta) - product * w;
                to.rotation[i] *= w;
                interpolated_rotation[i] = from.rotation[i] + to.rotation[i];
            }
        }

        return {
            interpolate_raw(from.translation, to.translation, delta),
            interpolate_raw(from.scale, to.scale, delta),
            interpolate_raw(from.skew, to.skew, delta),
            interpolated_rotation,
            interpolate_raw(from.perspective, to.perspective, delta),
        };
    };

    auto from_matrix = style_value_to_matrix(element, from);
    auto to_matrix = style_value_to_matrix(element, to);
    auto from_decomposed = decompose(from_matrix);
    auto to_decomposed = decompose(to_matrix);
    if (!from_decomposed.has_value() || !to_decomposed.has_value())
        return {};
    auto interpolated_decomposed = interpolate(from_decomposed.value(), to_decomposed.value(), delta);
    auto interpolated = recompose(interpolated_decomposed);

    StyleValueVector values;
    values.ensure_capacity(16);
    for (int i = 0; i < 16; i++)
        values.append(NumberStyleValue::create(static_cast<double>(interpolated(i % 4, i / 4))));
    return StyleValueList::create({ TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix3d, move(values)) }, StyleValueList::Separator::Comma);
}

Color interpolate_color(Color from, Color to, float delta)
{
    // https://drafts.csswg.org/css-color/#interpolation-space
    // If the host syntax does not define what color space interpolation should take place in, it defaults to Oklab.
    auto from_oklab = from.to_oklab();
    auto to_oklab = to.to_oklab();

    auto color = Color::from_oklab(
        interpolate_raw(from_oklab.L, to_oklab.L, delta),
        interpolate_raw(from_oklab.a, to_oklab.a, delta),
        interpolate_raw(from_oklab.b, to_oklab.b, delta));
    color.set_alpha(interpolate_raw(from.alpha(), to.alpha(), delta));
    return color;
}

RefPtr<CSSStyleValue const> interpolate_box_shadow(DOM::Element& element, CalculationContext const& calculation_context, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    // Animation type: by computed value, treating none as a zero-item list and appending blank shadows
    //                 (transparent 0 0 0 0) with a corresponding inset keyword as needed to match the longer list if
    //                 the shorter list is otherwise compatible with the longer one

    static constexpr auto process_list = [](CSSStyleValue const& value) {
        StyleValueVector shadows;
        if (value.is_value_list()) {
            for (auto const& element : value.as_value_list().values()) {
                if (element->is_shadow())
                    shadows.append(element);
            }
        } else if (value.is_shadow()) {
            shadows.append(value);
        } else if (!value.is_keyword() || value.as_keyword().keyword() != Keyword::None) {
            VERIFY_NOT_REACHED();
        }
        return shadows;
    };

    static constexpr auto extend_list_if_necessary = [](StyleValueVector& values, StyleValueVector const& other) {
        values.ensure_capacity(other.size());
        for (size_t i = values.size(); i < other.size(); i++) {
            values.unchecked_append(ShadowStyleValue::create(
                CSSColorValue::create_from_color(Color::Transparent, ColorSyntax::Legacy),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                other[i]->as_shadow().placement()));
        }
    };

    StyleValueVector from_shadows = process_list(from);
    StyleValueVector to_shadows = process_list(to);

    extend_list_if_necessary(from_shadows, to_shadows);
    extend_list_if_necessary(to_shadows, from_shadows);

    VERIFY(from_shadows.size() == to_shadows.size());
    StyleValueVector result_shadows;
    result_shadows.ensure_capacity(from_shadows.size());

    for (size_t i = 0; i < from_shadows.size(); i++) {
        auto const& from_shadow = from_shadows[i]->as_shadow();
        auto const& to_shadow = to_shadows[i]->as_shadow();
        auto interpolated_offset_x = interpolate_value(element, calculation_context, from_shadow.offset_x(), to_shadow.offset_x(), delta, allow_discrete);
        auto interpolated_offset_y = interpolate_value(element, calculation_context, from_shadow.offset_y(), to_shadow.offset_y(), delta, allow_discrete);
        auto interpolated_blur_radius = interpolate_value(element, calculation_context, from_shadow.blur_radius(), to_shadow.blur_radius(), delta, allow_discrete);
        auto interpolated_spread_distance = interpolate_value(element, calculation_context, from_shadow.spread_distance(), to_shadow.spread_distance(), delta, allow_discrete);
        if (!interpolated_offset_x || !interpolated_offset_y || !interpolated_blur_radius || !interpolated_spread_distance)
            return {};
        auto result_shadow = ShadowStyleValue::create(
            CSSColorValue::create_from_color(interpolate_color(from_shadow.color()->to_color({}), to_shadow.color()->to_color({}), delta), ColorSyntax::Modern),
            *interpolated_offset_x,
            *interpolated_offset_y,
            *interpolated_blur_radius,
            *interpolated_spread_distance,
            delta >= 0.5f ? to_shadow.placement() : from_shadow.placement());
        result_shadows.unchecked_append(result_shadow);
    }

    return StyleValueList::create(move(result_shadows), StyleValueList::Separator::Comma);
}

static RefPtr<CSSStyleValue const> interpolate_value_impl(DOM::Element& element, CalculationContext const& calculation_context, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (from.type() != to.type()) {
        // Handle mixed percentage and dimension types
        // https://www.w3.org/TR/css-values-4/#mixed-percentages

        struct NumericBaseTypeAndDefault {
            CSSNumericType::BaseType base_type;
            ValueComparingNonnullRefPtr<CSSStyleValue const> default_value;
        };
        static constexpr auto numeric_base_type_and_default = [](CSSStyleValue const& value) -> Optional<NumericBaseTypeAndDefault> {
            switch (value.type()) {
            case CSSStyleValue::Type::Angle: {
                static auto default_angle_value = AngleStyleValue::create(Angle::make_degrees(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Angle, default_angle_value };
            }
            case CSSStyleValue::Type::Frequency: {
                static auto default_frequency_value = FrequencyStyleValue::create(Frequency::make_hertz(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Frequency, default_frequency_value };
            }
            case CSSStyleValue::Type::Length: {
                static auto default_length_value = LengthStyleValue::create(Length::make_px(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Length, default_length_value };
            }
            case CSSStyleValue::Type::Percentage: {
                static auto default_percentage_value = PercentageStyleValue::create(Percentage { 0.0 });
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Percent, default_percentage_value };
            }
            case CSSStyleValue::Type::Time: {
                static auto default_time_value = TimeStyleValue::create(Time::make_seconds(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Time, default_time_value };
            }
            default:
                return {};
            }
        };

        static auto to_calculation_node = [calculation_context](CSSStyleValue const& value) -> NonnullRefPtr<CalculationNode const> {
            switch (value.type()) {
            case CSSStyleValue::Type::Angle:
                return NumericCalculationNode::create(value.as_angle().angle(), calculation_context);
            case CSSStyleValue::Type::Frequency:
                return NumericCalculationNode::create(value.as_frequency().frequency(), calculation_context);
            case CSSStyleValue::Type::Length:
                return NumericCalculationNode::create(value.as_length().length(), calculation_context);
            case CSSStyleValue::Type::Percentage:
                return NumericCalculationNode::create(value.as_percentage().percentage(), calculation_context);
            case CSSStyleValue::Type::Time:
                return NumericCalculationNode::create(value.as_time().time(), calculation_context);
            default:
                VERIFY_NOT_REACHED();
            }
        };

        auto from_base_type_and_default = numeric_base_type_and_default(from);
        auto to_base_type_and_default = numeric_base_type_and_default(to);

        if (from_base_type_and_default.has_value() && to_base_type_and_default.has_value() && (from_base_type_and_default->base_type == CSSNumericType::BaseType::Percent || to_base_type_and_default->base_type == CSSNumericType::BaseType::Percent)) {
            // This is an interpolation from a numeric unit to a percentage, or vice versa. The trick here is to
            // interpolate two separate values. For example, consider an interpolation from 30px to 80%. It's quite
            // hard to understand how this interpolation works, but if instead we rewrite the values as "30px + 0%" and
            // "0px + 80%", then it is very simple to understand; we just interpolate each component separately.

            auto interpolated_from = interpolate_value(element, calculation_context, from, from_base_type_and_default->default_value, delta, allow_discrete);
            auto interpolated_to = interpolate_value(element, calculation_context, to_base_type_and_default->default_value, to, delta, allow_discrete);
            if (!interpolated_from || !interpolated_to)
                return {};

            Vector<NonnullRefPtr<CalculationNode const>> values;
            values.ensure_capacity(2);
            values.unchecked_append(to_calculation_node(*interpolated_from));
            values.unchecked_append(to_calculation_node(*interpolated_to));
            auto calc_node = SumCalculationNode::create(move(values));
            return CalculatedStyleValue::create(move(calc_node), CSSNumericType { to_base_type_and_default->base_type, 1 }, calculation_context);
        }

        return {};
    }

    static auto interpolate_length_percentage = [](LengthPercentage const& from, LengthPercentage const& to, float delta) -> Optional<LengthPercentage> {
        if (from.is_length() && to.is_length())
            return Length::make_px(interpolate_raw(from.length().raw_value(), to.length().raw_value(), delta));
        if (from.is_percentage() && to.is_percentage())
            return Percentage(interpolate_raw(from.percentage().value(), to.percentage().value(), delta));
        return {};
    };

    switch (from.type()) {
    case CSSStyleValue::Type::Angle:
        return AngleStyleValue::create(Angle::make_degrees(interpolate_raw(from.as_angle().angle().to_degrees(), to.as_angle().angle().to_degrees(), delta)));
    case CSSStyleValue::Type::BackgroundSize: {
        auto interpolated_x = interpolate_length_percentage(from.as_background_size().size_x(), to.as_background_size().size_x(), delta);
        auto interpolated_y = interpolate_length_percentage(from.as_background_size().size_y(), to.as_background_size().size_y(), delta);
        if (!interpolated_x.has_value() || !interpolated_y.has_value())
            return {};

        return BackgroundSizeStyleValue::create(*interpolated_x, *interpolated_y);
    }
    case CSSStyleValue::Type::Color: {
        Optional<Layout::NodeWithStyle const&> layout_node;
        if (auto node = element.layout_node())
            layout_node = *node;
        return CSSColorValue::create_from_color(interpolate_color(from.to_color(layout_node), to.to_color(layout_node), delta), ColorSyntax::Modern);
    }
    case CSSStyleValue::Type::Edge: {
        auto resolved_from = from.as_edge().resolved_value(calculation_context);
        auto resolved_to = to.as_edge().resolved_value(calculation_context);
        auto const& edge = delta >= 0.5f ? resolved_to->edge() : resolved_from->edge();
        auto const& from_offset = resolved_from->offset();
        auto const& to_offset = resolved_to->offset();
        if (auto interpolated_value = interpolate_length_percentage(from_offset, to_offset, delta); interpolated_value.has_value())
            return EdgeStyleValue::create(edge, *interpolated_value);

        return {};
    }
    case CSSStyleValue::Type::FontStyle: {
        auto const& from_font_style = from.as_font_style();
        auto const& to_font_style = to.as_font_style();
        auto interpolated_font_style = interpolate_value(element, calculation_context, CSSKeywordValue::create(to_keyword(from_font_style.font_style())), CSSKeywordValue::create(to_keyword(to_font_style.font_style())), delta, allow_discrete);
        if (!interpolated_font_style)
            return {};
        if (from_font_style.angle() && to_font_style.angle()) {
            auto interpolated_angle = interpolate_value(element, calculation_context, *from_font_style.angle(), *to_font_style.angle(), delta, allow_discrete);
            if (!interpolated_angle)
                return {};
            return FontStyleStyleValue::create(*keyword_to_font_style(interpolated_font_style->to_keyword()), interpolated_angle);
        }

        return FontStyleStyleValue::create(*keyword_to_font_style(interpolated_font_style->to_keyword()));
    }
    case CSSStyleValue::Type::Integer: {
        // https://drafts.csswg.org/css-values/#combine-integers
        // Interpolation of <integer> is defined as Vresult = round((1 - p) × VA + p × VB);
        // that is, interpolation happens in the real number space as for <number>s, and the result is converted to an <integer> by rounding to the nearest integer.
        auto interpolated_value = interpolate_raw(from.as_integer().value(), to.as_integer().value(), delta);
        return IntegerStyleValue::create(round_to<i64>(interpolated_value));
    }
    case CSSStyleValue::Type::Length: {
        // FIXME: Absolutize values
        auto const& from_length = from.as_length().length();
        auto const& to_length = to.as_length().length();
        return LengthStyleValue::create(Length(interpolate_raw(from_length.raw_value(), to_length.raw_value(), delta), from_length.type()));
    }
    case CSSStyleValue::Type::Number:
        return NumberStyleValue::create(interpolate_raw(from.as_number().number(), to.as_number().number(), delta));
    case CSSStyleValue::Type::Percentage:
        return PercentageStyleValue::create(Percentage(interpolate_raw(from.as_percentage().percentage().value(), to.as_percentage().percentage().value(), delta)));
    case CSSStyleValue::Type::Position: {
        // https://www.w3.org/TR/css-values-4/#combine-positions
        // FIXME: Interpolation of <position> is defined as the independent interpolation of each component (x, y) normalized as an offset from the top left corner as a <length-percentage>.
        auto const& from_position = from.as_position();
        auto const& to_position = to.as_position();
        auto interpolated_edge_x = interpolate_value(element, calculation_context, from_position.edge_x(), to_position.edge_x(), delta, allow_discrete);
        auto interpolated_edge_y = interpolate_value(element, calculation_context, from_position.edge_y(), to_position.edge_y(), delta, allow_discrete);
        if (!interpolated_edge_x || !interpolated_edge_y)
            return {};
        return PositionStyleValue::create(interpolated_edge_x->as_edge(), interpolated_edge_y->as_edge());
    }
    case CSSStyleValue::Type::Ratio: {
        auto from_ratio = from.as_ratio().ratio();
        auto to_ratio = to.as_ratio().ratio();

        // https://drafts.csswg.org/css-values/#combine-ratio
        // If either <ratio> is degenerate, the values cannot be interpolated.
        if (from_ratio.is_degenerate() || to_ratio.is_degenerate())
            return {};

        // The interpolation of a <ratio> is defined by converting each <ratio> to a number by dividing the first value
        // by the second (so a ratio of 3 / 2 would become 1.5), taking the logarithm of that result (so the 1.5 would
        // become approximately 0.176), then interpolating those values. The result during the interpolation is
        // converted back to a <ratio> by inverting the logarithm, then interpreting the result as a <ratio> with the
        // result as the first value and 1 as the second value.
        auto from_number = log(from_ratio.value());
        auto to_number = log(to_ratio.value());
        auto interp_number = interpolate_raw(from_number, to_number, delta);
        return RatioStyleValue::create(Ratio(pow(M_E, interp_number)));
    }
    case CSSStyleValue::Type::Rect: {
        auto from_rect = from.as_rect().rect();
        auto to_rect = to.as_rect().rect();

        if (from_rect.top_edge.is_auto() != to_rect.top_edge.is_auto() || from_rect.right_edge.is_auto() != to_rect.right_edge.is_auto() || from_rect.bottom_edge.is_auto() != to_rect.bottom_edge.is_auto() || from_rect.left_edge.is_auto() != to_rect.left_edge.is_auto())
            return {};

        // FIXME: Absolutize values
        return RectStyleValue::create({
            Length(interpolate_raw(from_rect.top_edge.raw_value(), to_rect.top_edge.raw_value(), delta), from_rect.top_edge.type()),
            Length(interpolate_raw(from_rect.right_edge.raw_value(), to_rect.right_edge.raw_value(), delta), from_rect.right_edge.type()),
            Length(interpolate_raw(from_rect.bottom_edge.raw_value(), to_rect.bottom_edge.raw_value(), delta), from_rect.bottom_edge.type()),
            Length(interpolate_raw(from_rect.left_edge.raw_value(), to_rect.left_edge.raw_value(), delta), from_rect.left_edge.type()),
        });
    }
    case CSSStyleValue::Type::Transformation:
        VERIFY_NOT_REACHED();
    case CSSStyleValue::Type::ValueList: {
        auto const& from_list = from.as_value_list();
        auto const& to_list = to.as_value_list();
        if (from_list.size() != to_list.size())
            return {};

        // FIXME: If the number of components or the types of corresponding components do not match,
        // or if any component value uses discrete animation and the two corresponding values do not match,
        // then the property values combine as discrete.
        StyleValueVector interpolated_values;
        interpolated_values.ensure_capacity(from_list.size());
        for (size_t i = 0; i < from_list.size(); ++i) {
            auto interpolated = interpolate_value(element, calculation_context, from_list.values()[i], to_list.values()[i], delta, AllowDiscrete::No);
            if (!interpolated)
                return {};

            interpolated_values.append(*interpolated);
        }

        return StyleValueList::create(move(interpolated_values), from_list.separator());
    }
    default:
        return {};
    }
}

RefPtr<CSSStyleValue const> interpolate_repeatable_list(DOM::Element& element, CalculationContext const& calculation_context, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    // https://www.w3.org/TR/web-animations/#repeatable-list
    // Same as by computed value except that if the two lists have differing numbers of items, they are first repeated to the least common multiple number of items.
    // Each item is then combined by computed value.
    // If a pair of values cannot be combined or if any component value uses discrete animation, then the property values combine as discrete.

    auto make_repeatable_list = [&](auto const& from_list, auto const& to_list, Function<void(NonnullRefPtr<CSSStyleValue const>)> append_callback) -> bool {
        // If the number of components or the types of corresponding components do not match,
        // or if any component value uses discrete animation and the two corresponding values do not match,
        // then the property values combine as discrete
        auto list_size = AK::lcm(from_list.size(), to_list.size());
        for (size_t i = 0; i < list_size; ++i) {
            auto value = interpolate_value(element, calculation_context, from_list.value_at(i, true), to_list.value_at(i, true), delta, AllowDiscrete::No);
            if (!value)
                return false;
            append_callback(*value);
        }

        return true;
    };

    auto make_single_value_list = [&](auto const& value, size_t size, auto separator) {
        StyleValueVector values;
        values.ensure_capacity(size);
        for (size_t i = 0; i < size; ++i)
            values.append(value);
        return StyleValueList::create(move(values), separator);
    };

    NonnullRefPtr from_list = from;
    NonnullRefPtr to_list = to;
    if (!from.is_value_list() && to.is_value_list())
        from_list = make_single_value_list(from, to.as_value_list().size(), to.as_value_list().separator());
    else if (!to.is_value_list() && from.is_value_list())
        to_list = make_single_value_list(to, from.as_value_list().size(), to.as_value_list().separator());
    else if (!from.is_value_list() && !to.is_value_list())
        return interpolate_value(element, calculation_context, from, to, delta, allow_discrete);

    StyleValueVector interpolated_values;
    if (!make_repeatable_list(from_list->as_value_list(), to_list->as_value_list(), [&](auto const& value) { interpolated_values.append(value); }))
        return interpolate_discrete(from, to, delta, allow_discrete);
    return StyleValueList::create(move(interpolated_values), from_list->as_value_list().separator());
}

RefPtr<CSSStyleValue const> interpolate_value(DOM::Element& element, CalculationContext const& calculation_context, CSSStyleValue const& from, CSSStyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (auto result = interpolate_value_impl(element, calculation_context, from, to, delta, allow_discrete))
        return result;
    return interpolate_discrete(from, to, delta, allow_discrete);
}

}
