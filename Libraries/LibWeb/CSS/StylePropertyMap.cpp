/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StylePropertyMap.h"
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StylePropertyMapPrototype.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StylePropertyMap);

GC::Ref<StylePropertyMap> StylePropertyMap::create(JS::Realm& realm, GC::Ref<CSSStyleDeclaration> declaration)
{
    return realm.create<StylePropertyMap>(realm, declaration);
}

StylePropertyMap::StylePropertyMap(JS::Realm& realm, GC::Ref<CSSStyleDeclaration> declaration)
    : StylePropertyMapReadOnly(realm, declaration)
{
}

StylePropertyMap::~StylePropertyMap() = default;

CSSStyleDeclaration& StylePropertyMap::declarations()
{
    // Writable StylePropertyMaps must be backed by a CSSStyleDeclaration, not an AbstractElement.
    return m_declarations.get<GC::Ref<CSSStyleDeclaration>>();
}

void StylePropertyMap::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StylePropertyMap);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-set
WebIDL::ExceptionOr<void> StylePropertyMap::set(FlyString property, Vector<Variant<GC::Root<CSSStyleValue>, String>> values)
{
    // The set(property, ...values) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    if (!is_a_custom_property_name_string(property))
        property = property.to_ascii_lowercase();

    // 2. If property is not a valid CSS property, throw a TypeError.
    if (!is_a_valid_css_property(property))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property)) };

    // FIXME: 3. If property is a single-valued property and values has more than one item, throw a TypeError.

    // FIXME: 4. If any of the items in values have a non-null [[associatedProperty]] internal slot, and that slot’s value is anything other than property, throw a TypeError.

    // FIXME: 5. If the size of values is two or more, and one or more of the items are a CSSUnparsedValue or CSSVariableReferenceValue object, throw a TypeError.
    // NOTE: Having 2+ values implies that you’re setting multiple items of a list-valued property, but the presence of
    //       a var() function in the string-based OM disables all syntax parsing, including splitting into individual
    //       iterations (because there might be more commas inside of the var() value, so you can’t tell how many items
    //       are actually going to show up). This step’s restriction preserves the same semantics in the Typed OM.

    // 6. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = declarations();

    // 7. If props[property] exists, remove it.
    TRY(props.remove_property(property));

    // FIXME: 8. Let values to set be an empty list.

    // FIXME: 9. For each value in values, create an internal representation for property and value, and append the result to values to set.
    (void)values;

    // FIXME: 10. Set props[property] to values to set.
    // NOTE: The property is deleted then added back so that it gets put at the end of the ordered map, which gives the
    //       expected behavior in the face of shorthand properties.

    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-append
WebIDL::ExceptionOr<void> StylePropertyMap::append(FlyString property, Vector<Variant<GC::Root<CSSStyleValue>, String>> values)
{
    // The append(property, ...values) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    if (!is_a_custom_property_name_string(property))
        property = property.to_ascii_lowercase();

    // 2. If property is not a valid CSS property, throw a TypeError.
    if (!is_a_valid_css_property(property))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property)) };

    // FIXME: 3. If property is not a list-valued property, throw a TypeError.

    // FIXME: 4. If any of the items in values have a non-null [[associatedProperty]] internal slot, and that slot’s value is anything other than property, throw a TypeError.

    // FIXME: 5. If any of the items in values are a CSSUnparsedValue or CSSVariableReferenceValue object, throw a TypeError.

    // NOTE: When a property is set via string-based APIs, the presence of var() in a property prevents the entire thing from being interpreted. In other words, everything besides the var() is a plain component value, not a meaningful type. This step’s restriction preserves the same semantics in the Typed OM.

    // 6. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = declarations();

    // FIXME: 7. If props[property] does not exist, set props[property] to an empty list.
    (void)props;

    // FIXME: 8. If props[property] contains a var() reference, throw a TypeError.

    // FIXME: 9. Let temp values be an empty list.

    // FIXME: 10. For each value in values, create an internal representation with property and value, and append the returned value to temp values.
    (void)values;

    // FIXME: 11. Append the entries of temp values to props[property].

    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-delete
WebIDL::ExceptionOr<void> StylePropertyMap::delete_(FlyString property)
{
    // The delete(property) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    if (!is_a_custom_property_name_string(property))
        property = property.to_ascii_lowercase();

    // 2. If property is not a valid CSS property, throw a TypeError.
    if (!is_a_valid_css_property(property))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property)) };

    // 3. If this’s [[declarations]] internal slot contains property, remove it.
    TRY(declarations().remove_property(property));
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-clear
WebIDL::ExceptionOr<void> StylePropertyMap::clear()
{
    // The clear() method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. Remove all of the declarations in this’s [[declarations]] internal slot.
    return declarations().set_css_text(""sv);
}

}
