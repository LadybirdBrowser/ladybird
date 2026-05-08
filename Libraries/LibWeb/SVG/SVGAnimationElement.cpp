/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGAnimationElement.h>

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimationElement.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/Layout/Node.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimationElement);

SVGAnimationElement::SVGAnimationElement(DOM::Document& document, DOM::QualifiedName name)
    : SVGElement(document, name)
{
}

void SVGAnimationElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimationElement);
    Base::initialize(realm);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
void SVGAnimationElement::set_onbegin(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::begin, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onbegin()
{
    return event_handler_attribute(HTML::EventNames::begin);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
void SVGAnimationElement::set_onend(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::end, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onend()
{
    return event_handler_attribute(HTML::EventNames::error);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
void SVGAnimationElement::set_onrepeat(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::end, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onrepeat()
{
    return event_handler_attribute(HTML::EventNames::repeat);
}

}
