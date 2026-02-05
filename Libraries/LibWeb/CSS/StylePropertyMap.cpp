/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StylePropertyMap.h"
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StylePropertyMapPrototype.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/CSSUnparsedValue.h>
#include <LibWeb/CSS/CSSVariableReferenceValue.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
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

static bool any_have_non_matching_associated_property(FlyString const& property, Vector<Variant<GC::Root<CSSStyleValue>, String>> values)
{
    return any_of(values, [&property](Variant<GC::Root<CSSStyleValue>, String> const& value) {
        if (auto* style_value = value.get_pointer<GC::Root<CSSStyleValue>>()) {
            if (auto associated_property = (*style_value)->associated_property();
                associated_property.has_value() && associated_property != property)
                return true;
        }
        return false;
    });
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
static WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(JS::VM& vm, PropertyNameAndID const& property, Variant<GC::Root<CSSStyleValue>, String> const& value)
{
    // To create an internal representation, given a string property and a string or CSSStyleValue value:
    return value.visit(
        [&property](GC::Root<CSSStyleValue> const& css_style_value) {
            return css_style_value->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::Yes);
        },
        [&](String const& css_text) -> WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> {
            // If value is a USVString,
            //     Parse a CSSStyleValue with property property, cssText value, and parseMultiple set to false, and
            //     return the result.
            // FIXME: Avoid passing name as a string, as it gets immediately converted back to PropertyNameAndID.
            auto result = TRY(CSSStyleValue::parse_a_css_style_value(vm, property.name(), css_text, CSSStyleValue::ParseMultiple::No));
            // AD-HOC: Result is a CSSStyleValue but we want an internal representation, so... convert it again I guess?
            return result.get<GC::Ref<CSSStyleValue>>()->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::Yes);
        });
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-set
WebIDL::ExceptionOr<void> StylePropertyMap::set(FlyString property_name, Vector<Variant<GC::Root<CSSStyleValue>, String>> values)
{
    // The set(property, ...values) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property_name)) };

    // 3. If property is a single-valued property and values has more than one item, throw a TypeError.
    // NB: Custom properties should all be single-valued.
    if ((property->is_custom_property() || property_is_single_valued(property->id())) && values.size() > 1)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Property '{}' only accepts a single value", property_name)) };

    // 4. If any of the items in values have a non-null [[associatedProperty]] internal slot, and that slot’s value is
    //    anything other than property, throw a TypeError.
    if (any_have_non_matching_associated_property(property->name(), values))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "One of the passed CSSStyleValues has a different associated property"_string };

    // 5. If the size of values is two or more, and one or more of the items are a CSSUnparsedValue or
    //    CSSVariableReferenceValue object, throw a TypeError.
    // NOTE: Having 2+ values implies that you’re setting multiple items of a list-valued property, but the presence of
    //       a var() function in the string-based OM disables all syntax parsing, including splitting into individual
    //       iterations (because there might be more commas inside of the var() value, so you can’t tell how many items
    //       are actually going to show up). This step’s restriction preserves the same semantics in the Typed OM.
    // FIXME: This is done as part of step 9, because we need to detect if a string value would be an CSSUnparsedValue
    //        or CSSVariableReferenceValue. Spec issue: https://github.com/w3c/css-houdini-drafts/issues/1157

    // 6. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = declarations();

    // 7. If props[property] exists, remove it.
    // FIXME: Avoid converting to string and back.
    TRY(props.remove_property(property->name()));

    // 8. Let values to set be an empty list.
    StyleValueVector values_to_set;

    // 9. For each value in values, create an internal representation for property and value, and append the result to values to set.
    for (auto const& value : values) {
        // AD-HOC: Step 5 is done here, see above.
        auto internal_representation = TRY(create_an_internal_representation(vm(), property.value(), value));

        if (values.size() >= 2 && internal_representation->is_unresolved())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot provide multiple values if one is an CSSUnparsedValue or CSSVariableReferenceValue"_string };

        values_to_set.append(move(internal_representation));
    }

    // AD-HOC: To match the behavior of our parser we should store values of list-valued longhands as lists even if
    //         there is only one value, except in some rare circumstances.
    auto const should_wrap_value_in_list = [](PropertyNameAndID const& property, NonnullRefPtr<StyleValue const> const& value) {
        if (property_is_shorthand(property.id()))
            return false;

        if (!property_is_list_valued(property.id()))
            return false;

        // Values which are not yet fully resolved should not be wrapped in lists.
        if (value->is_unresolved() || value->is_pending_substitution() || value->is_guaranteed_invalid() || value->is_css_wide_keyword())
            return false;

        // Some "list-valued" properties have possible values that are not lists, and those should not be wrapped.
        if (property.id() == PropertyID::BackdropFilter && value->to_keyword() == Keyword::None)
            return false;

        if (first_is_one_of(property.id(), PropertyID::CounterIncrement, PropertyID::CounterReset, PropertyID::CounterSet) && value->to_keyword() == Keyword::None)
            return false;

        if (property.id() == PropertyID::Filter && value->to_keyword() == Keyword::None)
            return false;

        if (first_is_one_of(property.id(), PropertyID::FontFeatureSettings, PropertyID::FontVariationSettings) && value->to_keyword() == Keyword::Normal)
            return false;

        if (property.id() == PropertyID::Quotes && first_is_one_of(value->to_keyword(), Keyword::Auto, Keyword::None, Keyword::MatchParent))
            return false;

        if (property.id() == PropertyID::TransitionProperty && value->to_keyword() == Keyword::None)
            return false;

        if (property.id() == PropertyID::WillChange && value->to_keyword() == Keyword::Auto)
            return false;

        return true;
    };

    // 10. Set props[property] to values to set.
    // NOTE: The property is deleted then added back so that it gets put at the end of the ordered map, which gives the
    //       expected behavior in the face of shorthand properties.
    if (values_to_set.size() == 1 && !should_wrap_value_in_list(property.value(), values_to_set.first())) {
        TRY(props.set_property_style_value(property.value(), values_to_set.take_first()));
    } else {
        // FIXME: How do we know if this is comma-separated or not?
        auto values_list = StyleValueList::create(move(values_to_set), StyleValueList::Separator::Comma);
        TRY(props.set_property_style_value(property.value(), move(values_list)));
    }

    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-append
WebIDL::ExceptionOr<void> StylePropertyMap::append(FlyString property_name, Vector<Variant<GC::Root<CSSStyleValue>, String>> values)
{
    // The append(property, ...values) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value()) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property_name)) };
    }

    // 3. If property is not a list-valued property, throw a TypeError.
    if (!property_is_list_valued(property->id())) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a list-valued property", property_name)) };
    }

    // 4. If any of the items in values have a non-null [[associatedProperty]] internal slot, and that slot’s value is
    //    anything other than property, throw a TypeError.
    if (any_have_non_matching_associated_property(property->name(), values)) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "One of the passed CSSStyleValues has a different associated property"_string };
    }

    // 5. If any of the items in values are a CSSUnparsedValue or CSSVariableReferenceValue object, throw a TypeError.
    // NOTE: When a property is set via string-based APIs, the presence of var() in a property prevents the entire
    //       thing from being interpreted. In other words, everything besides the var() is a plain component value, not
    //       a meaningful type. This step’s restriction preserves the same semantics in the Typed OM.
    // FIXME: This is done as part of step 10, because we need to detect if a string value would be an CSSUnparsedValue
    //        or CSSVariableReferenceValue. Spec issue: https://github.com/w3c/css-houdini-drafts/issues/1157

    // 6. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = declarations();

    // 7. If props[property] does not exist, set props[property] to an empty list.
    auto existing_value = props.get_property_style_value(property.value());

    // 8. If props[property] contains a var() reference, throw a TypeError.
    if (existing_value && existing_value->is_unresolved()) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Existing value for '{}' contains var() references.", property_name)) };
    }

    // 9. Let temp values be an empty list.
    // NB: StyleValues are immutable, so we always create a new one. We add directly to it instead of using "temp values".
    StyleValueVector value_list;
    if (existing_value) {
        if (existing_value->is_value_list())
            value_list.extend(existing_value->as_value_list().values());
        else
            value_list.append(existing_value.release_nonnull());
    }

    // 10. For each value in values, create an internal representation with property and value, and append the returned value to temp values.
    for (auto const& value : values) {
        // AD-HOC: Step 5 is done here, see above.
        auto internal_representation = TRY(create_an_internal_representation(vm(), property.value(), value));

        if (internal_representation->is_unresolved())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot append a CSSUnparsedValue or CSSVariableReferenceValue"_string };

        value_list.append(move(internal_representation));
    }

    // 11. Append the entries of temp values to props[property].
    // NB: StyleValues are immutable, so we create a new one instead.
    // FIXME: How do we know if this is comma-separated or not?
    return props.set_property_style_value(property.value(), StyleValueList::create(move(value_list), StyleValueList::Separator::Comma));
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
