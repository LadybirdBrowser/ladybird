/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGAnimationElement.h>

#include <AK/Math.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SMILAnimation.h>

namespace Web::SVG {

SVGAnimationElement::SVGAnimationElement(DOM::Document& document, DOM::QualifiedName name)
    : SVGElement(document, name)
{
}

void SVGAnimationElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
}

void SVGAnimationElement::inserted()
{
    Base::inserted();
    update_animation();
}

void SVGAnimationElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_animation();
}

void SVGAnimationElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
    update_animation();
}

void SVGAnimationElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (name == AttributeNames::attributeName && !namespace_.has_value())
        update_animation();
}

// https://svgwg.org/specs/animations/#HrefAttribute
GC::Ptr<SVGElement> SVGAnimationElement::target_element()
{
    // FIXME: A URL reference to the element which is the target of this animation element and which therefore will be
    //        modified over time.
    // If the ‘href’ attribute or the deprecated ‘xlink:href’ attribute is not provided, then the target element will
    // be the immediate parent element of the current animation element.
    return as_if<SVGElement>(parent_element().ptr());
}

// https://svgwg.org/specs/animations/#AttributeNameAttribute
Optional<CSS::PropertyID> SVGAnimationElement::target_property() const
{
    auto attribute_name = get_attribute(AttributeNames::attributeName);
    if (!attribute_name.has_value())
        return {};

    // That is, when determining if ‘attributeName’ corresponds to an attribute name or a CSS property name, the
    // implementation must first search through the list of CSS properties for a matching property name.
    // NB: Neither custom properties nor properties that cannot be animated have an animation.
    auto property_id = CSS::property_id_from_string(*attribute_name);
    if (!property_id.has_value() || property_id == CSS::PropertyID::Custom || !CSS::is_animatable_property(*property_id))
        return {};

    // FIXME: If no matching property is found, the implementation must search for a matching attribute on the target
    //        element.
    return property_id;
}

void SVGAnimationElement::update_animation()
{
    if (m_animation) {
        m_animation->cancel();
        m_animation = nullptr;
    }

    if (!is_connected())
        return;
    auto target = target_element();
    if (!target)
        return;
    auto property_id = target_property();
    if (!property_id.has_value())
        return;
    auto keyframes = this->keyframes(*target, *property_id);
    if (keyframes.is_empty())
        return;

    // FIXME: Resolve the interval to play from the begin, dur and end attributes, and the other timing attributes.
    // FIXME: A running animation requests a rendering update on every frame, even when its output cannot change, as
    //        with this indefinite one. This keeps the rendering loop running for as long as the element is connected.
    auto effect = MUST(Animations::KeyframeEffect::create_from_processed_keyframes(target, move(keyframes), AK::Infinity<double>));

    // AD-HOC: Animations begin once their element is connected, on the document's default timeline. They should
    //         begin at the document begin of their SVG document fragment, which is the time of its load event.
    auto animation = SMILAnimation::create(*this);
    animation->set_timeline(document().timeline());
    animation->set_effect(effect);
    HTML::TemporaryExecutionContext execution_context { document().relevant_settings_object() };
    MUST(animation->play());
    m_animation = animation;
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
void SVGAnimationElement::set_onbegin(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::beginEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onbegin()
{
    return event_handler_attribute(HTML::EventNames::beginEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
void SVGAnimationElement::set_onend(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::endEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onend()
{
    return event_handler_attribute(HTML::EventNames::endEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
void SVGAnimationElement::set_onrepeat(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::repeatEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onrepeat()
{
    return event_handler_attribute(HTML::EventNames::repeatEvent);
}

}
