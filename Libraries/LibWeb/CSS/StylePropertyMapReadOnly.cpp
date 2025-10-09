/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StylePropertyMapReadOnly.h"
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StylePropertyMapReadOnlyPrototype.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StylePropertyMapReadOnly);

GC::Ref<StylePropertyMapReadOnly> StylePropertyMapReadOnly::create_computed_style(JS::Realm& realm, DOM::AbstractElement element)
{
    return realm.create<StylePropertyMapReadOnly>(realm, element);
}

StylePropertyMapReadOnly::StylePropertyMapReadOnly(JS::Realm& realm, Source source)
    : Bindings::PlatformObject(realm)
    , m_declarations(move(source))
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

    m_declarations.visit(
        [&visitor](DOM::AbstractElement& element) { element.visit(visitor); },
        [&visitor](GC::Ref<CSSStyleDeclaration>& declaration) { visitor.visit(declaration); });
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-get
WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> StylePropertyMapReadOnly::get(String const& property_name)
{
    // The get(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property_name)) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // 4. If props[property] exists, subdivide into iterations props[property], then reify the first item of the result and return it.
    if (auto property_value = get_style_value(props, property.value())) {
        // FIXME: Subdivide into iterations, and only reify/return the first.
        return property_value->reify(realm(), property->name());
    }

    // 5. Otherwise, return undefined.
    return Empty {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-getall
WebIDL::ExceptionOr<Vector<GC::Ref<CSSStyleValue>>> StylePropertyMapReadOnly::get_all(String const& property_name)
{
    // The getAll(property) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property_name)) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // 4. If props[property] exists, subdivide into iterations props[property], then reify each item of the result, and return the list.
    if (auto property_value = get_style_value(props, property.value())) {
        // FIXME: Subdivide into iterations.
        return Vector { property_value->reify(realm(), property->name()) };
    }

    // 5. Otherwise, return an empty list.
    return Vector<GC::Ref<CSSStyleValue>> {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-has
WebIDL::ExceptionOr<bool> StylePropertyMapReadOnly::has(String const& property_name)
{
    // The has(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property_name)) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // 4. If props[property] exists, return true. Otherwise, return false.
    return props.visit(
        [&property](DOM::AbstractElement& element) {
            // From https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap we need to include:
            // "the name and computed value of every longhand CSS property supported by the User Agent, every
            // registered custom property, and every non-registered custom property which is not set to its initial
            // value on this"
            // Ensure style is computed on the element before we try to read it, so we can check custom properties.
            element.document().update_style();
            if (property->is_custom_property()) {
                if (element.get_custom_property(property->name()))
                    return true;
                return element.document().registered_custom_properties().contains(property->name());
            }
            return true;
        },
        [&property](GC::Ref<CSSStyleDeclaration>& declaration) {
            return declaration->has_property(property.value());
        });
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-size
WebIDL::UnsignedLong StylePropertyMapReadOnly::size() const
{
    // The size attribute, on getting from a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. Return the size of the value of this’s [[declarations]] internal slot.
    return m_declarations.visit(
        [](DOM::AbstractElement const& element) {
            // From https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap we need to include:
            // "the name and computed value of every longhand CSS property supported by the User Agent, every
            // registered custom property, and every non-registered custom property which is not set to its initial
            // value on this"
            // Ensure style is computed on the element before we try to read it.
            element.document().update_style();

            // Some custom properties set on the element might also be in the registered custom properties set, so we
            // want the size of the union of the two sets.
            HashTable<FlyString> custom_properties;
            for (auto const& key : element.custom_properties().keys())
                custom_properties.set(key);
            for (auto const& [key, _] : element.document().registered_custom_properties())
                custom_properties.set(key);

            return number_of_longhand_properties + custom_properties.size();
        },
        [](GC::Ref<CSSStyleDeclaration> const& declaration) { return declaration->length(); });
}

RefPtr<StyleValue const> StylePropertyMapReadOnly::get_style_value(Source& source, PropertyNameAndID const& property)
{
    return source.visit(
        [&property](DOM::AbstractElement& element) -> RefPtr<StyleValue const> {
            // From https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap we need to include:
            // "the name and computed value of every longhand CSS property supported by the User Agent, every
            // registered custom property, and every non-registered custom property which is not set to its initial
            // value on this"
            // Ensure style is computed on the element before we try to read it.
            element.document().update_style();
            if (property.is_custom_property()) {
                if (auto custom_property = element.get_custom_property(property.name()))
                    return custom_property;
                if (auto registered_custom_property = element.document().registered_custom_properties().get(property.name()); registered_custom_property.has_value())
                    return registered_custom_property.value()->initial_style_value();
                return nullptr;
            }

            if (property.id() >= first_longhand_property_id && property.id() <= last_longhand_property_id) {
                // FIXME: This will only ever be null for pseudo-elements. What should we do in that case?
                if (auto computed_properties = element.computed_properties())
                    return computed_properties->property(property.id());
            }

            return nullptr;
        },
        [&property](GC::Ref<CSSStyleDeclaration>& declaration) {
            return declaration->get_property_style_value(property);
        });
}

}
