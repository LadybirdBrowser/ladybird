/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/DOMStringMap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/DOMStringMap.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DOMStringMap);

GC::Ref<DOMStringMap> DOMStringMap::create(DOM::Element& element)
{
    auto& realm = element.realm();
    return realm.create<DOMStringMap>(element);
}

DOMStringMap::DOMStringMap(DOM::Element& element)
    : PlatformObject(element.realm())
    , m_associated_element(element)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_named_properties = true,
        .has_named_property_setter = true,
        .has_named_property_deleter = true,
        .has_legacy_override_built_ins_interface_extended_attribute = true,
    };
}

DOMStringMap::~DOMStringMap() = default;

void DOMStringMap::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMStringMap);
    Base::initialize(realm);
}

void DOMStringMap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_element);
}

static bool is_data_attribute_name(Utf16View name)
{
    // This implements the attribute-name predicate from getting the DOMStringMap's name-value pairs:
    //
    // 2. For each content attribute on the DOMStringMap's associated element whose first five characters are the
    //    string "data-" and whose remaining characters (if any) do not include any ASCII upper alphas, ...
    if (!name.starts_with(u"data-"sv))
        return false;

    auto name_after_starting_data = name.substring_view(5);
    for (size_t character_index = 0; character_index < name_after_starting_data.length_in_code_units(); ++character_index) {
        auto character = name_after_starting_data.code_unit_at(character_index);
        if (is_ascii_upper_alpha(character))
            return false;
    }

    return true;
}

static Optional<Utf16FlyString> property_name_from_data_attribute_name(Utf16View name)
{
    if (!is_data_attribute_name(name))
        return {};

    auto name_after_starting_data = name.substring_view(5);

    // 2. For each content attribute on the DOMStringMap's associated element whose first five characters are the
    //    string "data-" and whose remaining characters (if any) do not include any ASCII upper alphas, in the order
    //    that those attributes are listed in the element's attribute list, add a name-value pair to list whose name is
    //    the attribute's name with the first five characters removed and whose value is the attribute's value.
    //
    // 3. For each name in list, for each U+002D HYPHEN-MINUS character (-) in the name that is followed by an ASCII
    //    lower alpha, remove the U+002D HYPHEN-MINUS character (-) and replace the character that followed it by the
    //    same character converted to ASCII uppercase.
    Utf16StringBuilder builder;
    for (size_t character_index = 0; character_index < name_after_starting_data.length_in_code_units(); ++character_index) {
        auto current_character = name_after_starting_data.code_unit_at(character_index);

        if (character_index + 1 < name_after_starting_data.length_in_code_units() && current_character == '-') {
            auto next_character = name_after_starting_data.code_unit_at(character_index + 1);

            if (is_ascii_lower_alpha(next_character)) {
                builder.append_code_unit(to_ascii_uppercase(next_character));

                // Skip the next character
                ++character_index;

                continue;
            }
        }

        builder.append_code_unit(current_character);
    }

    return Utf16FlyString::from_utf16(builder.view());
}

static bool data_attribute_name_matches_property_name(Utf16View attribute_name, Utf16View property_name)
{
    if (!is_data_attribute_name(attribute_name))
        return false;

    // This performs the name transformation from getting the DOMStringMap's name-value pairs, but compares each UTF-16
    // code unit directly instead of constructing the transformed name.
    auto name_after_starting_data = attribute_name.substring_view(5);
    size_t property_name_index = 0;

    for (size_t character_index = 0; character_index < name_after_starting_data.length_in_code_units(); ++character_index) {
        auto current_character = name_after_starting_data.code_unit_at(character_index);

        if (character_index + 1 < name_after_starting_data.length_in_code_units() && current_character == '-') {
            auto next_character = name_after_starting_data.code_unit_at(character_index + 1);

            if (is_ascii_lower_alpha(next_character)) {
                current_character = to_ascii_uppercase(next_character);
                ++character_index;
            }
        }

        if (property_name_index >= property_name.length_in_code_units())
            return false;
        if (property_name.code_unit_at(property_name_index) != current_character)
            return false;
        ++property_name_index;
    }

    return property_name_index == property_name.length_in_code_units();
}

// https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-pairs
Vector<Utf16FlyString> DOMStringMap::supported_property_names() const
{
    // The supported property names on a DOMStringMap object at any instant are the names of each pair returned from
    // getting the DOMStringMap's name-value pairs at that instant, in the order returned.
    //
    // 1. Let list be an empty list of name-value pairs.
    Vector<Utf16FlyString> names;

    m_associated_element->for_each_attribute([&](Utf16FlyString const& name, auto const&) {
        auto property_name = property_name_from_data_attribute_name(name.view());
        if (property_name.has_value())
            names.append(property_name.release_value());
    });

    // 4. Return list.
    return names;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-nameditem
Utf16String DOMStringMap::determine_value_of_named_property(Utf16View name) const
{
    // To determine the value of a named property name for a DOMStringMap, return the value component of the name-value pair whose name component is name in the list returned from getting the
    // DOMStringMap's name-value pairs.
    Optional<Utf16String> optional_value;
    m_associated_element->for_each_attribute([&](Utf16FlyString const& attribute_name, Utf16View value) {
        if (optional_value.has_value())
            return;
        if (data_attribute_name_matches_property_name(attribute_name.view(), name))
            optional_value = Utf16String::from_utf16(value);
    });

    // NOTE: determine_value_of_named_property is only called if `name` is in supported_property_names.
    VERIFY(optional_value.has_value());

    return optional_value.release_value();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-setitem
WebIDL::ExceptionOr<void> DOMStringMap::set_value_of_new_named_property(Utf16FlyString const& name, JS::Value unconverted_value)
{
    // NOTE: Since PlatformObject does not know the type of value, we must convert it ourselves.
    //       The type of `value` is `DOMString`.
    auto value = TRY(unconverted_value.to_utf16_string(vm()));

    Utf16StringBuilder builder;

    // 3. Insert the string data- at the front of name.
    // NOTE: This is done out of order because StringBuilder doesn't have prepend.
    builder.append("data-"_utf16);

    auto name_view = name.view();

    for (size_t character_index = 0; character_index < name_view.length_in_code_units(); ++character_index) {
        // 1. If name contains a U+002D HYPHEN-MINUS character (-) followed by an ASCII lower alpha, then throw a "SyntaxError" DOMException.
        auto current_character = name_view.code_unit_at(character_index);

        if (current_character == '-' && character_index + 1 < name_view.length_in_code_units()) {
            auto next_character = name_view.code_unit_at(character_index + 1);
            if (is_ascii_lower_alpha(next_character))
                return WebIDL::SyntaxError::create(realm(), "Name cannot contain a '-' followed by a lowercase character."_utf16);
        }

        // 2. For each ASCII upper alpha in name, insert a U+002D HYPHEN-MINUS character (-) before the character and replace the character with the same character converted to ASCII lowercase.
        if (is_ascii_upper_alpha(current_character)) {
            builder.append_ascii('-');
            builder.append_code_unit(to_ascii_lowercase(current_character));
            continue;
        }

        builder.append_code_unit(current_character);
    }

    // 4. If name is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (!DOM::is_valid_attribute_local_name(builder.view()))
        return WebIDL::InvalidCharacterError::create(realm(), "Name is not a valid attribute local name."_utf16);

    // 5. Set an attribute value for the DOMStringMap's associated element using name and value.
    auto data_name = Utf16FlyString::from_utf16(builder.view());
    m_associated_element->set_attribute_value(data_name, value);

    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-setitem
WebIDL::ExceptionOr<void> DOMStringMap::set_value_of_existing_named_property(Utf16FlyString const& name, JS::Value value)
{
    return set_value_of_new_named_property(name, value);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-removeitem
WebIDL::ExceptionOr<Bindings::PlatformObject::DidDeletionFail> DOMStringMap::delete_value(Utf16FlyString const& name)
{
    Utf16StringBuilder builder;

    // 2. Insert the string data- at the front of name.
    // NOTE: This is done out of order because StringBuilder doesn't have prepend.
    builder.append("data-"_utf16);

    for (auto character : name.view()) {
        // 1. For each ASCII upper alpha in name, insert a U+002D HYPHEN-MINUS character (-) before the character and replace the character with the same character converted to ASCII lowercase.
        if (is_ascii_upper_alpha(character)) {
            builder.append_ascii('-');
            builder.append_code_unit(to_ascii_lowercase(character));
            continue;
        }

        builder.append_code_point(character);
    }

    // Remove an attribute by name given name and the DOMStringMap's associated element.
    auto data_name = Utf16FlyString::from_utf16(builder.view());
    m_associated_element->remove_attribute(data_name);

    // The spec doesn't have the step. This indicates that the deletion was successful.
    return DidDeletionFail::No;
}

JS::Value DOMStringMap::named_item_value(Utf16FlyString const& name) const
{
    return JS::PrimitiveString::create(vm(), determine_value_of_named_property(name.view()));
}

}
