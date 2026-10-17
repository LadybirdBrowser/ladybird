/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGSetElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSetElement);

SVGSetElement::SVGSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGAnimationElement(document, move(qualified_name))
{
}

void SVGSetElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (name == AttributeNames::to && !namespace_.has_value())
        update_animation();
}

// https://svgwg.org/specs/animations/#SetElementToAttribute
Vector<Animations::BaseKeyframe> SVGSetElement::keyframes(SVGElement const& target, CSS::PropertyID property_id) const
{
    // Specifies the value for the attribute during the duration of the ‘set’ element. The argument value must match the
    // attribute type.
    auto to = get_attribute(AttributeNames::to);
    if (!to.has_value())
        return {};
    auto value = target.parse_presentation_attribute(property_id, to->utf16_view());
    if (!value)
        return {};

    // NB: The value holds for the entire duration, so the effect starts and ends on it.
    Vector<Animations::BaseKeyframe> keyframes;
    for (auto offset : { 0.0, 1.0 }) {
        Animations::BaseKeyframe keyframe;
        keyframe.offset = offset;
        Animations::BaseKeyframe::ParsedProperties properties;
        properties.set(CSS::PropertyNameAndID::from_id(property_id), CSS::RustStyleValueHandle::retained(value->rust_style_value_data()));
        keyframe.properties = move(properties);
        keyframes.append(move(keyframe));
    }
    return keyframes;
}

}
