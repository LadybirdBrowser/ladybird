/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/Path.h>
#include <LibWeb/SVG/SVGAnimateMotionElement.h>
#include <LibWeb/SVG/SVGGeometryElement.h>
#include <LibWeb/SVG/SVGMPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimateMotionElement);

SVGAnimateMotionElement::SVGAnimateMotionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGAnimationElement(document, move(qualified_name))
{
}

void SVGAnimateMotionElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (first_is_one_of(name, AttributeNames::from, AttributeNames::path, AttributeNames::rotate, AttributeNames::to) && !namespace_.has_value())
        update_animation();
}

void SVGAnimateMotionElement::schedule_referenced_motion_path_update()
{
    document().schedule_svg_referenced_motion_path_update(*this);
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
void SVGAnimateMotionElement::update_referenced_motion_path()
{
    // If the ‘mpath’ sub-element refers to a shape element, the motion path is defined by the equivalent path of the
    // shape element.
    // NB: Only the first mpath child counts, as the content model allows one. Without a shape element that it refers to,
    //     the attributes of this element define the motion path.
    // FIXME: Follow changes to the geometry of the referenced element.
    m_referenced_motion_path_vertices.clear();
    if (auto* mpath_element = is_connected() ? first_child_of_type<SVGMPathElement>() : nullptr) {
        if (auto* geometry_element = as_if<SVGGeometryElement>(mpath_element->referenced_element().ptr()))
            m_referenced_motion_path_vertices = geometry_element->equivalent_path_vertices();
    }
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

static Optional<Gfx::FloatPoint> translation_in_pixels(CSS::StyleValue const& translation)
{
    if (!translation.is_transformation())
        return {};
    auto values = translation.as_transformation().values();
    if (values.size() != 2 || !values[0]->is_length() || !values[1]->is_length())
        return {};
    auto x = values[0]->as_length().length();
    auto y = values[1]->as_length().length();
    if (!x.is_absolute() || !y.is_absolute())
        return {};
    return Gfx::FloatPoint { x.absolute_length_to_px_without_rounding(), y.absolute_length_to_px_without_rounding() };
}

namespace {

struct MotionRotation {
    bool follows_direction { false };
    double degrees { 0 };

    bool is_none() const { return !follows_direction && degrees == 0; }
};

// Rotations along the motion path, each within half a turn of the previous one so that the target turns the short way
// between them.
class MotionRotations {
public:
    explicit MotionRotations(MotionRotation rotation)
        : m_rotation(rotation)
    {
    }

    NonnullRefPtr<CSS::StyleValue const> rotation_for(Gfx::FloatVector2 direction)
    {
        auto degrees = m_rotation.degrees;
        if (m_rotation.follows_direction)
            degrees += AK::to_degrees(AK::atan2(static_cast<double>(direction.y()), static_cast<double>(direction.x())));
        if (m_previous_degrees.has_value())
            degrees -= 360 * AK::round((degrees - *m_previous_degrees) / 360);
        m_previous_degrees = degrees;
        return CSS::TransformationStyleValue::create(CSS::PropertyID::Rotate, CSS::TransformFunction::Rotate,
            { CSS::AngleStyleValue::create(CSS::Angle::make_degrees(degrees)) });
    }

private:
    MotionRotation m_rotation;
    Optional<double> m_previous_degrees;
};

}

// https://svgwg.org/specs/animations/#RotateAttribute
static MotionRotation motion_rotation(DOM::Element const& element)
{
    // The ‘rotate’ attribute post-multiplies a supplemental transformation matrix onto the CTM of the target element to
    // apply a rotation transformation about the origin of the current user coordinate system. The rotation
    // transformation is applied after the supplemental translation transformation that is computed due to the ‘path’
    // attribute.
    // AD-HOC: Animate the rotate property for this, which applies after the translate property. Unlike the supplemental
    //         transformation matrix, it rotates about the transform origin of the target, which is the origin of its
    //         user coordinate system unless the transform-origin property says otherwise, and it replaces any rotate
    //         property of the target.
    auto rotate = element.get_attribute(AttributeNames::rotate);
    if (!rotate.has_value())
        return {};

    // auto
    //     Indicates that the object is rotated over time by the angle of the direction (i.e., directional tangent
    //     vector) of the motion path.
    if (*rotate == "auto"sv)
        return { .follows_direction = true, .degrees = 0 };

    // auto-reverse
    //     Indicates that the object is rotated over time by the angle of the direction (i.e., directional tangent
    //     vector) of the motion path plus 180 degrees.
    if (*rotate == "auto-reverse"sv)
        return { .follows_direction = true, .degrees = 180 };

    // <number>
    //     Indicates that the target element has a constant rotation transformation applied to it, where the rotation
    //     angle is the specified number of degrees.
    if (auto number = parse_number_percentage(*rotate); number.has_value() && !number->is_percentage() && isfinite(number->value()))
        return { .follows_direction = false, .degrees = number->value() };

    // NB: SMIL Animation ignores a value with any error in its syntax, which leaves the initial value of 0.
    return {};
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
Vector<Animations::BaseKeyframe> SVGAnimateMotionElement::keyframes_along_motion_path(ReadonlySpan<PathVertex> vertices, CSS::PropertyID property_id) const
{
    // NB: Follow polylines through the motion path, with enough points along its curves to stay as close to them as the
    //     length of the motion path is measured.
    // FIXME: Animate a position along the motion path rather than points along it, so that the target follows its curves
    //        exactly, as other engines do.
    if (vertices.is_empty())
        return {};

    // At any time t within a motion path animation of duration dur, the computed coordinate (x,y) along the motion path
    // is determined by finding the point (x,y) which is t/dur distance along the motion path using the user agent's
    // distance along the path algorithm.
    // NB: Each vertex gets the fraction of the length of the motion path it is along as its offset, between which the
    //     target moves in straight lines. A motion path without length keeps the target at its initial point.
    auto rotation = motion_rotation(*this);
    MotionRotations rotations { rotation };
    auto length = vertices.last().distance;
    if (length == 0) {
        Vector<Animations::BaseKeyframe> keyframes { keyframe(0, property_id, translation(vertices.first().point)), keyframe(1, property_id, translation(vertices.first().point)) };
        if (!rotation.is_none()) {
            for (auto& keyframe : keyframes)
                set_keyframe_property(keyframe, CSS::PropertyID::Rotate, rotations.rotation_for(vertices.first().direction));
        }
        return keyframes;
    }

    // When a path is combined with a "paced" ‘calcMode’ setting, all "move to" commands are considered to have 0 length
    // (i.e. they always happen instantaneously), and is not considered in computing the pacing.
    // NB: The vertices where a subpath ends and where the next one starts are at the same distance, so the target jumps
    //     between them.
    // FIXME: Divide the duration between the points of the path for the other calcMode values.
    // NB: Where the motion path changes direction at once, its vertex appears twice, so the target turns at once.
    Vector<Animations::BaseKeyframe> keyframes;
    keyframes.ensure_capacity(vertices.size());
    for (auto const& vertex : vertices) {
        auto vertex_keyframe = keyframe(vertex.distance / length, property_id, translation(vertex.point));
        if (!rotation.is_none())
            set_keyframe_property(vertex_keyframe, CSS::PropertyID::Rotate, rotations.rotation_for(vertex.direction));
        keyframes.unchecked_append(move(vertex_keyframe));
    }
    return keyframes;
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
Vector<Animations::BaseKeyframe> SVGAnimateMotionElement::keyframes(SVGElement const& target, CSS::PropertyID property_id) const
{
    // Regarding the definition of the motion path, the ‘mpath’ element overrides the ‘path’ attribute, which overrides
    // ‘values’, which overrides ‘from’, ‘by’ and ‘to’.
    // FIXME: Take the motion path from the values and by attributes.
    // FIXME: Apply the keyPoints attribute.
    if (m_referenced_motion_path_vertices.has_value())
        return keyframes_along_motion_path(*m_referenced_motion_path_vertices, property_id);
    if (auto path = get_attribute(AttributeNames::path); path.has_value())
        return keyframes_along_motion_path(parse_path_data(*path).vertices(), property_id);

    auto to = get_attribute(AttributeNames::to);
    auto to_translation = to.has_value() ? parse_coordinate_pair_as_translation(target, *to) : nullptr;
    if (!to_translation)
        return {};

    auto from = get_attribute(AttributeNames::from);
    RefPtr<CSS::StyleValue const> from_translation;
    if (from.has_value()) {
        from_translation = parse_coordinate_pair_as_translation(target, *from);
        if (!from_translation)
            return {};
    }

    // NB: Without a from value, this is a to animation, which animates from the underlying value. The keyframe effect
    //     takes its missing start keyframe from that value.
    Vector<Animations::BaseKeyframe> keyframes;
    if (from_translation)
        keyframes.append(keyframe(0, property_id, *from_translation));
    keyframes.append(keyframe(1, property_id, *to_translation));

    auto rotation = motion_rotation(*this);
    if (rotation.is_none())
        return keyframes;

    // NB: The underlying value of a motion is no translation at all, so a to animation moves from the origin. Only
    //     coordinates in absolute lengths give the motion a known direction. Rotations do not come from the underlying
    //     value, so a to animation gets a start keyframe for its rotation alone.
    auto from_point = from_translation ? translation_in_pixels(*from_translation) : Gfx::FloatPoint {};
    auto to_point = translation_in_pixels(*to_translation);
    Gfx::FloatVector2 direction;
    if (from_point.has_value() && to_point.has_value())
        direction = { to_point->x() - from_point->x(), to_point->y() - from_point->y() };
    if (!from_translation) {
        Animations::BaseKeyframe rotation_keyframe;
        rotation_keyframe.offset = 0;
        rotation_keyframe.properties = Animations::BaseKeyframe::ParsedProperties {};
        keyframes.prepend(move(rotation_keyframe));
    }
    MotionRotations rotations { rotation };
    for (auto& keyframe : keyframes)
        set_keyframe_property(keyframe, CSS::PropertyID::Rotate, rotations.rotation_for(direction));
    return keyframes;
}

}
