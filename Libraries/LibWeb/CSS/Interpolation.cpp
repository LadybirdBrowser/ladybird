/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Interpolation.h"
#include <AK/IntegralMath.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/Transformation.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

template<typename T>
static T interpolate_raw(T from, T to, float delta, Optional<AcceptedTypeRange> accepted_type_range = {})
{
    if constexpr (AK::Detail::IsSame<T, double>) {
        if (accepted_type_range.has_value())
            return clamp(from + (to - from) * static_cast<double>(delta), accepted_type_range->min, accepted_type_range->max);
        return from + (to - from) * static_cast<double>(delta);
    } else if constexpr (AK::Detail::IsIntegral<T>) {
        auto from_float = static_cast<float>(from);
        auto to_float = static_cast<float>(to);
        auto min = accepted_type_range.has_value() ? accepted_type_range->min : NumericLimits<T>::min();
        auto max = accepted_type_range.has_value() ? accepted_type_range->max : NumericLimits<T>::max();
        auto unclamped_result = roundf(from_float + (to_float - from_float) * delta);
        return static_cast<AK::Detail::RemoveCVReference<T>>(clamp(unclamped_result, min, max));
    }
    VERIFY(!accepted_type_range.has_value());
    return static_cast<AK::Detail::RemoveCVReference<T>>(from + (to - from) * delta);
}

static NonnullRefPtr<StyleValue const> with_keyword_values_resolved(DOM::Element& element, PropertyID property_id, StyleValue const& value)
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
        return StyleComputer::get_non_animated_inherit_value(property_id, { element });
    default:
        break;
    }
    return value;
}

static RefPtr<StyleValue const> interpolate_discrete(StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (from.equals(to))
        return from;
    if (allow_discrete == AllowDiscrete::No)
        return {};
    return delta >= 0.5f ? to : from;
}

static RefPtr<StyleValue const> interpolate_scale(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
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
    RefPtr<StyleValue const> interpolated_z;

    if (from_transform.values().size() == 3 || to_transform.values().size() == 3) {
        static auto one_value = NumberStyleValue::create(1);
        auto from = from_transform.values().size() == 3 ? from_transform.values()[2] : one_value;
        auto to = to_transform.values().size() == 3 ? to_transform.values()[2] : one_value;
        interpolated_z = interpolate_value(element, calculation_context, from, to, delta, allow_discrete);
        if (!interpolated_z)
            return {};
    }

    StyleValueVector new_values = { *interpolated_x, *interpolated_y };
    if (interpolated_z)
        new_values.append(*interpolated_z);

    return TransformationStyleValue::create(
        PropertyID::Scale,
        new_values.size() == 3 ? TransformFunction::Scale3d : TransformFunction::Scale,
        move(new_values));
}

// https://drafts.fxtf.org/filter-effects/#interpolation-of-filter-functions
static Optional<FilterValue> interpolate_filter_function(DOM::Element& element, CalculationContext const& calculation_context, FilterValue const& from, FilterValue const& to, float delta, AllowDiscrete allow_discrete)
{
    VERIFY(!from.has<URL>());
    VERIFY(!to.has<URL>());

    if (from.index() != to.index()) {
        return {};
    }

    auto result = from.visit(
        [&](FilterOperation::Blur const& from_value) -> Optional<FilterValue> {
            auto const& to_value = to.get<FilterOperation::Blur>();

            if (auto interpolated_style_value = interpolate_value(element, calculation_context, from_value.radius.as_style_value(), to_value.radius.as_style_value(), delta, allow_discrete)) {
                LengthOrCalculated interpolated_radius = interpolated_style_value->is_length() ? LengthOrCalculated { interpolated_style_value->as_length().length() } : LengthOrCalculated { interpolated_style_value->as_calculated() };
                return FilterOperation::Blur {
                    .radius = interpolated_radius
                };
            }
            return {};
        },
        [&](FilterOperation::HueRotate const& from_value) -> Optional<FilterValue> {
            auto const& to_value = to.get<FilterOperation::HueRotate>();
            auto const& from_style_value = from_value.angle.has<FilterOperation::HueRotate::Zero>() ? AngleStyleValue::create(Angle::make_degrees(0)) : from_value.angle.get<AngleOrCalculated>().as_style_value();
            auto const& to_style_value = to_value.angle.has<FilterOperation::HueRotate::Zero>() ? AngleStyleValue::create(Angle::make_degrees(0)) : to_value.angle.get<AngleOrCalculated>().as_style_value();
            if (auto interpolated_style_value = interpolate_value(element, calculation_context, from_style_value, to_style_value, delta, allow_discrete)) {
                AngleOrCalculated interpolated_angle = interpolated_style_value->is_angle() ? AngleOrCalculated { interpolated_style_value->as_angle().angle() } : AngleOrCalculated { interpolated_style_value->as_calculated() };
                return FilterOperation::HueRotate {
                    .angle = interpolated_angle,
                };
            }
            return {};
        },
        [&](FilterOperation::Color const& from_value) -> Optional<FilterValue> {
            auto resolve_number_percentage = [](NumberPercentage const& amount) -> ValueComparingNonnullRefPtr<StyleValue const> {
                if (amount.is_number())
                    return NumberStyleValue::create(amount.number().value());
                if (amount.is_percentage())
                    return NumberStyleValue::create(amount.percentage().as_fraction());
                if (amount.is_calculated())
                    return amount.calculated();
                VERIFY_NOT_REACHED();
            };
            auto const& to_value = to.get<FilterOperation::Color>();
            auto from_style_value = resolve_number_percentage(from_value.amount);
            auto to_style_value = resolve_number_percentage(to_value.amount);
            if (auto interpolated_style_value = interpolate_value(element, calculation_context, from_style_value, to_style_value, delta, allow_discrete)) {
                auto to_number_percentage = [&](StyleValue const& style_value) -> NumberPercentage {
                    if (style_value.is_number())
                        return Number {
                            Number::Type::Number,
                            style_value.as_number().number(),
                        };
                    if (style_value.is_percentage())
                        return Percentage { style_value.as_percentage().percentage() };
                    if (style_value.is_calculated())
                        return NumberPercentage { style_value.as_calculated() };
                    VERIFY_NOT_REACHED();
                };
                return FilterOperation::Color {
                    .operation = delta >= 0.5f ? to_value.operation : from_value.operation,
                    .amount = to_number_percentage(*interpolated_style_value)
                };
            }
            return {};
        },
        [](auto const&) -> Optional<FilterValue> {
            // FIXME: Handle interpolating shadow list values
            return {};
        });

    return result;
}

// https://drafts.fxtf.org/filter-effects/#interpolation-of-filters
static RefPtr<StyleValue const> interpolate_filter_value_list(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    auto is_filter_value_list_without_url = [](StyleValue const& value) {
        if (!value.is_filter_value_list())
            return false;
        auto const& filter_value_list = value.as_filter_value_list();
        return !filter_value_list.contains_url();
    };

    auto initial_value_for = [&](FilterValue value) {
        return value.visit([&](FilterOperation::Blur const&) -> FilterValue { return FilterOperation::Blur {}; },
            [&](FilterOperation::DropShadow const&) -> FilterValue {
                return FilterOperation::DropShadow {
                    .offset_x = Length::make_px(0),
                    .offset_y = Length::make_px(0),
                    .radius = Length::make_px(0),
                    .color = Color::Transparent
                };
            },
            [&](FilterOperation::HueRotate const&) -> FilterValue {
                return FilterOperation::HueRotate {};
            },
            [&](FilterOperation::Color const& color) -> FilterValue {
                auto default_value_for_interpolation = [&]() {
                    switch (color.operation) {
                    case Gfx::ColorFilterType::Grayscale:
                    case Gfx::ColorFilterType::Invert:
                    case Gfx::ColorFilterType::Sepia:
                        return 0.0;
                    case Gfx::ColorFilterType::Brightness:
                    case Gfx::ColorFilterType::Contrast:
                    case Gfx::ColorFilterType::Opacity:
                    case Gfx::ColorFilterType::Saturate:
                        return 1.0;
                    }
                    VERIFY_NOT_REACHED();
                }();
                return FilterOperation::Color { .operation = color.operation, .amount = NumberPercentage { Number { Number::Type::Integer, default_value_for_interpolation } } };
            },
            [&](auto&) -> FilterValue {
                VERIFY_NOT_REACHED();
            });
    };

    auto interpolate_filter_values = [&](StyleValue const& from, StyleValue const& to) -> RefPtr<FilterValueListStyleValue const> {
        auto const& from_filter_values = from.as_filter_value_list().filter_value_list();
        auto const& to_filter_values = to.as_filter_value_list().filter_value_list();
        Vector<FilterValue> interpolated_filter_values;
        for (size_t i = 0; i < from.as_filter_value_list().size(); ++i) {
            auto const& from_value = from_filter_values[i];
            auto const& to_value = to_filter_values[i];

            auto interpolated_value = interpolate_filter_function(element, calculation_context, from_value, to_value, delta, allow_discrete);
            if (!interpolated_value.has_value())
                return {};
            interpolated_filter_values.append(interpolated_value.release_value());
        }
        return FilterValueListStyleValue::create(move(interpolated_filter_values));
    };

    if (is_filter_value_list_without_url(a_from) && is_filter_value_list_without_url(a_to)) {
        auto const& from_list = a_from.as_filter_value_list();
        auto const& to_list = a_to.as_filter_value_list();
        // If both filters have a <filter-value-list> of same length without <url> and for each <filter-function> for which there is a corresponding item in each list
        if (from_list.size() == to_list.size()) {
            // Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
            return interpolate_filter_values(a_from, a_to);
        }

        // If both filters have a <filter-value-list> of different length without <url> and for each <filter-function> for which there is a corresponding item in each list

        // 1. Append the missing equivalent <filter-function>s from the longer list to the end of the shorter list. The new added <filter-function>s must be initialized to their initial values for interpolation.
        auto append_missing_values_to = [&](FilterValueListStyleValue const& short_list, FilterValueListStyleValue const& longer_list) -> ValueComparingNonnullRefPtr<FilterValueListStyleValue const> {
            Vector<FilterValue> new_filter_list = short_list.filter_value_list();
            for (size_t i = new_filter_list.size(); i < longer_list.size(); ++i) {
                auto const& filter_value = longer_list.filter_value_list()[i];
                new_filter_list.append(initial_value_for(filter_value));
            }
            return FilterValueListStyleValue::create(move(new_filter_list));
        };
        ValueComparingNonnullRefPtr<StyleValue const> from = from_list.size() < to_list.size() ? append_missing_values_to(from_list, to_list) : a_from;
        ValueComparingNonnullRefPtr<StyleValue const> to = to_list.size() < from_list.size() ? append_missing_values_to(to_list, from_list) : a_to;

        // 2. Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
        return interpolate_filter_values(from, to);
    }

    // If one filter is none and the other is a <filter-value-list> without <url>
    if ((is_filter_value_list_without_url(a_from) && a_to.to_keyword() == Keyword::None)
        || (is_filter_value_list_without_url(a_to) && a_from.to_keyword() == Keyword::None)) {

        // 1. Replace none with the corresponding <filter-value-list> of the other filter. The new <filter-function>s must be initialized to their initial values for interpolation.
        auto replace_none_with_initial_filter_list_values = [&](FilterValueListStyleValue const& filter_value_list) {
            Vector<FilterValue> initial_values;
            for (auto const& filter_value : filter_value_list.filter_value_list()) {
                initial_values.append(initial_value_for(filter_value));
            }
            return FilterValueListStyleValue::create(move(initial_values));
        };

        ValueComparingNonnullRefPtr<StyleValue const> from = a_from.is_keyword() ? replace_none_with_initial_filter_list_values(a_to.as_filter_value_list()) : a_from;
        ValueComparingNonnullRefPtr<StyleValue const> to = a_to.is_keyword() ? replace_none_with_initial_filter_list_values(a_from.as_filter_value_list()) : a_to;

        // 2. Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
        return interpolate_filter_values(from, to);
    }

    // Otherwise:
    // Use discrete interpolation
    return {};
}

static RefPtr<StyleValue const> interpolate_translate(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
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

    RefPtr<StyleValue const> interpolated_z;

    if (from_transform.values().size() == 3 || to_transform.values().size() == 3) {
        auto from_z = from_transform.values().size() == 3 ? from_transform.values()[2] : zero_px;
        auto to_z = to_transform.values().size() == 3 ? to_transform.values()[2] : zero_px;
        interpolated_z = interpolate_value(element, calculation_context, from_z, to_z, delta, allow_discrete);
        if (!interpolated_z)
            return {};
    }

    StyleValueVector new_values = { *interpolated_x, *interpolated_y };
    if (interpolated_z)
        new_values.append(*interpolated_z);

    return TransformationStyleValue::create(
        PropertyID::Translate,
        new_values.size() == 3 ? TransformFunction::Translate3d : TransformFunction::Translate,
        move(new_values));
}

// https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
static FloatVector4 slerp(FloatVector4 const& from, FloatVector4 const& to, float delta)
{
    auto product = from.dot(to);

    product = clamp(product, -1.0f, 1.0f);
    if (fabsf(product) >= 1.0f)
        return from;

    auto theta = acosf(product);
    auto w = sinf(delta * theta) / sqrtf(1 - (product * product));
    auto from_multiplier = cosf(delta * theta) - (product * w);

    if (abs(w) < AK::NumericLimits<float>::epsilon())
        return from * from_multiplier;

    if (abs(from_multiplier) < AK::NumericLimits<float>::epsilon())
        return to * w;

    return from * from_multiplier + to * w;
}

static RefPtr<StyleValue const> interpolate_rotate(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    if (a_from.to_keyword() == Keyword::None && a_to.to_keyword() == Keyword::None)
        return a_from;

    static auto zero_degrees_value = AngleStyleValue::create(Angle::make_degrees(0));
    static auto zero = TransformationStyleValue::create(PropertyID::Rotate, TransformFunction::Rotate, { zero_degrees_value });

    auto const& from = a_from.to_keyword() == Keyword::None ? *zero : a_from;
    auto const& to = a_to.to_keyword() == Keyword::None ? *zero : a_to;

    auto const& from_transform = from.as_transformation();
    auto const& to_transform = to.as_transformation();

    auto from_transform_type = from_transform.transform_function();
    auto to_transform_type = to_transform.transform_function();

    if (from_transform_type == to_transform_type && from_transform.values().size() == 1) {
        auto interpolated_angle = interpolate_value(element, calculation_context, from_transform.values()[0], to_transform.values()[0], delta, allow_discrete);
        if (!interpolated_angle)
            return {};
        return TransformationStyleValue::create(PropertyID::Rotate, from_transform_type, { *interpolated_angle.release_nonnull() });
    }

    FloatVector3 from_axis { 0, 0, 1 };
    auto from_angle_value = from_transform.values()[0];
    if (from_transform.values().size() == 4) {
        from_axis.set_x(from_transform.values()[0]->as_number().number());
        from_axis.set_y(from_transform.values()[1]->as_number().number());
        from_axis.set_z(from_transform.values()[2]->as_number().number());
        from_angle_value = from_transform.values()[3];
    }
    float from_angle = from_angle_value->as_angle().angle().to_radians();

    FloatVector3 to_axis { 0, 0, 1 };
    auto to_angle_value = to_transform.values()[0];
    if (to_transform.values().size() == 4) {
        to_axis.set_x(to_transform.values()[0]->as_number().number());
        to_axis.set_y(to_transform.values()[1]->as_number().number());
        to_axis.set_z(to_transform.values()[2]->as_number().number());
        to_angle_value = to_transform.values()[3];
    }
    float to_angle = to_angle_value->as_angle().angle().to_radians();

    auto from_axis_angle = [](FloatVector3 const& axis, float angle) -> FloatVector4 {
        auto normalized = axis.normalized();
        auto half_angle = angle / 2.0f;
        auto sin_half_angle = sinf(half_angle);
        FloatVector4 result { normalized.x() * sin_half_angle, normalized.y() * sin_half_angle, normalized.z() * sin_half_angle, cosf(half_angle) };
        return result;
    };

    struct AxisAngle {
        FloatVector3 axis;
        float angle;
    };
    auto quaternion_to_axis_angle = [](FloatVector4 const& quaternion) {
        FloatVector3 axis { quaternion[0], quaternion[1], quaternion[2] };
        auto epsilon = 1e-5f;
        auto sin_half_angle = sqrtf(max(0.0f, 1.0f - quaternion[3] * quaternion[3]));
        if (sin_half_angle < epsilon)
            return AxisAngle { axis, quaternion[3] };
        auto angle = 2.0f * acosf(quaternion[3]);
        axis = axis * (1.0f / sin_half_angle);
        return AxisAngle { axis, angle };
    };

    auto from_quaternion = from_axis_angle(from_axis, from_angle);
    auto to_quaternion = from_axis_angle(to_axis, to_angle);

    auto interpolated_quaternion = slerp(from_quaternion, to_quaternion, delta);
    auto interpolated_axis_angle = quaternion_to_axis_angle(interpolated_quaternion);
    auto interpolated_x_axis = NumberStyleValue::create(interpolated_axis_angle.axis.x());
    auto interpolated_y_axis = NumberStyleValue::create(interpolated_axis_angle.axis.y());
    auto interpolated_z_axis = NumberStyleValue::create(interpolated_axis_angle.axis.z());
    auto interpolated_angle = AngleStyleValue::create(Angle::make_degrees(AK::to_degrees(interpolated_axis_angle.angle)));

    return TransformationStyleValue::create(
        PropertyID::Rotate,
        TransformFunction::Rotate3d,
        { interpolated_x_axis, interpolated_y_axis, interpolated_z_axis, interpolated_angle });
}

static Optional<GridTrackSizeList> interpolate_grid_track_size_list(CalculationContext const& calculation_context, GridTrackSizeList const& from, GridTrackSizeList const& to, float delta)
{
    auto interpolate_css_size = [&](Size const& from_size, Size const& to_size) -> Size {
        if (from_size.is_length_percentage() && to_size.is_length_percentage()) {
            auto interpolated_length = interpolate_length_percentage(calculation_context, from_size.length_percentage(), to_size.length_percentage(), delta);
            return Size::make_length_percentage(*interpolated_length);
        }

        if (from_size.type() != to_size.type())
            return delta < 0.5f ? from_size : to_size;

        switch (from_size.type()) {
        case Size::Type::FitContent: {
            if (!from_size.fit_content_available_space().has_value() || !to_size.fit_content_available_space().has_value())
                break;
            auto interpolated_available_space = interpolate_length_percentage(calculation_context, *from_size.fit_content_available_space(), *to_size.fit_content_available_space(), delta);
            if (!interpolated_available_space.has_value())
                break;
            return Size::make_fit_content(interpolated_available_space.release_value());
        }
        default:
            break;
        }

        return delta < 0.5f ? from_size : to_size;
    };

    auto interpolate_grid_size = [&](GridSize const& from_grid_size, GridSize const& to_grid_size) -> GridSize {
        if (from_grid_size.is_flexible_length() || to_grid_size.is_flexible_length()) {
            if (from_grid_size.is_flexible_length() && to_grid_size.is_flexible_length()) {
                auto interpolated_flex = interpolate_raw(from_grid_size.flex_factor(), to_grid_size.flex_factor(), delta);
                return GridSize { Flex::make_fr(interpolated_flex) };
            }
        } else {
            auto interpolated_size = interpolate_css_size(from_grid_size.css_size(), to_grid_size.css_size());
            return GridSize { move(interpolated_size) };
        }
        return delta < 0.5f ? from_grid_size : to_grid_size;
    };

    struct ExpandedTracksAndLines {
        Vector<ExplicitGridTrack> tracks;
        Vector<Optional<GridLineNames>> line_names;
    };

    auto expand_tracks_and_lines = [](GridTrackSizeList const& list) -> ExpandedTracksAndLines {
        ExpandedTracksAndLines result;
        Optional<ExplicitGridTrack> current_track;
        Optional<GridLineNames> current_line_names;
        auto append_result = [&] {
            result.tracks.append(*current_track);
            result.line_names.append(move(current_line_names));
            current_track.clear();
            current_line_names.clear();
        };

        for (auto const& component : list.list()) {
            if (auto const* grid_line_names = component.get_pointer<GridLineNames>()) {
                VERIFY(!current_line_names.has_value());
                current_line_names = *grid_line_names;
            } else if (auto const* grid_track = component.get_pointer<ExplicitGridTrack>()) {
                if (current_track.has_value())
                    append_result();

                current_track = *grid_track;
            }
            if (current_track.has_value() && current_line_names.has_value())
                append_result();
        }
        if (current_track.has_value())
            append_result();

        return result;
    };

    auto expanded_from = expand_tracks_and_lines(from);
    auto expanded_to = expand_tracks_and_lines(to);

    if (expanded_from.tracks.size() != expanded_to.tracks.size())
        return {};

    GridTrackSizeList interpolated_grid_track_size_list;
    auto add_interpolated_grid_track = [&](ExplicitGridTrack track, Optional<GridLineNames> line_names) {
        interpolated_grid_track_size_list.append(move(track));
        if (line_names.has_value())
            interpolated_grid_track_size_list.append(line_names.release_value());
    };

    for (size_t i = 0; i < expanded_from.tracks.size(); ++i) {
        auto& from_track = expanded_from.tracks[i];
        auto& to_track = expanded_to.tracks[i];
        auto interpolated_line_names = delta < 0.5f ? move(expanded_from.line_names[i]) : move(expanded_to.line_names[i]);

        if (from_track.is_repeat() || to_track.is_repeat()) {
            // https://drafts.csswg.org/css-grid/#repeat-interpolation
            if (!from_track.is_repeat() || !to_track.is_repeat())
                return {};

            auto from_repeat = from_track.repeat();
            auto to_repeat = to_track.repeat();
            if (!from_repeat.is_fixed() || !to_repeat.is_fixed())
                return {};
            if (from_repeat.repeat_count() != to_repeat.repeat_count() || from_repeat.grid_track_size_list().track_list().size() != to_repeat.grid_track_size_list().track_list().size())
                return {};

            auto interpolated_repeat_grid_tracks = interpolate_grid_track_size_list(calculation_context, from_repeat.grid_track_size_list(), to_repeat.grid_track_size_list(), delta);
            if (!interpolated_repeat_grid_tracks.has_value())
                return {};

            ExplicitGridTrack interpolated_grid_track { GridRepeat { from_repeat.type(), move(*interpolated_repeat_grid_tracks), from_repeat.repeat_count() } };
            add_interpolated_grid_track(move(interpolated_grid_track), move(interpolated_line_names));
        } else if (from_track.is_minmax() && to_track.is_minmax()) {
            auto from_minmax = from_track.minmax();
            auto to_minmax = to_track.minmax();
            auto interpolated_min = interpolate_grid_size(from_minmax.min_grid_size(), to_minmax.min_grid_size());
            auto interpolated_max = interpolate_grid_size(from_minmax.max_grid_size(), to_minmax.max_grid_size());
            ExplicitGridTrack interpolated_grid_track { GridMinMax { interpolated_min, interpolated_max } };
            add_interpolated_grid_track(move(interpolated_grid_track), move(interpolated_line_names));
        } else if (from_track.is_default() && to_track.is_default()) {
            auto const& from_grid_size = from_track.grid_size();
            auto const& to_grid_size = to_track.grid_size();
            auto interpolated_grid_size = interpolate_grid_size(from_grid_size, to_grid_size);
            ExplicitGridTrack interpolated_grid_track { move(interpolated_grid_size) };
            add_interpolated_grid_track(move(interpolated_grid_track), move(interpolated_line_names));
        } else {
            auto interpolated_grid_track = delta < 0.5f ? move(from_track) : move(to_track);
            add_interpolated_grid_track(move(interpolated_grid_track), move(interpolated_line_names));
        }
    }
    return interpolated_grid_track_size_list;
}

ValueComparingRefPtr<StyleValue const> interpolate_property(DOM::Element& element, PropertyID property_id, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    auto from = with_keyword_values_resolved(element, property_id, a_from);
    auto to = with_keyword_values_resolved(element, property_id, a_to);

    auto calculation_context = CalculationContext::for_property(PropertyNameAndID::from_id(property_id));

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
            if (auto interpolated_transform = interpolate_transform(element, calculation_context, from, to, delta, allow_discrete))
                return *interpolated_transform;

            // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
            // In some cases, an animation might cause a transformation matrix to be singular or non-invertible.
            // For example, an animation in which scale moves from 1 to -1. At the time when the matrix is in
            // such a state, the transformed element is not rendered.
            return {};
        }
        if (property_id == PropertyID::BoxShadow || property_id == PropertyID::TextShadow) {
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

        if (property_id == PropertyID::FontVariationSettings) {
            // https://drafts.csswg.org/css-fonts/#font-variation-settings-def
            // Two declarations of font-feature-settings can be animated between if they are "like". "Like" declarations
            // are ones where the same set of properties appear (in any order). Because successive duplicate properties
            // are applied instead of prior duplicate properties, two declarations can be "like" even if they have
            // differing number of properties. If two declarations are "like" then animation occurs pairwise between
            // corresponding values in the declarations. Otherwise, animation is not possible.
            if (!from->is_value_list() || !to->is_value_list())
                return interpolate_discrete(from, to, delta, allow_discrete);

            // The values in these lists have already been deduplicated and sorted at this point, so we can use
            // interpolate_value() to interpolate them pairwise.
            return interpolate_value(element, calculation_context, from, to, delta, allow_discrete);
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
                return KeywordStyleValue::create(Keyword::Visible);
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

        if (property_id == PropertyID::Rotate) {
            if (auto result = interpolate_rotate(element, calculation_context, from, to, delta, allow_discrete))
                return result;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::Filter || property_id == PropertyID::BackdropFilter) {
            if (auto result = interpolate_filter_value_list(element, calculation_context, from, to, delta, allow_discrete))
                return result;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::GridTemplateRows || property_id == PropertyID::GridTemplateColumns) {
            // https://drafts.csswg.org/css-grid/#track-sizing
            // If the list lengths match, by computed value type per item in the computed track list.
            auto from_list = from->as_grid_track_size_list().grid_track_size_list();
            auto to_list = to->as_grid_track_size_list().grid_track_size_list();

            auto interpolated_grid_tack_size_list = interpolate_grid_track_size_list(calculation_context, from_list, to_list, delta);
            if (!interpolated_grid_tack_size_list.has_value())
                return interpolate_discrete(from, to, delta, allow_discrete);

            return GridTrackSizeListStyleValue::create(interpolated_grid_tack_size_list.release_value());
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
bool property_values_are_transitionable(PropertyID property_id, StyleValue const& old_value, StyleValue const& new_value, DOM::Element& element, TransitionBehavior transition_behavior)
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

static Optional<FloatMatrix4x4> interpolate_matrices(FloatMatrix4x4 const& from, FloatMatrix4x4 const& to, float delta)
{
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
        if (matrix[3, 3] == 0.f)
            return {};

        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                matrix[i, j] /= matrix[3, 3];

        // perspectiveMatrix is used to solve for perspective, but it also provides
        // an easy way to test for singularity of the upper 3x3 component.
        auto perspective_matrix = matrix;
        for (int i = 0; i < 3; i++)
            perspective_matrix[3, i] = 0.f;
        perspective_matrix[3, 3] = 1.f;

        if (!perspective_matrix.is_invertible())
            return {};

        DecomposedValues values;

        // First, isolate perspective.
        if (matrix[3, 0] != 0.f || matrix[3, 1] != 0.f || matrix[3, 2] != 0.f) {
            // rightHandSide is the right hand side of the equation.
            // Note: It is the bottom side in a row-major matrix
            FloatVector4 bottom_side = {
                matrix[3, 0],
                matrix[3, 1],
                matrix[3, 2],
                matrix[3, 3],
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
            values.translation[i] = matrix[i, 3];

        // Now get scale and shear. 'row' is a 3 element array of 3 component vectors
        FloatVector3 row[3];
        for (int i = 0; i < 3; i++)
            row[i] = { matrix[0, i], matrix[1, i], matrix[2, i] };

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
            matrix[3, i] = values.perspective[i];

        // apply translation
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++)
                matrix[i, 3] += values.translation[j] * matrix[i, j];
        }

        // apply rotation
        auto x = values.rotation[0];
        auto y = values.rotation[1];
        auto z = values.rotation[2];
        auto w = values.rotation[3];

        // Construct a composite rotation matrix from the quaternion values
        // rotationMatrix is a identity 4x4 matrix initially
        auto rotation_matrix = FloatMatrix4x4::identity();
        rotation_matrix[0, 0] = 1.f - 2.f * (y * y + z * z);
        rotation_matrix[1, 0] = 2.f * (x * y - z * w);
        rotation_matrix[2, 0] = 2.f * (x * z + y * w);
        rotation_matrix[0, 1] = 2.f * (x * y + z * w);
        rotation_matrix[1, 1] = 1.f - 2.f * (x * x + z * z);
        rotation_matrix[2, 1] = 2.f * (y * z - x * w);
        rotation_matrix[0, 2] = 2.f * (x * z - y * w);
        rotation_matrix[1, 2] = 2.f * (y * z + x * w);
        rotation_matrix[2, 2] = 1.f - 2.f * (x * x + y * y);

        matrix = matrix * rotation_matrix;

        // apply skew
        // temp is a identity 4x4 matrix initially
        auto temp = FloatMatrix4x4::identity();
        if (values.skew[2] != 0.f) {
            temp[1, 2] = values.skew[2];
            matrix = matrix * temp;
        }

        if (values.skew[1] != 0.f) {
            temp[1, 2] = 0.f;
            temp[0, 2] = values.skew[1];
            matrix = matrix * temp;
        }

        if (values.skew[0] != 0.f) {
            temp[0, 2] = 0.f;
            temp[0, 1] = values.skew[0];
            matrix = matrix * temp;
        }

        // apply scale
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 4; j++)
                matrix[j, i] *= values.scale[i];
        }

        return matrix;
    };

    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
    static constexpr auto interpolate = [](DecomposedValues& from, DecomposedValues& to, float delta) -> DecomposedValues {
        auto interpolated_rotation = slerp(from.rotation, to.rotation, delta);
        return {
            interpolate_raw(from.translation, to.translation, delta),
            interpolate_raw(from.scale, to.scale, delta),
            interpolate_raw(from.skew, to.skew, delta),
            interpolated_rotation,
            interpolate_raw(from.perspective, to.perspective, delta),
        };
    };

    auto from_decomposed = decompose(from);
    auto to_decomposed = decompose(to);
    if (!from_decomposed.has_value() || !to_decomposed.has_value())
        return {};
    auto interpolated_decomposed = interpolate(from_decomposed.value(), to_decomposed.value(), delta);
    return recompose(interpolated_decomposed);
}

// https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
RefPtr<StyleValue const> interpolate_transform(DOM::Element& element, CalculationContext const& calculation_context,
    StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete)
{
    // * If both Va and Vb are none:
    //   * Vresult is none.
    if (from.is_keyword() && from.as_keyword().keyword() == Keyword::None
        && to.is_keyword() && to.as_keyword().keyword() == Keyword::None) {
        return KeywordStyleValue::create(Keyword::None);
    }

    // * Treating none as a list of zero length, if Va or Vb differ in length:
    auto style_value_to_transformations = [](StyleValue const& style_value)
        -> Vector<NonnullRefPtr<TransformationStyleValue const>> {
        if (style_value.is_transformation())
            return { style_value.as_transformation() };

        // NB: This encompasses both the allowed value "none" and any invalid values.
        if (!style_value.is_value_list())
            return {};

        Vector<NonnullRefPtr<TransformationStyleValue const>> result;
        result.ensure_capacity(style_value.as_value_list().size());
        for (auto const& value : style_value.as_value_list().values()) {
            VERIFY(value->is_transformation());
            result.unchecked_append(value->as_transformation());
        }
        return result;
    };
    auto from_transformations = style_value_to_transformations(from);
    auto to_transformations = style_value_to_transformations(to);
    if (from_transformations.size() != to_transformations.size()) {
        //   * extend the shorter list to the length of the longer list, setting the function at each additional
        //     position to the identity transform function matching the function at the corresponding position in the
        //     longer list. Both transform function lists are then interpolated following the next rule.
        auto& shorter_list = from_transformations.size() < to_transformations.size() ? from_transformations : to_transformations;
        auto const& longer_list = from_transformations.size() < to_transformations.size() ? to_transformations : from_transformations;
        for (size_t i = shorter_list.size(); i < longer_list.size(); ++i) {
            auto const& transformation = longer_list[i];
            shorter_list.append(TransformationStyleValue::identity_transformation(transformation->transform_function()));
        }
    }

    // https://drafts.csswg.org/css-transforms-1/#transform-primitives
    auto is_2d_primitive = [](TransformFunction function) {
        return first_is_one_of(function,
            TransformFunction::Rotate,
            TransformFunction::Scale,
            TransformFunction::Translate);
    };
    auto is_2d_transform = [&is_2d_primitive](TransformFunction function) {
        return is_2d_primitive(function)
            || first_is_one_of(function,
                TransformFunction::ScaleX,
                TransformFunction::ScaleY,
                TransformFunction::TranslateX,
                TransformFunction::TranslateY);
    };

    // https://drafts.csswg.org/css-transforms-2/#transform-primitives
    auto is_3d_primitive = [](TransformFunction function) {
        return first_is_one_of(function,
            TransformFunction::Rotate3d,
            TransformFunction::Scale3d,
            TransformFunction::Translate3d);
    };
    auto is_3d_transform = [&is_2d_transform, &is_3d_primitive](TransformFunction function) {
        return is_2d_transform(function)
            || is_3d_primitive(function)
            || first_is_one_of(function,
                TransformFunction::RotateX,
                TransformFunction::RotateY,
                TransformFunction::RotateZ,
                TransformFunction::ScaleZ,
                TransformFunction::TranslateZ);
    };

    auto convert_2d_transform_to_primitive = [](NonnullRefPtr<TransformationStyleValue const> transform)
        -> NonnullRefPtr<TransformationStyleValue const> {
        TransformFunction generic_function;
        StyleValueVector parameters;
        switch (transform->transform_function()) {
        case TransformFunction::Scale:
            generic_function = TransformFunction::Scale;
            parameters.append(transform->values()[0]);
            parameters.append(transform->values().size() > 1 ? transform->values()[1] : transform->values()[0]);
            break;
        case TransformFunction::ScaleX:
            generic_function = TransformFunction::Scale;
            parameters.append(transform->values()[0]);
            parameters.append(NumberStyleValue::create(1.));
            break;
        case TransformFunction::ScaleY:
            generic_function = TransformFunction::Scale;
            parameters.append(NumberStyleValue::create(1.));
            parameters.append(transform->values()[0]);
            break;
        case TransformFunction::Rotate:
            generic_function = TransformFunction::Rotate;
            parameters.append(transform->values()[0]);
            break;
        case TransformFunction::Translate:
            generic_function = TransformFunction::Translate;
            parameters.append(transform->values()[0]);
            parameters.append(transform->values().size() > 1
                    ? transform->values()[1]
                    : LengthStyleValue::create(Length::make_px(0.)));
            break;
        case TransformFunction::TranslateX:
            generic_function = TransformFunction::Translate;
            parameters.append(transform->values()[0]);
            parameters.append(LengthStyleValue::create(Length::make_px(0.)));
            break;
        case TransformFunction::TranslateY:
            generic_function = TransformFunction::Translate;
            parameters.append(LengthStyleValue::create(Length::make_px(0.)));
            parameters.append(transform->values()[0]);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        return TransformationStyleValue::create(PropertyID::Transform, generic_function, move(parameters));
    };

    auto convert_3d_transform_to_primitive = [&](NonnullRefPtr<TransformationStyleValue const> transform)
        -> NonnullRefPtr<TransformationStyleValue const> {
        // NB: Convert to 2D primitive if possible so we don't have to deal with scale/translate X/Y separately.
        if (is_2d_transform(transform->transform_function()))
            transform = convert_2d_transform_to_primitive(transform);

        TransformFunction generic_function;
        StyleValueVector parameters;
        switch (transform->transform_function()) {
        case TransformFunction::Rotate:
        case TransformFunction::RotateZ:
            generic_function = TransformFunction::Rotate3d;
            parameters.append(NumberStyleValue::create(0.));
            parameters.append(NumberStyleValue::create(0.));
            parameters.append(NumberStyleValue::create(1.));
            parameters.append(transform->values()[0]);
            break;
        case TransformFunction::RotateX:
            generic_function = TransformFunction::Rotate3d;
            parameters.append(NumberStyleValue::create(1.));
            parameters.append(NumberStyleValue::create(0.));
            parameters.append(NumberStyleValue::create(0.));
            parameters.append(transform->values()[0]);
            break;
        case TransformFunction::RotateY:
            generic_function = TransformFunction::Rotate3d;
            parameters.append(NumberStyleValue::create(0.));
            parameters.append(NumberStyleValue::create(1.));
            parameters.append(NumberStyleValue::create(0.));
            parameters.append(transform->values()[0]);
            break;
        case TransformFunction::Scale:
            generic_function = TransformFunction::Scale3d;
            parameters.append(transform->values()[0]);
            parameters.append(transform->values().size() > 1 ? transform->values()[1] : transform->values()[0]);
            parameters.append(NumberStyleValue::create(1.));
            break;
        case TransformFunction::ScaleZ:
            generic_function = TransformFunction::Scale3d;
            parameters.append(NumberStyleValue::create(1.));
            parameters.append(NumberStyleValue::create(1.));
            parameters.append(transform->values()[0]);
            break;
        case TransformFunction::Translate:
            generic_function = TransformFunction::Translate3d;
            parameters.append(transform->values()[0]);
            parameters.append(transform->values().size() > 1
                    ? transform->values()[1]
                    : LengthStyleValue::create(Length::make_px(0.)));
            parameters.append(LengthStyleValue::create(Length::make_px(0.)));
            break;
        case TransformFunction::TranslateZ:
            generic_function = TransformFunction::Translate3d;
            parameters.append(LengthStyleValue::create(Length::make_px(0.)));
            parameters.append(LengthStyleValue::create(Length::make_px(0.)));
            parameters.append(transform->values()[0]);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        return TransformationStyleValue::create(PropertyID::Transform, generic_function, move(parameters));
    };

    // *  Let Vresult be an empty list. Beginning at the start of Va and Vb, compare the corresponding functions at each
    //    position:
    StyleValueVector result;
    result.ensure_capacity(from_transformations.size());
    size_t index = 0;
    for (; index < from_transformations.size(); ++index) {
        auto from_transformation = from_transformations[index];
        auto to_transformation = to_transformations[index];

        auto from_function = from_transformation->transform_function();
        auto to_function = to_transformation->transform_function();

        //   * While the functions have either the same name, or are derivatives of the same primitive transform
        //     function, interpolate the corresponding pair of functions as described in § 10 Interpolation of
        //     primitives and derived transform functions and append the result to Vresult.

        // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
        // Two different types of transform functions that share the same primitive, or transform functions of the same
        // type with different number of arguments can be interpolated. Both transform functions need a former
        // conversion to the common primitive first and get interpolated numerically afterwards. The computed value will
        // be the primitive with the resulting interpolated arguments.

        // The transform functions <matrix()>, matrix3d() and perspective() get converted into 4x4 matrices first and
        // interpolated as defined in section Interpolation of Matrices afterwards.
        if (first_is_one_of(TransformFunction::Matrix, from_function, to_function)
            || first_is_one_of(TransformFunction::Matrix3d, from_function, to_function)
            || first_is_one_of(TransformFunction::Perspective, from_function, to_function)) {
            break;
        }

        // If both transform functions share a primitive in the two-dimensional space, both transform functions get
        // converted to the two-dimensional primitive. If one or both transform functions are three-dimensional
        // transform functions, the common three-dimensional primitive is used.
        if (is_2d_transform(from_function) && is_2d_transform(to_function)) {
            from_transformation = convert_2d_transform_to_primitive(from_transformation);
            to_transformation = convert_2d_transform_to_primitive(to_transformation);
        } else if (is_3d_transform(from_function) || is_3d_transform(to_function)) {
            // NB: 3D primitives do not support value expansion like their 2D counterparts do (e.g. scale(1.5) ->
            //     scale(1.5, 1.5), so we check if they are already a primitive first.
            if (!is_3d_primitive(from_function))
                from_transformation = convert_3d_transform_to_primitive(from_transformation);
            if (!is_3d_primitive(to_function))
                to_transformation = convert_3d_transform_to_primitive(to_transformation);
        }
        from_function = from_transformation->transform_function();
        to_function = to_transformation->transform_function();

        // NB: We converted both functions to their primitives. But if they're different primitives or if they have a
        //     different number of values, we can't interpolate numerically between them. Break here so the next loop
        //     can take care of the remaining functions.
        auto const& from_values = from_transformation->values();
        auto const& to_values = to_transformation->values();
        if (from_function != to_function || from_values.size() != to_values.size())
            break;

        // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
        if (from_function == TransformFunction::Rotate3d) {
            // FIXME: For interpolations with the primitive rotate3d(), the direction vectors of the transform functions get
            // normalized first. If the normalized vectors are not equal and both rotation angles are non-zero the
            // transform functions get converted into 4x4 matrices first and interpolated as defined in section
            // Interpolation of Matrices afterwards. Otherwise the rotation angle gets interpolated numerically and the
            // rotation vector of the non-zero angle is used or (0, 0, 1) if both angles are zero.

            auto interpolated_rotation = interpolate_rotate(element, calculation_context, from_transformation,
                to_transformation, delta, AllowDiscrete::No);
            if (!interpolated_rotation)
                break;
            result.unchecked_append(*interpolated_rotation);
        } else {
            StyleValueVector interpolated;
            interpolated.ensure_capacity(from_values.size());
            for (size_t i = 0; i < from_values.size(); ++i) {
                auto interpolated_value = interpolate_value(element, calculation_context, from_values[i], to_values[i],
                    delta, AllowDiscrete::No);
                if (!interpolated_value)
                    break;
                interpolated.unchecked_append(*interpolated_value);
            }
            if (interpolated.size() != from_values.size())
                break;
            result.unchecked_append(TransformationStyleValue::create(PropertyID::Transform, from_function, move(interpolated)));
        }
    }

    // NB: Return if we're done.
    if (index == from_transformations.size())
        return StyleValueList::create(move(result), StyleValueList::Separator::Space);

    //   * If the pair do not have a common name or primitive transform function, post-multiply the remaining
    //     transform functions in each of Va and Vb respectively to produce two 4x4 matrices. Interpolate these two
    //     matrices as described in § 11 Interpolation of Matrices, append the result to Vresult, and cease
    //     iterating over Va and Vb.
    Optional<Painting::PaintableBox const&> paintable_box;
    if (auto* paintable = as_if<Painting::PaintableBox>(element.paintable()))
        paintable_box = *paintable;

    auto post_multiply_remaining_transformations = [&paintable_box](size_t start_index, Vector<NonnullRefPtr<TransformationStyleValue const>> const& transformations) {
        FloatMatrix4x4 result = FloatMatrix4x4::identity();
        for (auto index = start_index; index < transformations.size(); ++index) {
            auto transformation = transformations[index]->to_transformation();
            auto transformation_matrix = transformation.to_matrix(paintable_box);
            if (transformation_matrix.is_error()) {
                dbgln("Unable to interpret a transformation's matrix; bailing out of interpolation.");
                break;
            }
            result = result * transformation_matrix.value();
        }
        return result;
    };
    auto from_matrix = post_multiply_remaining_transformations(index, from_transformations);
    auto to_matrix = post_multiply_remaining_transformations(index, to_transformations);

    auto maybe_interpolated_matrix = interpolate_matrices(from_matrix, to_matrix, delta);
    if (maybe_interpolated_matrix.has_value()) {
        auto interpolated_matrix = maybe_interpolated_matrix.release_value();
        StyleValueVector values;
        values.ensure_capacity(16);
        for (int i = 0; i < 16; i++)
            values.unchecked_append(NumberStyleValue::create(interpolated_matrix[i % 4, i / 4]));
        result.append(TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix3d, move(values)));
    } else {
        dbgln("Unable to interpolate matrices.");
    }

    return StyleValueList::create(move(result), StyleValueList::Separator::Space);
}

Color interpolate_color(Color from, Color to, float delta, ColorSyntax syntax)
{
    // https://drafts.csswg.org/css-color/#interpolation
    // FIXME: Handle all interpolation methods.
    // FIXME: Handle "analogous", "missing", and "powerless" components, somehow.
    // FIXME: Remove duplicated code with Color::mixed_with(Color other, float weight)

    // https://drafts.csswg.org/css-color/#interpolation-space
    // If the host syntax does not define what color space interpolation should take place in, it defaults to Oklab.
    // However, user agents must handle interpolation between legacy sRGB color formats (hex colors, named colors,
    // rgb(), hsl() or hwb() and the equivalent alpha-including forms) in gamma-encoded sRGB space.  This provides
    // Web compatibility; legacy sRGB content interpolates in the sRGB space by default.

    Color result;
    if (syntax == ColorSyntax::Modern) {
        // 5. changing the color components to premultiplied form
        auto from_oklab = from.to_premultiplied_oklab();
        auto to_oklab = to.to_premultiplied_oklab();

        // 6. linearly interpolating each component of the computed value of the color separately
        // 7. undoing premultiplication
        auto from_alpha = from.alpha() / 255.0f;
        auto to_alpha = to.alpha() / 255.0f;
        auto interpolated_alpha = interpolate_raw(from_alpha, to_alpha, delta);

        result = Color::from_oklab(
            interpolate_raw(from_oklab.L, to_oklab.L, delta) / interpolated_alpha,
            interpolate_raw(from_oklab.a, to_oklab.a, delta) / interpolated_alpha,
            interpolate_raw(from_oklab.b, to_oklab.b, delta) / interpolated_alpha,
            interpolated_alpha);
    } else {
        result = Color {
            interpolate_raw(from.red(), to.red(), delta),
            interpolate_raw(from.green(), to.green(), delta),
            interpolate_raw(from.blue(), to.blue(), delta),
            interpolate_raw(from.alpha(), to.alpha(), delta)
        };
    }

    return result;
}

RefPtr<StyleValue const> interpolate_box_shadow(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    // Animation type: by computed value, treating none as a zero-item list and appending blank shadows
    //                 (transparent 0 0 0 0) with a corresponding inset keyword as needed to match the longer list if
    //                 the shorter list is otherwise compatible with the longer one

    static constexpr auto process_list = [](StyleValue const& value) {
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
                other.get(0).value()->as_shadow().shadow_type(),
                ColorStyleValue::create_from_color(Color::Transparent, ColorSyntax::Legacy),
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

    ColorResolutionContext color_resolution_context {};
    if (auto node = element.layout_node()) {
        color_resolution_context = ColorResolutionContext::for_layout_node_with_style(*element.layout_node());
    }

    for (size_t i = 0; i < from_shadows.size(); i++) {
        auto const& from_shadow = from_shadows[i]->as_shadow();
        auto const& to_shadow = to_shadows[i]->as_shadow();
        auto interpolated_offset_x = interpolate_value(element, calculation_context, from_shadow.offset_x(), to_shadow.offset_x(), delta, allow_discrete);
        auto interpolated_offset_y = interpolate_value(element, calculation_context, from_shadow.offset_y(), to_shadow.offset_y(), delta, allow_discrete);
        auto interpolated_blur_radius = interpolate_value(element, calculation_context, from_shadow.blur_radius(), to_shadow.blur_radius(), delta, allow_discrete);
        auto interpolated_spread_distance = interpolate_value(element, calculation_context, from_shadow.spread_distance(), to_shadow.spread_distance(), delta, allow_discrete);
        if (!interpolated_offset_x || !interpolated_offset_y || !interpolated_blur_radius || !interpolated_spread_distance)
            return {};

        auto color_syntax = ColorSyntax::Legacy;
        if ((!from_shadow.color()->is_keyword() && from_shadow.color()->as_color().color_syntax() == ColorSyntax::Modern)
            || (!to_shadow.color()->is_keyword() && to_shadow.color()->as_color().color_syntax() == ColorSyntax::Modern)) {
            color_syntax = ColorSyntax::Modern;
        }

        // FIXME: If we aren't able to resolve the colors here, we should postpone interpolation until we can (perhaps
        //        by creating something similar to a ColorMixStyleValue).
        auto from_color = from_shadow.color()->to_color(color_resolution_context);
        auto to_color = to_shadow.color()->to_color(color_resolution_context);

        Color interpolated_color = Color::Black;

        if (from_color.has_value() && to_color.has_value())
            interpolated_color = interpolate_color(from_color.value(), to_color.value(), delta, color_syntax);

        auto result_shadow = ShadowStyleValue::create(
            from_shadow.shadow_type(),
            ColorStyleValue::create_from_color(interpolated_color, ColorSyntax::Modern),
            *interpolated_offset_x,
            *interpolated_offset_y,
            *interpolated_blur_radius,
            *interpolated_spread_distance,
            delta >= 0.5f ? to_shadow.placement() : from_shadow.placement());
        result_shadows.unchecked_append(result_shadow);
    }

    return StyleValueList::create(move(result_shadows), StyleValueList::Separator::Comma);
}

static RefPtr<StyleValue const> interpolate_mixed_value(CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta)
{
    auto get_value_type_of_numeric_style_value = [&calculation_context](StyleValue const& value) -> Optional<ValueType> {
        switch (value.type()) {
        case StyleValue::Type::Angle:
            return ValueType::Angle;
        case StyleValue::Type::Frequency:
            return ValueType::Frequency;
        case StyleValue::Type::Integer:
            return ValueType::Integer;
        case StyleValue::Type::Length:
            return ValueType::Length;
        case StyleValue::Type::Number:
            return ValueType::Number;
        case StyleValue::Type::Percentage:
            return calculation_context.percentages_resolve_as.value_or(ValueType::Percentage);
        case StyleValue::Type::Resolution:
            return ValueType::Resolution;
        case StyleValue::Type::Time:
            return ValueType::Time;
        case StyleValue::Type::Calculated: {
            auto const& calculated = value.as_calculated();
            if (calculated.resolves_to_angle_percentage())
                return ValueType::Angle;
            if (calculated.resolves_to_frequency_percentage())
                return ValueType::Frequency;
            if (calculated.resolves_to_length_percentage())
                return ValueType::Length;
            if (calculated.resolves_to_resolution())
                return ValueType::Resolution;
            if (calculated.resolves_to_number())
                return calculation_context.resolve_numbers_as_integers ? ValueType::Integer : ValueType::Number;
            if (calculated.resolves_to_percentage())
                return calculation_context.percentages_resolve_as.value_or(ValueType::Percentage);
            if (calculated.resolves_to_time_percentage())
                return ValueType::Time;

            return {};
        }
        default:
            return {};
        }
    };

    auto from_value_type = get_value_type_of_numeric_style_value(from);
    auto to_value_type = get_value_type_of_numeric_style_value(to);

    if (from_value_type.has_value() && from_value_type == to_value_type) {
        auto to_calculation_node = [&calculation_context](StyleValue const& value) -> NonnullRefPtr<CalculationNode const> {
            switch (value.type()) {
            case StyleValue::Type::Angle:
                return NumericCalculationNode::create(value.as_angle().angle(), calculation_context);
            case StyleValue::Type::Frequency:
                return NumericCalculationNode::create(value.as_frequency().frequency(), calculation_context);
            case StyleValue::Type::Integer:
                // https://drafts.csswg.org/css-values-4/#combine-integers
                // Interpolation of <integer> is defined as Vresult = round((1 - p) × VA + p × VB); that is,
                // interpolation happens in the real number space as for <number>s, and the result is converted to an
                // <integer> by rounding to the nearest integer.
                return NumericCalculationNode::create(Number { Number::Type::Number, static_cast<double>(value.as_integer().integer()) }, calculation_context);
            case StyleValue::Type::Length:
                return NumericCalculationNode::create(value.as_length().length(), calculation_context);
            case StyleValue::Type::Number:
                return NumericCalculationNode::create(Number { Number::Type::Number, value.as_number().number() }, calculation_context);
            case StyleValue::Type::Percentage:
                return NumericCalculationNode::create(value.as_percentage().percentage(), calculation_context);
            case StyleValue::Type::Time:
                return NumericCalculationNode::create(value.as_time().time(), calculation_context);
            case StyleValue::Type::Calculated:
                return value.as_calculated().calculation();
            default:
                VERIFY_NOT_REACHED();
            }
        };

        // https://drafts.csswg.org/css-values-4/#combine-mixed
        // The computed value of a percentage-dimension mix is defined as
        // FIXME: a computed dimension if the percentage component is zero or is defined specifically to compute to a dimension value
        // a computed percentage if the dimension component is zero
        // a computed calc() expression otherwise
        if (auto const* from_dimension_value = as_if<DimensionStyleValue>(from); from_dimension_value && to.type() == StyleValue::Type::Percentage) {
            auto dimension_component = from_dimension_value->raw_value() * (1.f - delta);
            auto percentage_component = to.as_percentage().raw_value() * delta;
            if (dimension_component == 0.f)
                return PercentageStyleValue::create(Percentage { percentage_component });
        } else if (auto const* to_dimension_value = as_if<DimensionStyleValue>(to); to_dimension_value && from.type() == StyleValue::Type::Percentage) {
            auto dimension_component = to_dimension_value->raw_value() * delta;
            auto percentage_component = from.as_percentage().raw_value() * (1.f - delta);
            if (dimension_component == 0)
                return PercentageStyleValue::create(Percentage { percentage_component });
        }

        auto from_node = to_calculation_node(from);
        auto to_node = to_calculation_node(to);

        // https://drafts.csswg.org/css-values-4/#combine-math
        // Interpolation of math functions, with each other or with numeric values and other numeric-valued functions, is defined as Vresult = calc((1 - p) * VA + p * VB).
        auto from_contribution = ProductCalculationNode::create({
            from_node,
            NumericCalculationNode::create(Number { Number::Type::Number, 1.f - delta }, calculation_context),
        });

        auto to_contribution = ProductCalculationNode::create({
            to_node,
            NumericCalculationNode::create(Number { Number::Type::Number, delta }, calculation_context),
        });

        return CalculatedStyleValue::create(
            simplify_a_calculation_tree(SumCalculationNode::create({ from_contribution, to_contribution }), calculation_context, {}),
            *from_node->numeric_type()->added_to(*to_node->numeric_type()),
            calculation_context);
    }

    return {};
}

template<typename T>
static NonnullRefPtr<StyleValue const> length_percentage_or_auto_to_style_value(T const& value)
{
    if constexpr (requires { value.is_auto(); }) {
        if (value.is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
    }
    if (value.is_length())
        return LengthStyleValue::create(value.length());
    if (value.is_percentage())
        return PercentageStyleValue::create(value.percentage());
    if (value.is_calculated())
        return value.calculated();
    VERIFY_NOT_REACHED();
}

Optional<LengthPercentage> interpolate_length_percentage(CalculationContext const& calculation_context, LengthPercentage const& from, LengthPercentage const& to, float delta)
{
    if (from.is_length() && to.is_length())
        return Length::make_px(interpolate_raw(from.length().raw_value(), to.length().raw_value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Length)));
    if (from.is_percentage() && to.is_percentage())
        return Percentage(interpolate_raw(from.percentage().value(), to.percentage().value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Percentage)));
    auto from_style_value = length_percentage_or_auto_to_style_value(from);
    auto to_style_value = length_percentage_or_auto_to_style_value(to);
    auto interpolated_style_value = interpolate_mixed_value(calculation_context, from_style_value, to_style_value, delta);
    if (!interpolated_style_value)
        return {};
    return LengthPercentage::from_style_value(*interpolated_style_value);
}

Optional<LengthPercentageOrAuto> interpolate_length_percentage_or_auto(CalculationContext const& calculation_context, LengthPercentageOrAuto const& from, LengthPercentageOrAuto const& to, float delta)
{
    if (from.is_auto() && to.is_auto())
        return LengthPercentageOrAuto::make_auto();
    if (from.is_length() && to.is_length())
        return Length::make_px(interpolate_raw(from.length().raw_value(), to.length().raw_value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Length)));
    if (from.is_percentage() && to.is_percentage())
        return Percentage(interpolate_raw(from.percentage().value(), to.percentage().value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Percentage)));

    auto from_style_value = length_percentage_or_auto_to_style_value(from);
    auto to_style_value = length_percentage_or_auto_to_style_value(to);
    auto interpolated_style_value = interpolate_mixed_value(calculation_context, from_style_value, to_style_value, delta);
    if (!interpolated_style_value)
        return {};
    return LengthPercentageOrAuto::from_style_value(*interpolated_style_value);
}

static RefPtr<StyleValue const> interpolate_value_impl(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (from.type() != to.type() || from.is_calculated() || to.is_calculated()) {
        // Handle mixed percentage and dimension types, as well as CalculatedStyleValues
        // https://www.w3.org/TR/css-values-4/#mixed-percentages
        return interpolate_mixed_value(calculation_context, from, to, delta);
    }

    switch (from.type()) {
    case StyleValue::Type::Angle: {
        auto interpolated_value = interpolate_raw(from.as_angle().angle().to_degrees(), to.as_angle().angle().to_degrees(), delta, calculation_context.accepted_type_ranges.get(ValueType::Angle));
        return AngleStyleValue::create(Angle::make_degrees(interpolated_value));
    }
    case StyleValue::Type::BackgroundSize: {
        auto interpolated_x = interpolate_value(element, calculation_context, from.as_background_size().size_x(), to.as_background_size().size_x(), delta, allow_discrete);
        auto interpolated_y = interpolate_value(element, calculation_context, from.as_background_size().size_y(), to.as_background_size().size_y(), delta, allow_discrete);
        if (!interpolated_x || !interpolated_y)
            return {};

        return BackgroundSizeStyleValue::create(*interpolated_x, *interpolated_y);
    }
    case StyleValue::Type::BorderImageSlice: {
        auto& from_border_image_slice = from.as_border_image_slice();
        auto& to_border_image_slice = to.as_border_image_slice();
        if (from_border_image_slice.fill() != to_border_image_slice.fill())
            return {};
        auto interpolated_top = interpolate_value(element, calculation_context, from_border_image_slice.top(), to_border_image_slice.top(), delta, allow_discrete);
        auto interpolated_right = interpolate_value(element, calculation_context, from_border_image_slice.right(), to_border_image_slice.right(), delta, allow_discrete);
        auto interpolated_bottom = interpolate_value(element, calculation_context, from_border_image_slice.bottom(), to_border_image_slice.bottom(), delta, allow_discrete);
        auto interpolated_left = interpolate_value(element, calculation_context, from_border_image_slice.left(), to_border_image_slice.left(), delta, allow_discrete);
        if (!interpolated_top || !interpolated_right || !interpolated_bottom || !interpolated_left)
            return {};
        return BorderImageSliceStyleValue::create(
            interpolated_top.release_nonnull(),
            interpolated_right.release_nonnull(),
            interpolated_bottom.release_nonnull(),
            interpolated_left.release_nonnull(),
            from_border_image_slice.fill());
    }
    case StyleValue::Type::BasicShape: {
        // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
        auto& from_shape = from.as_basic_shape().basic_shape();
        auto& to_shape = to.as_basic_shape().basic_shape();
        if (from_shape.index() != to_shape.index())
            return {};

        auto interpolate_length_box = [](CalculationContext const& calculation_context, LengthBox const& from, LengthBox const& to, float delta) -> Optional<LengthBox> {
            auto interpolated_top = interpolate_length_percentage_or_auto(calculation_context, from.top(), to.top(), delta);
            auto interpolated_right = interpolate_length_percentage_or_auto(calculation_context, from.right(), to.right(), delta);
            auto interpolated_bottom = interpolate_length_percentage_or_auto(calculation_context, from.bottom(), to.bottom(), delta);
            auto interpolated_left = interpolate_length_percentage_or_auto(calculation_context, from.left(), to.left(), delta);
            if (!interpolated_top.has_value() || !interpolated_right.has_value() || !interpolated_bottom.has_value() || !interpolated_left.has_value())
                return {};
            return LengthBox { *interpolated_top, *interpolated_right, *interpolated_bottom, *interpolated_left };
        };

        auto interpolated_shape = from_shape.visit(
            [&](Inset const& from_inset) -> Optional<BasicShape> {
                // If both shapes are of type inset(), interpolate between each value in the shape functions.
                auto& to_inset = to_shape.get<Inset>();
                auto interpolated_inset_box = interpolate_length_box(calculation_context, from_inset.inset_box, to_inset.inset_box, delta);
                if (!interpolated_inset_box.has_value())
                    return {};
                return Inset { *interpolated_inset_box };
            },
            [&](Xywh const& from_xywh) -> Optional<BasicShape> {
                auto& to_xywh = to_shape.get<Xywh>();
                auto interpolated_x = interpolate_length_percentage(calculation_context, from_xywh.x, to_xywh.x, delta);
                auto interpolated_y = interpolate_length_percentage(calculation_context, from_xywh.x, to_xywh.x, delta);
                auto interpolated_width = interpolate_length_percentage(calculation_context, from_xywh.width, to_xywh.width, delta);
                auto interpolated_height = interpolate_length_percentage(calculation_context, from_xywh.height, to_xywh.height, delta);
                if (!interpolated_x.has_value() || !interpolated_y.has_value() || !interpolated_width.has_value() || !interpolated_height.has_value())
                    return {};
                return Xywh { *interpolated_x, *interpolated_y, *interpolated_width, *interpolated_height };
            },
            [&](Rect const& from_rect) -> Optional<BasicShape> {
                auto const& to_rect = to_shape.get<Rect>();
                auto from_rect_box = from_rect.box;
                auto to_rect_box = to_rect.box;
                auto interpolated_rect_box = interpolate_length_box(calculation_context, from_rect_box, to_rect_box, delta);
                if (!interpolated_rect_box.has_value())
                    return {};
                return Rect { *interpolated_rect_box };
            },
            [&](Circle const& from_circle) -> Optional<BasicShape> {
                // If both shapes are the same type, that type is ellipse() or circle(), and the radiuses are specified
                // as <length-percentage> (rather than keywords), interpolate between each value in the shape functions.
                auto const& to_circle = to_shape.get<Circle>();
                if (!from_circle.radius.has<LengthPercentage>() || !to_circle.radius.has<LengthPercentage>())
                    return {};
                auto interpolated_radius = interpolate_length_percentage(calculation_context, from_circle.radius.get<LengthPercentage>(), to_circle.radius.get<LengthPercentage>(), delta);
                if (!interpolated_radius.has_value())
                    return {};
                auto interpolated_position = interpolate_value(element, calculation_context, from_circle.position, to_circle.position, delta, allow_discrete);
                if (!interpolated_position)
                    return {};
                return Circle { *interpolated_radius, interpolated_position->as_position() };
            },
            [&](Ellipse const& from_ellipse) -> Optional<BasicShape> {
                auto const& to_ellipse = to_shape.get<Ellipse>();
                if (!from_ellipse.radius_x.has<LengthPercentage>() || !to_ellipse.radius_x.has<LengthPercentage>())
                    return {};
                if (!from_ellipse.radius_y.has<LengthPercentage>() || !to_ellipse.radius_y.has<LengthPercentage>())
                    return {};
                auto interpolated_radius_x = interpolate_length_percentage(calculation_context, from_ellipse.radius_x.get<LengthPercentage>(), to_ellipse.radius_x.get<LengthPercentage>(), delta);
                if (!interpolated_radius_x.has_value())
                    return {};
                auto interpolated_radius_y = interpolate_length_percentage(calculation_context, from_ellipse.radius_y.get<LengthPercentage>(), to_ellipse.radius_y.get<LengthPercentage>(), delta);
                if (!interpolated_radius_y.has_value())
                    return {};
                auto interpolated_position = interpolate_value(element, calculation_context, from_ellipse.position, to_ellipse.position, delta, allow_discrete);
                if (!interpolated_position)
                    return {};
                return Ellipse { *interpolated_radius_x, *interpolated_radius_y, interpolated_position->as_position() };
            },
            [&](Polygon const& from_polygon) -> Optional<BasicShape> {
                // If both shapes are of type polygon(), both polygons have the same number of vertices, and use the
                // same <'fill-rule'>, interpolate between each value in the shape functions.
                auto const& to_polygon = to_shape.get<Polygon>();
                if (from_polygon.fill_rule != to_polygon.fill_rule)
                    return {};
                if (from_polygon.points.size() != to_polygon.points.size())
                    return {};
                Vector<Polygon::Point> interpolated_points;
                interpolated_points.ensure_capacity(from_polygon.points.size());
                for (size_t i = 0; i < from_polygon.points.size(); i++) {
                    auto const& from_point = from_polygon.points[i];
                    auto const& to_point = to_polygon.points[i];
                    auto interpolated_point_x = interpolate_length_percentage(calculation_context, from_point.x, to_point.x, delta);
                    auto interpolated_point_y = interpolate_length_percentage(calculation_context, from_point.y, to_point.y, delta);
                    if (!interpolated_point_x.has_value() || !interpolated_point_y.has_value())
                        return {};
                    interpolated_points.unchecked_append(Polygon::Point { *interpolated_point_x, *interpolated_point_y });
                }

                return Polygon { from_polygon.fill_rule, move(interpolated_points) };
            },
            [](auto&) -> Optional<BasicShape> {
                return {};
            });

        if (!interpolated_shape.has_value())
            return {};

        return BasicShapeStyleValue::create(*interpolated_shape);
    }
    case StyleValue::Type::BorderRadius: {
        auto const& from_horizontal_radius = from.as_border_radius().horizontal_radius();
        auto const& to_horizontal_radius = to.as_border_radius().horizontal_radius();
        auto const& from_vertical_radius = from.as_border_radius().vertical_radius();
        auto const& to_vertical_radius = to.as_border_radius().vertical_radius();
        auto interpolated_horizontal_radius = interpolate_value_impl(element, calculation_context, from_horizontal_radius, to_horizontal_radius, delta, allow_discrete);
        auto interpolated_vertical_radius = interpolate_value_impl(element, calculation_context, from_vertical_radius, to_vertical_radius, delta, allow_discrete);
        if (!interpolated_horizontal_radius || !interpolated_vertical_radius)
            return {};
        return BorderRadiusStyleValue::create(interpolated_horizontal_radius.release_nonnull(), interpolated_vertical_radius.release_nonnull());
    }
    case StyleValue::Type::Color: {
        ColorResolutionContext color_resolution_context {};
        if (auto node = element.layout_node()) {
            color_resolution_context = ColorResolutionContext::for_layout_node_with_style(*element.layout_node());
        }

        auto color_syntax = ColorSyntax::Legacy;
        if ((!from.is_keyword() && from.as_color().color_syntax() == ColorSyntax::Modern)
            || (!to.is_keyword() && to.as_color().color_syntax() == ColorSyntax::Modern)) {
            color_syntax = ColorSyntax::Modern;
        }

        // FIXME: If we aren't able to resolve the colors here, we should postpone interpolation until we can (perhaps
        //        by creating something similar to a ColorMixStyleValue).
        auto from_color = from.to_color(color_resolution_context);
        auto to_color = to.to_color(color_resolution_context);

        Color interpolated_color = Color::Black;

        if (from_color.has_value() && to_color.has_value())
            interpolated_color = interpolate_color(from_color.value(), to_color.value(), delta, color_syntax);

        return ColorStyleValue::create_from_color(interpolated_color, ColorSyntax::Modern);
    }
    case StyleValue::Type::Edge: {
        auto resolved_from = from.as_edge().resolved_value(calculation_context);
        auto resolved_to = to.as_edge().resolved_value(calculation_context);
        auto const& edge = delta >= 0.5f ? resolved_to->edge() : resolved_from->edge();
        auto const& from_offset = resolved_from->offset();
        auto const& to_offset = resolved_to->offset();
        if (auto interpolated_value = interpolate_length_percentage(calculation_context, from_offset, to_offset, delta); interpolated_value.has_value())
            return EdgeStyleValue::create(edge, *interpolated_value);

        return {};
    }
    case StyleValue::Type::FontStyle: {
        auto const& from_font_style = from.as_font_style();
        auto const& to_font_style = to.as_font_style();
        auto interpolated_font_style = interpolate_value(element, calculation_context, KeywordStyleValue::create(to_keyword(from_font_style.font_style())), KeywordStyleValue::create(to_keyword(to_font_style.font_style())), delta, allow_discrete);
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
    case StyleValue::Type::Integer: {
        // https://drafts.csswg.org/css-values/#combine-integers
        // Interpolation of <integer> is defined as Vresult = round((1 - p) × VA + p × VB);
        // that is, interpolation happens in the real number space as for <number>s, and the result is converted to an <integer> by rounding to the nearest integer.
        auto interpolated_value = interpolate_raw(from.as_integer().integer(), to.as_integer().integer(), delta, calculation_context.accepted_type_ranges.get(ValueType::Integer));
        return IntegerStyleValue::create(interpolated_value);
    }
    case StyleValue::Type::Length: {
        auto const& from_length = from.as_length().length();
        auto const& to_length = to.as_length().length();
        auto interpolated_value = interpolate_raw(from_length.raw_value(), to_length.raw_value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Length));
        return LengthStyleValue::create(Length(interpolated_value, from_length.unit()));
    }
    case StyleValue::Type::Number: {
        auto interpolated_value = interpolate_raw(from.as_number().number(), to.as_number().number(), delta, calculation_context.accepted_type_ranges.get(ValueType::Number));
        return NumberStyleValue::create(interpolated_value);
    }
    case StyleValue::Type::OpenTypeTagged: {
        auto& from_open_type_tagged = from.as_open_type_tagged();
        auto& to_open_type_tagged = to.as_open_type_tagged();
        if (from_open_type_tagged.tag() != to_open_type_tagged.tag())
            return {};
        auto interpolated_value = interpolate_value(element, calculation_context, from_open_type_tagged.value(), to_open_type_tagged.value(), delta, allow_discrete);
        if (!interpolated_value)
            return {};
        return OpenTypeTaggedStyleValue::create(OpenTypeTaggedStyleValue::Mode::FontVariationSettings, from_open_type_tagged.tag(), interpolated_value.release_nonnull());
    }
    case StyleValue::Type::Percentage: {
        auto interpolated_value = interpolate_raw(from.as_percentage().percentage().value(), to.as_percentage().percentage().value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Percentage));
        return PercentageStyleValue::create(Percentage(interpolated_value));
    }
    case StyleValue::Type::Position: {
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
    case StyleValue::Type::Ratio: {
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
        auto interpolated_value = interpolate_raw(from_number, to_number, delta, calculation_context.accepted_type_ranges.get(ValueType::Ratio));
        return RatioStyleValue::create(Ratio(pow(M_E, interpolated_value)));
    }
    case StyleValue::Type::Rect: {
        auto from_rect = from.as_rect().rect();
        auto to_rect = to.as_rect().rect();

        if (from_rect.top_edge.is_auto() != to_rect.top_edge.is_auto() || from_rect.right_edge.is_auto() != to_rect.right_edge.is_auto() || from_rect.bottom_edge.is_auto() != to_rect.bottom_edge.is_auto() || from_rect.left_edge.is_auto() != to_rect.left_edge.is_auto())
            return {};

        auto interpolate_length_or_auto = [](LengthOrAuto const& from, LengthOrAuto const& to, CalculationContext const& calculation_context, float delta) {
            if (from.is_auto() && to.is_auto())
                return LengthOrAuto::make_auto();
            // FIXME: Actually handle the units not matching.
            auto interpolated_value = interpolate_raw(from.length().raw_value(), to.length().raw_value(), delta, calculation_context.accepted_type_ranges.get(ValueType::Rect));
            return LengthOrAuto { Length { interpolated_value, from.length().unit() } };
        };

        return RectStyleValue::create({
            interpolate_length_or_auto(from_rect.top_edge, to_rect.top_edge, calculation_context, delta),
            interpolate_length_or_auto(from_rect.right_edge, to_rect.right_edge, calculation_context, delta),
            interpolate_length_or_auto(from_rect.bottom_edge, to_rect.bottom_edge, calculation_context, delta),
            interpolate_length_or_auto(from_rect.left_edge, to_rect.left_edge, calculation_context, delta),
        });
    }
    case StyleValue::Type::Superellipse: {
        // https://drafts.csswg.org/css-borders-4/#corner-shape-interpolation

        // https://drafts.csswg.org/css-borders-4/#normalized-superellipse-half-corner
        auto normalized_super_ellipse_half_corner = [](double s) -> double {
            //  To compute the normalized superellipse half corner given a superellipse parameter s, return the first matching statement, switching on s:

            // -∞ Return 0.
            if (s == -AK::Infinity<double>)
                return 0;

            // ∞ Return 1.
            if (s == AK::Infinity<double>)
                return 1;

            // Otherwise
            // 1. Let k be 0.5^abs(s).
            auto k = pow(0.5, abs(s));

            // 2. Let convexHalfCorner be 0.5^k.
            auto convex_half_corner = pow(0.5, k);

            // 3. If s is less than 0, return 1 - convexHalfCorner.
            if (s < 0)
                return 1 - convex_half_corner;

            // 4. Return convexHalfCorner.
            return convex_half_corner;
        };

        auto interpolation_value_to_super_ellipse_parameter = [](double interpolation_value) -> double {
            // To convert a <number [0,1]> interpolationValue back to a superellipse parameter, switch on interpolationValue:

            // 0 Return -∞.
            if (interpolation_value == 0)
                return -AK::Infinity<double>;

            // 0.5 Return 0.
            if (interpolation_value == 0.5)
                return 0;

            // 1 Return ∞.
            if (interpolation_value == 1)
                return AK::Infinity<double>;

            // Otherwise
            // 1. Let convexHalfCorner be interpolationValue.
            auto convex_half_corner = interpolation_value;

            // 2. If interpolationValue is less than 0.5, set convexHalfCorner to 1 - interpolationValue.
            if (interpolation_value < 0.5)
                convex_half_corner = 1 - interpolation_value;

            // 3. Let k be ln(0.5) / ln(convexHalfCorner).
            auto k = log(0.5) / log(convex_half_corner);

            // 4. Let s be log2(k).
            auto s = log2(k);

            // AD-HOC: The logs above can introduce slight inaccuracies, this can interfere with the behaviour of
            //         serializing superellipse style values as their equivalent keywords as that relies on exact
            //         equality. To mitigate this we simply round to a whole number if we are sufficiently near
            if (abs(round(s) - s) < AK::NumericLimits<float>::epsilon())
                s = round(s);

            // 5. If interpolationValue is less than 0.5, return -s.
            if (interpolation_value < 0.5)
                return -s;

            // 6. Return s.
            return s;
        };

        auto from_normalized_value = normalized_super_ellipse_half_corner(from.as_superellipse().parameter());
        auto to_normalized_value = normalized_super_ellipse_half_corner(to.as_superellipse().parameter());

        auto interpolated_value = interpolate_raw(from_normalized_value, to_normalized_value, delta, AcceptedTypeRange { .min = 0, .max = 1 });

        return SuperellipseStyleValue::create(NumberStyleValue::create(interpolation_value_to_super_ellipse_parameter(interpolated_value)));
    }
    case StyleValue::Type::Transformation:
        VERIFY_NOT_REACHED();
    case StyleValue::Type::ValueList: {
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

RefPtr<StyleValue const> interpolate_repeatable_list(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    // https://www.w3.org/TR/web-animations/#repeatable-list
    // Same as by computed value except that if the two lists have differing numbers of items, they are first repeated to the least common multiple number of items.
    // Each item is then combined by computed value.
    // If a pair of values cannot be combined or if any component value uses discrete animation, then the property values combine as discrete.

    auto make_repeatable_list = [&](auto const& from_list, auto const& to_list, Function<void(NonnullRefPtr<StyleValue const>)> append_callback) -> bool {
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
        to_list = make_single_value_list(to, from.as_value_list().size(), from.as_value_list().separator());
    else if (!from.is_value_list() && !to.is_value_list())
        return interpolate_value(element, calculation_context, from, to, delta, allow_discrete);

    StyleValueVector interpolated_values;
    if (!make_repeatable_list(from_list->as_value_list(), to_list->as_value_list(), [&](auto const& value) { interpolated_values.append(value); }))
        return interpolate_discrete(from, to, delta, allow_discrete);
    return StyleValueList::create(move(interpolated_values), from_list->as_value_list().separator());
}

RefPtr<StyleValue const> interpolate_value(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (auto result = interpolate_value_impl(element, calculation_context, from, to, delta, allow_discrete))
        return result;
    return interpolate_discrete(from, to, delta, allow_discrete);
}

template<typename T>
static T composite_raw_values(T underlying_raw_value, T animated_raw_value)
{
    return underlying_raw_value + animated_raw_value;
}

RefPtr<StyleValue const> composite_value(StyleValue const& underlying_value, StyleValue const& animated_value, Bindings::CompositeOperation composite_operation)
{
    auto composite_dimension_value = [](StyleValue const& underlying_value, StyleValue const& animated_value) -> Optional<double> {
        auto const& underlying_dimension = as<DimensionStyleValue>(underlying_value);
        auto const& animated_dimension = as<DimensionStyleValue>(animated_value);
        return composite_raw_values(underlying_dimension.raw_value(), animated_dimension.raw_value());
    };

    if (composite_operation == Bindings::CompositeOperation::Replace)
        return {};

    // FIXME: Composite mixed values where possible
    if (underlying_value.type() != animated_value.type())
        return {};

    switch (underlying_value.type()) {
    case StyleValue::Type::Angle: {
        auto result = composite_dimension_value(underlying_value, animated_value);
        if (!result.has_value())
            return {};
        VERIFY(underlying_value.as_angle().angle().unit() == animated_value.as_angle().angle().unit());
        return AngleStyleValue::create({ *result, underlying_value.as_angle().angle().unit() });
    }
    case StyleValue::Type::BorderImageSlice: {
        auto& underlying_border_image_slice_value = underlying_value.as_border_image_slice();
        auto& animated_border_image_slice_value = animated_value.as_border_image_slice();
        if (underlying_border_image_slice_value.fill() != animated_border_image_slice_value.fill())
            return {};
        auto composited_top = composite_value(underlying_border_image_slice_value.top(), animated_border_image_slice_value.top(), composite_operation);
        auto composited_right = composite_value(underlying_border_image_slice_value.right(), animated_border_image_slice_value.right(), composite_operation);
        auto composited_bottom = composite_value(underlying_border_image_slice_value.bottom(), animated_border_image_slice_value.bottom(), composite_operation);
        auto composited_left = composite_value(underlying_border_image_slice_value.left(), animated_border_image_slice_value.left(), composite_operation);
        if (!composited_top || !composited_right || !composited_bottom || !composited_left)
            return {};
        return BorderImageSliceStyleValue::create(composited_top.release_nonnull(), composited_right.release_nonnull(), composited_bottom.release_nonnull(), composited_left.release_nonnull(), underlying_border_image_slice_value.fill());
    }
    case StyleValue::Type::BorderRadius: {
        auto composited_horizontal_radius = composite_value(underlying_value.as_border_radius().horizontal_radius(), animated_value.as_border_radius().horizontal_radius(), composite_operation);
        auto composited_vertical_radius = composite_value(underlying_value.as_border_radius().vertical_radius(), animated_value.as_border_radius().vertical_radius(), composite_operation);
        if (!composited_horizontal_radius || !composited_vertical_radius)
            return {};
        return BorderRadiusStyleValue::create(composited_horizontal_radius.release_nonnull(), composited_vertical_radius.release_nonnull());
    }
    case StyleValue::Type::Integer: {
        auto result = composite_raw_values(underlying_value.as_integer().integer(), animated_value.as_integer().integer());
        return IntegerStyleValue::create(result);
    }
    case StyleValue::Type::Length: {
        auto result = composite_dimension_value(underlying_value, animated_value);
        if (!result.has_value())
            return {};
        VERIFY(underlying_value.as_length().length().unit() == animated_value.as_length().length().unit());
        return LengthStyleValue::create(Length { *result, underlying_value.as_length().length().unit() });
    }
    case StyleValue::Type::Number: {
        auto result = composite_raw_values(underlying_value.as_number().number(), animated_value.as_number().number());
        return NumberStyleValue::create(result);
    }
    case StyleValue::Type::OpenTypeTagged: {
        auto& underlying_open_type_tagged = underlying_value.as_open_type_tagged();
        auto& animated_open_type_tagged = animated_value.as_open_type_tagged();
        if (underlying_open_type_tagged.tag() != animated_open_type_tagged.tag())
            return {};
        auto composited_value = composite_value(underlying_open_type_tagged.value(), animated_open_type_tagged.value(), composite_operation);
        if (!composited_value)
            return {};
        return OpenTypeTaggedStyleValue::create(OpenTypeTaggedStyleValue::Mode::FontVariationSettings, underlying_open_type_tagged.tag(), composited_value.release_nonnull());
    }
    case StyleValue::Type::Percentage: {
        auto result = composite_raw_values(underlying_value.as_percentage().percentage().value(), animated_value.as_percentage().percentage().value());
        return PercentageStyleValue::create(Percentage { result });
    }
    case StyleValue::Type::Position: {
        auto& underlying_position = underlying_value.as_position();
        auto& animated_position = animated_value.as_position();
        auto composited_edge_x = composite_value(underlying_position.edge_x(), animated_position.edge_x(), composite_operation);
        auto composited_edge_y = composite_value(underlying_position.edge_y(), animated_position.edge_y(), composite_operation);
        if (!composited_edge_x || !composited_edge_y)
            return {};

        return PositionStyleValue::create(composited_edge_x->as_edge(), composited_edge_y->as_edge());
    }
    case StyleValue::Type::Ratio: {
        // https://drafts.csswg.org/css-values/#combine-ratio
        // Addition of <ratio>s is not possible.
        return {};
    }
    case StyleValue::Type::ValueList: {
        auto& underlying_list = underlying_value.as_value_list();
        auto& animated_list = animated_value.as_value_list();
        if (underlying_list.size() != animated_list.size() || underlying_list.separator() != animated_list.separator())
            return {};
        StyleValueVector values;
        values.ensure_capacity(underlying_list.size());
        for (size_t i = 0; i < underlying_list.size(); ++i) {
            auto composited_value = composite_value(underlying_list.values()[i], animated_list.values()[i], composite_operation);
            if (!composited_value)
                return {};
            values.unchecked_append(*composited_value);
        }
        return StyleValueList::create(move(values), underlying_list.separator());
    }
    default:
        // FIXME: Implement compositing for missing types
        return {};
    }
}

}
