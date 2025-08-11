/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StylePropertyMapReadOnly.h"
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StylePropertyMapReadOnlyPrototype.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StylePropertyMapReadOnly);

GC::Ref<StylePropertyMapReadOnly> StylePropertyMapReadOnly::create_computed_style(JS::Realm& realm, DOM::AbstractElement element)
{
    return realm.create<StylePropertyMapReadOnly>(realm, element);
}

StylePropertyMapReadOnly::StylePropertyMapReadOnly(JS::Realm& realm, Optional<DOM::AbstractElement> element)
    : Bindings::PlatformObject(realm)
    , m_source_element(move(element))
{
}

StylePropertyMapReadOnly::StylePropertyMapReadOnly(JS::Realm& realm, GC::Ref<CSSStyleDeclaration> declaration)
    : Bindings::PlatformObject(realm)
    , m_source_declaration(declaration)
{
}

StylePropertyMapReadOnly::~StylePropertyMapReadOnly() = default;

void StylePropertyMapReadOnly::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StylePropertyMapReadOnly);
    Base::initialize(realm);
}

void StylePropertyMapReadOnly::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_source_declaration);
    if (m_source_element.has_value())
        m_source_element->visit(visitor);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-get
WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> StylePropertyMapReadOnly::get(String property)
{
    // The get(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    if (!is_a_custom_property_name_string(property))
        property = property.to_ascii_lowercase();

    // 2. If property is not a valid CSS property, throw a TypeError.
    if (!is_a_valid_css_property(property))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property)) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // FIXME: 4. If props[property] exists, subdivide into iterations props[property], then reify the first item of the result and return it.
    (void)props;

    // 5. Otherwise, return undefined.
    return Empty {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-getall
WebIDL::ExceptionOr<Vector<GC::Ref<CSSStyleValue>>> StylePropertyMapReadOnly::get_all(String property)
{
    // The getAll(property) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    if (!is_a_custom_property_name_string(property))
        property = property.to_ascii_lowercase();

    // 2. If property is not a valid CSS property, throw a TypeError.
    if (!is_a_valid_css_property(property))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property)) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // FIXME: 4. If props[property] exists, subdivide into iterations props[property], then reify each item of the result, and return the list.
    (void)props;

    // 5. Otherwise, return an empty list.
    return Vector<GC::Ref<CSSStyleValue>> {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-has
WebIDL::ExceptionOr<bool> StylePropertyMapReadOnly::has(String property)
{
    // The has(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    if (!is_a_custom_property_name_string(property))
        property = property.to_ascii_lowercase();

    // 2. If property is not a valid CSS property, throw a TypeError.
    if (!is_a_valid_css_property(property))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property)) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // 4. If props[property] exists, return true. Otherwise, return false.
    return props.contains(property);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-size
WebIDL::UnsignedLong StylePropertyMapReadOnly::size() const
{
    // The size attribute, on getting from a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. Return the size of the value of this’s [[declarations]] internal slot.
    return m_declarations.size();
}

}
