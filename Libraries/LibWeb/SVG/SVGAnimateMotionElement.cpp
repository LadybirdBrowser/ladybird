/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibGfx/Path.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/Path.h>
#include <LibWeb/SVG/SVGAnimateMotionElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimateMotionElement);

SVGAnimateMotionElement::SVGAnimateMotionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGAnimationElement(document, move(qualified_name))
{
}

void SVGAnimateMotionElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (first_is_one_of(name, AttributeNames::from, AttributeNames::path, AttributeNames::to) && !namespace_.has_value())
        update_animation();
}

// https://svgwg.org/specs/animations/#AnimateMotionElementPathAttribute
Optional<CSS::PropertyID> SVGAnimateMotionElement::target_property() const
{
    // The effect of a motion path animation is to add a supplemental transformation matrix onto the CTM for the
    // referenced object which causes a translation along the x- and y-axes of the current user coordinate system by the
    // computed X and Y values computed over time.
    // AD-HOC: Animate the translate property instead, which translates the target in the coordinate system of its
    //         parent, outside of its own transform, like the supplemental transformation matrix does. Unlike that
    //         matrix, it replaces any translate property of the target.
    return CSS::PropertyID::Translate;
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
static RefPtr<CSS::StyleValue const> parse_coordinate_pair_as_translation(SVGElement const& target, Utf16View coordinate_pair)
{
    // For ‘animateMotion’, the specified values for ‘from’, ‘by’, ‘to’ and ‘values’ consists of x, y coordinate pairs,
    // with a single comma and/or white space separating the x coordinate from the y coordinate.
    auto is_separator = [](char16_t code_unit) { return code_unit == ',' || is_ascii_space(code_unit); };
    coordinate_pair = coordinate_pair.trim_ascii_whitespace();
    size_t x_length = 0;
    while (x_length < coordinate_pair.length_in_code_units() && !is_separator(coordinate_pair.code_unit_at(x_length)))
        ++x_length;
    auto x = coordinate_pair.substring_view(0, x_length);
    auto y = coordinate_pair.substring_view(x_length).trim_ascii_whitespace(TrimMode::Left);
    if (!y.is_empty() && y.code_unit_at(0) == ',')
        y = y.substring_view(1).trim_ascii_whitespace(TrimMode::Left);
    if (x.is_empty() || y.is_empty())
        return {};
    for (size_t i = 0; i < y.length_in_code_units(); ++i) {
        if (is_separator(y.code_unit_at(i)))
            return {};
    }

    // Each coordinate represents a <length>.
    // NB: Parse the coordinates as a translate value the way a presentation attribute is parsed, which takes a number
    //     as a length in user units.
    return target.parse_presentation_attribute(CSS::PropertyID::Translate, Utf16String::formatted("{} {}", x, y));
}

static NonnullRefPtr<CSS::StyleValue const> translation(Gfx::FloatPoint point)
{
    return CSS::TransformationStyleValue::create(CSS::PropertyID::Translate, CSS::TransformFunction::Translate,
        { CSS::LengthStyleValue::create(CSS::Length::make_px(point.x())), CSS::LengthStyleValue::create(CSS::Length::make_px(point.y())) });
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
Vector<Animations::BaseKeyframe> SVGAnimateMotionElement::keyframes_along_motion_path(Gfx::Path const& motion_path, CSS::PropertyID property_id)
{
    // NB: Follow polylines through the motion path, with enough points along its curves to stay as close to them as the
    //     length of the motion path is measured.
    // FIXME: Animate a position along the motion path rather than points along it, so that the target follows its curves
    //        exactly, as other engines do.
    auto vertices = motion_path.vertices();
    if (vertices.is_empty())
        return {};

    // At any time t within a motion path animation of duration dur, the computed coordinate (x,y) along the motion path
    // is determined by finding the point (x,y) which is t/dur distance along the motion path using the user agent's
    // distance along the path algorithm.
    // NB: Each vertex gets the fraction of the length of the motion path it is along as its offset, between which the
    //     target moves in straight lines. A motion path without length keeps the target at its initial point.
    auto length = vertices.last().distance;
    if (length == 0) {
        auto initial_point = translation(vertices.first().point);
        return { keyframe(0, property_id, initial_point), keyframe(1, property_id, initial_point) };
    }

    // When a path is combined with a "paced" ‘calcMode’ setting, all "move to" commands are considered to have 0 length
    // (i.e. they always happen instantaneously), and is not considered in computing the pacing.
    // NB: The vertices where a subpath ends and where the next one starts are at the same distance, so the target jumps
    //     between them.
    // FIXME: Divide the duration between the points of the path for the other calcMode values.
    Vector<Animations::BaseKeyframe> keyframes;
    keyframes.ensure_capacity(vertices.size());
    for (auto const& vertex : vertices)
        keyframes.unchecked_append(keyframe(vertex.distance / length, property_id, translation(vertex.point)));
    return keyframes;
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
Vector<Animations::BaseKeyframe> SVGAnimateMotionElement::keyframes(SVGElement const& target, CSS::PropertyID property_id) const
{
    // Regarding the definition of the motion path, the ‘mpath’ element overrides the ‘path’ attribute, which overrides
    // ‘values’, which overrides ‘from’, ‘by’ and ‘to’.
    // FIXME: Take the motion path from an mpath child element, and from the values and by attributes.
    // FIXME: Apply the keyPoints and rotate attributes.
    if (auto path = get_attribute(AttributeNames::path); path.has_value())
        return keyframes_along_motion_path(parse_path_data(*path).to_gfx_path(), property_id);

    auto to = get_attribute(AttributeNames::to);
    auto to_translation = to.has_value() ? parse_coordinate_pair_as_translation(target, *to) : nullptr;
    if (!to_translation)
        return {};

    // NB: Without a from value, this is a to animation, which animates from the underlying value. The keyframe effect
    //     takes its missing start keyframe from that value.
    auto from = get_attribute(AttributeNames::from);
    if (!from.has_value())
        return { keyframe(1, property_id, *to_translation) };
    auto from_translation = parse_coordinate_pair_as_translation(target, *from);
    if (!from_translation)
        return {};
    return { keyframe(0, property_id, *from_translation), keyframe(1, property_id, *to_translation) };
}

}
