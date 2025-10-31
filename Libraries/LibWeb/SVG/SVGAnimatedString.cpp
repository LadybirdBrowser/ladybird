/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedStringPrototype.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedString);

GC::Ref<SVGAnimatedString> SVGAnimatedString::create(JS::Realm& realm, GC::Ref<SVGElement> element, DOM::QualifiedName reflected_attribute, Optional<DOM::QualifiedName> deprecated_reflected_attribute, Optional<FlyString> initial_value)
{
    return realm.create<SVGAnimatedString>(realm, element, move(reflected_attribute), move(deprecated_reflected_attribute), move(initial_value));
}

SVGAnimatedString::SVGAnimatedString(JS::Realm& realm, GC::Ref<SVGElement> element, DOM::QualifiedName reflected_attribute, Optional<DOM::QualifiedName> deprecated_reflected_attribute, Optional<FlyString> initial_value)
    : Bindings::PlatformObject(realm)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_deprecated_reflected_attribute(move(deprecated_reflected_attribute))
    , m_initial_value(move(initial_value))
{
}

SVGAnimatedString::~SVGAnimatedString() = default;

void SVGAnimatedString::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedString);
    Base::initialize(realm);
}

void SVGAnimatedString::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedString__baseVal
String SVGAnimatedString::base_val() const
{
    // On getting baseVal or animVal, the following steps are run:
    // 1. If the reflected attribute is not present, then:
    if (!m_element->has_attribute_ns(m_reflected_attribute.namespace_(), m_reflected_attribute.local_name())) {
        // 1. If the SVGAnimatedString object is defined to additionally reflect a second, deprecated attribute,
        //    and that attribute is present, then return its value.
        if (m_deprecated_reflected_attribute.has_value()) {
            if (auto attribute = m_element->get_attribute_ns(m_deprecated_reflected_attribute->namespace_(), m_deprecated_reflected_attribute->local_name()); attribute.has_value())
                return attribute.release_value();
        }

        // 2. Otherwise, if the reflected attribute has an initial value, then return it.
        if (m_initial_value.has_value())
            return m_initial_value.value().to_string();

        // 3. Otherwise, return the empty string.
        return {};
    }

    // 2. Otherwise, the reflected attribute is present. Return its value.
    return m_element->get_attribute_ns(m_reflected_attribute.namespace_(), m_reflected_attribute.local_name()).value();
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedString__baseVal
void SVGAnimatedString::set_base_val(String const& base_val)
{
    // 1. If the reflected attribute is not present, the SVGAnimatedString object is defined to additionally reflect
    //    a second, deprecated attribute, and that deprecated attribute is present, then set that deprecated attribute
    //    to the specified value.
    if (!m_element->has_attribute_ns(m_reflected_attribute.namespace_(), m_reflected_attribute.local_name())
        && m_deprecated_reflected_attribute.has_value()
        && m_element->has_attribute_ns(m_deprecated_reflected_attribute->namespace_(), m_deprecated_reflected_attribute->local_name())) {
        m_element->set_attribute_value(m_deprecated_reflected_attribute->local_name(), base_val, m_deprecated_reflected_attribute->prefix(), m_deprecated_reflected_attribute->namespace_());
        return;
    }

    // 2. Otherwise, set the reflected attribute to the specified value.
    m_element->set_attribute_value(m_reflected_attribute.local_name(), base_val, m_reflected_attribute.prefix(), m_reflected_attribute.namespace_());
}

}
