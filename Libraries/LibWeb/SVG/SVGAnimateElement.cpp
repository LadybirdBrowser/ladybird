/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimateElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimateElement);

SVGAnimateElement::SVGAnimateElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGAnimationElement(document, move(qualified_name))
{
}

void SVGAnimateElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (first_is_one_of(name, AttributeNames::calcMode, AttributeNames::from, AttributeNames::keySplines, AttributeNames::keyTimes, AttributeNames::to, AttributeNames::values) && !namespace_.has_value())
        update_animation();
}

enum class CalcMode : u8 {
    Discrete,
    Linear,
    Paced,
    Spline,
};

// https://svgwg.org/specs/animations/#CalcModeAttribute
static CalcMode calc_mode(SVGAnimateElement const& element)
{
    // The default mode is 'linear', however if the attribute does not support linear interpolation (e.g. for strings),
    // the ‘calcMode’ attribute is ignored and discrete interpolation is used.
    // NB: SMIL Animation ignores a value with any error in its syntax, which leaves the default mode.
    auto calc_mode = element.get_attribute(AttributeNames::calcMode);
    if (!calc_mode.has_value())
        return CalcMode::Linear;
    if (*calc_mode == "discrete"sv)
        return CalcMode::Discrete;
    if (*calc_mode == "paced"sv)
        return CalcMode::Paced;
    if (*calc_mode == "spline"sv)
        return CalcMode::Spline;
    return CalcMode::Linear;
}

// NB: The values, keyTimes and keySplines attributes are lists separated by semicolons, which may end in one.
static Vector<Utf16View> split_semicolon_separated_list(Utf16View list)
{
    // Let values be a list of strings formed by splitting attribute at each U+003B SEMICOLON character.
    auto items = list.split_view(';', SplitBehavior::KeepEmpty);

    // Strip any leading and trailing white space characters from each value of values.
    for (auto& item : items)
        item = item.trim_ascii_whitespace();

    // Remove the final value in values if it is the empty string.
    if (!items.is_empty() && items.last().is_empty())
        items.take_last();
    return items;
}

// https://svgwg.org/specs/animations/#KeyTimesAttribute
static Optional<Vector<double>> parse_key_times(Utf16View attribute, size_t value_count, CalcMode calc_mode)
{
    // For animations specified with a ‘values’ list, the ‘keyTimes’ attribute if specified must have exactly as many
    // values as there are in the ‘values’ attribute. For from/to/by animations, the ‘keyTimes’ attribute if specified
    // must have two values.
    auto items = split_semicolon_separated_list(attribute);
    if (items.size() != value_count)
        return {};

    Vector<double> key_times;
    key_times.ensure_capacity(items.size());
    for (auto item : items) {
        // Each time value in the ‘keyTimes’ list is specified as a floating point value between 0 and 1 (inclusive),
        // representing a proportional offset into the simple duration of the animation element.
        auto key_time = parse_number(item);
        if (!key_time.has_value() || *key_time < 0 || *key_time > 1)
            return {};

        // Each successive time value must be greater than or equal to the preceding time value.
        if (!key_times.is_empty() && *key_time < key_times.last())
            return {};
        key_times.unchecked_append(*key_time);
    }

    // For linear and spline animation, the first time value in the list must be 0, and the last time value in the
    // list must be 1.
    // For discrete animation, the first time value in the list must be 0.
    if (key_times.first() != 0)
        return {};
    if (calc_mode != CalcMode::Discrete && key_times.last() != 1)
        return {};
    return key_times;
}

// https://svgwg.org/specs/animations/#KeySplinesAttribute
static Optional<Vector<CSS::EasingFunction>> parse_key_splines(Utf16View attribute, size_t interval_count)
{
    // Thus, there must be one fewer sets of control points than there are ‘keyTimes’.
    auto items = split_semicolon_separated_list(attribute);
    if (items.size() != interval_count)
        return {};

    Vector<CSS::EasingFunction> key_splines;
    key_splines.ensure_capacity(items.size());
    for (auto item : items) {
        // Each control point description is a set of four values: x1 y1 x2 y2, describing the Bézier control points
        // for one time segment.
        // The values must all be in the range 0 to 1.
        auto control_points = parse_table_values(item);
        if (control_points.size() != 4 || any_of(control_points, [](float value) { return value < 0 || value > 1; }))
            return {};

        // NB: The keyTimes values are the anchor points of a segment, so its control points describe a cubic-bezier()
        //     easing function across it.
        auto easing = Animations::AnimationEffect::parse_easing_string(Utf16String::formatted("cubic-bezier({}, {}, {}, {})", control_points[0], control_points[1], control_points[2], control_points[3]));
        if (!easing.has_value())
            return {};
        key_splines.unchecked_append(easing.release_value());
    }
    return key_splines;
}

// https://svgwg.org/specs/animations/#AnimateElement
Vector<Animations::BaseKeyframe> SVGAnimateElement::keyframes(SVGElement const& target, CSS::PropertyID property_id) const
{
    // NB: A missing value stands for the underlying value of the attribute or property, which the keyframe effect
    //     takes as its missing start keyframe.
    Vector<RefPtr<CSS::StyleValue const>> values;

    // If a list of ‘values’ is specified, any ‘from’, ‘to’ and ‘by’ attribute values are ignored.
    if (auto values_attribute = get_attribute(AttributeNames::values); values_attribute.has_value()) {
        // Parse each value in values using the rules for parsing the attribute identified by the ‘href’, and
        // ‘attributeName’ attributes.
        // NB: If any values are not legal, the document fragment is in error, so the animation does not apply.
        for (auto item : split_semicolon_separated_list(*values_attribute)) {
            auto value = target.parse_presentation_attribute(property_id, item);
            if (!value)
                return {};
            values.append(move(value));
        }
    } else {
        // FIXME: Support by animations.
        auto to = get_attribute(AttributeNames::to);
        auto to_value = to.has_value() ? target.parse_presentation_attribute(property_id, to->utf16_view()) : nullptr;
        if (!to_value)
            return {};

        // NB: Without a from value, this is a to animation, which animates from the underlying value.
        RefPtr<CSS::StyleValue const> from_value;
        if (auto from = get_attribute(AttributeNames::from); from.has_value()) {
            from_value = target.parse_presentation_attribute(property_id, from->utf16_view());
            if (!from_value)
                return {};
        }
        values = { move(from_value), move(to_value) };
    }
    if (values.is_empty())
        return {};

    // NB: A single value holds for the entire duration, as with a set element.
    if (values.size() == 1)
        return { keyframe(0, property_id, *values.first()), keyframe(1, property_id, *values.first()) };

    auto calc_mode = ::Web::SVG::calc_mode(*this);

    // If the interpolation mode is 'paced', the ‘keyTimes’ attribute is ignored.
    // If the simple duration is indefinite, any ‘keyTimes’ specification will be ignored.
    // FIXME: Pace paced animations evenly rather than interpolating them linearly.
    Optional<Vector<double>> key_times;
    if (auto key_times_attribute = get_attribute(AttributeNames::keyTimes);
        key_times_attribute.has_value() && calc_mode != CalcMode::Paced && !isinf(simple_duration())) {
        // If there are any errors in the ‘keyTimes’ specification (bad values, too many or too few values), the
        // document fragment is in error (see error processing).
        key_times = parse_key_times(*key_times_attribute, values.size(), calc_mode);
        if (!key_times.has_value())
            return {};
    }

    // spline
    //     Interpolates from one value in the ‘values’ list to the next according to a time function defined by a cubic
    //     Bézier spline. The points of the spline are defined in the ‘keyTimes’ attribute, and the control points for
    //     each interval are defined in the ‘keySplines’ attribute.
    Optional<Vector<CSS::EasingFunction>> key_splines;
    if (calc_mode == CalcMode::Spline) {
        // If there are any errors in the ‘keySplines’ specification (bad values, too many or too few values), the
        // document fragment is in error (see error processing).
        // NB: A missing keySplines attribute is an error too, since there must be one set of control points for each
        //     interval.
        auto key_splines_attribute = get_attribute(AttributeNames::keySplines);
        if (key_splines_attribute.has_value())
            key_splines = parse_key_splines(*key_splines_attribute, values.size() - 1);
        if (!key_splines.has_value())
            return {};
    }

    // discrete
    //     This specifies that the animation function will jump from one value to the next without any interpolation.
    // NB: Without keyTimes, the values of a discrete animation divide the simple duration evenly, each holding for its
    //     part of it. Each keyframe steps to the next one only at its end, and a closing keyframe holds the last value
    //     until the end of the simple duration.
    // FIXME: Hold the underlying value of a discrete to animation for the first half of the simple duration.
    auto offset_of_value = [&](size_t index) -> double {
        if (key_times.has_value())
            return (*key_times)[index];
        if (calc_mode == CalcMode::Discrete)
            return static_cast<double>(index) / static_cast<double>(values.size());
        return static_cast<double>(index) / static_cast<double>(values.size() - 1);
    };

    Vector<Animations::BaseKeyframe> keyframes;
    keyframes.ensure_capacity(values.size() + 1);
    for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i])
            continue;
        auto value_keyframe = keyframe(offset_of_value(i), property_id, *values[i]);
        if (calc_mode == CalcMode::Discrete)
            value_keyframe.easing = Animations::AnimationEffect::parse_easing_string("step-end"_utf16).release_value();
        else if (key_splines.has_value() && i < key_splines->size())
            value_keyframe.easing = (*key_splines)[i];
        else
            value_keyframe.easing = CSS::EasingFunction::linear();
        keyframes.unchecked_append(move(value_keyframe));
    }
    if (calc_mode == CalcMode::Discrete && keyframes.last().offset != 1)
        keyframes.unchecked_append(keyframe(1, property_id, *values.last()));
    return keyframes;
}

}
