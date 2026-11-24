/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/dom.html#domstringmap

use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Trace, Utf16StringBuilder, Utf16View, Vm};

use crate::bindings::cpp_cell::{LentCppCell, cpp_class};
use crate::bindings::interface::{Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::element::{CppElement, is_valid_attribute_local_name};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMStringMap {
    // https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-element
    associated_element: CppElement,
}

impl Wrappable for DOMStringMap {}

impl DOMStringMap {
    // https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-pairs
    pub(crate) fn supported_property_names(&self, _: &mut CallContext) -> Vec<Utf16String> {
        // The supported property names on a DOMStringMap object at any instant are the names of each pair returned from
        // getting the DOMStringMap's name-value pairs at that instant, in the order returned.
        //
        // 1. Let list be an empty list of name-value pairs.
        let mut names = Vec::new();

        self.associated_element.for_each_attribute(|name, _| {
            if let Some(property_name) = property_name_from_data_attribute_name(Utf16View::of_fly_string(name)) {
                names.push(property_name);
            }
        });

        // 4. Return list.
        names
    }

    // https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-nameditem
    pub(crate) fn determine_value_of_named_property(
        &self,
        _: &mut CallContext,
        name: Utf16String,
    ) -> WebIdlResult<Utf16String> {
        // To determine the value of a named property name for a DOMStringMap, return the value component of the
        // name-value pair whose name component is name in the list returned from getting the DOMStringMap's name-value
        // pairs.
        let mut value = None;
        self.associated_element
            .for_each_attribute(|attribute_name, attribute_value| {
                if value.is_some() {
                    return;
                }
                if data_attribute_name_matches_property_name(
                    Utf16View::of_fly_string(attribute_name),
                    Utf16View::of_string(&name),
                ) {
                    value = Some(attribute_value.clone());
                }
            });

        Ok(value.expect("a DOMStringMap determines the value of a supported property name"))
    }

    pub(crate) fn set_value_of_new_named_property(
        &self,
        _: &mut CallContext,
        name: Utf16String,
        value: Utf16String,
    ) -> WebIdlResult<()> {
        self.set_value_of_named_property(&name, value)
    }

    pub(crate) fn set_value_of_existing_named_property(
        &self,
        _: &mut CallContext,
        name: Utf16String,
        value: Utf16String,
    ) -> WebIdlResult<()> {
        self.set_value_of_named_property(&name, value)
    }

    // https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-setitem
    fn set_value_of_named_property(&self, name: &Utf16String, value: Utf16String) -> WebIdlResult<()> {
        let mut builder = Utf16StringBuilder::new();

        // 3. Insert the string data- at the front of name.
        // NB: This is done out of order because the builder cannot prepend.
        builder.append_ascii("data-");

        let name = Utf16View::of_string(name);

        for character_index in 0..name.length_in_code_units() {
            // 1. If name contains a U+002D HYPHEN-MINUS character (-) followed by an ASCII lower alpha, then throw a
            //    "SyntaxError" DOMException.
            let current_character = name.code_unit_at(character_index);

            if current_character == u16::from(b'-') && character_index + 1 < name.length_in_code_units() {
                let next_character = name.code_unit_at(character_index + 1);
                if is_ascii_lower_alpha(next_character) {
                    return Err(Exception::syntax_error(Utf16String::from_utf8(
                        "Name cannot contain a '-' followed by a lowercase character.",
                    )));
                }
            }

            // 2. For each ASCII upper alpha in name, insert a U+002D HYPHEN-MINUS character (-) before the character
            //    and replace the character with the same character converted to ASCII lowercase.
            if is_ascii_upper_alpha(current_character) {
                builder.append_ascii("-");
                builder.append_code_unit(to_ascii_lowercase(current_character));
                continue;
            }

            builder.append_code_unit(current_character);
        }

        // 4. If name is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
        let data_name = builder.to_utf16_string();
        if !is_valid_attribute_local_name(&data_name) {
            return Err(Exception::Dom(
                "InvalidCharacterError",
                Utf16String::from_utf8("Name is not a valid attribute local name."),
            ));
        }

        // 5. Set an attribute value for the DOMStringMap's associated element using name and value.
        let data_name = Utf16FlyString::from_utf16_string(&data_name);
        self.associated_element.set_attribute_value(&data_name, value);

        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/dom.html#dom-domstringmap-removeitem
    pub(crate) fn delete_existing_named_property(&self, _: &mut CallContext, name: Utf16String) -> WebIdlResult<()> {
        let mut builder = Utf16StringBuilder::new();

        // 2. Insert the string data- at the front of name.
        // NB: This is done out of order because the builder cannot prepend.
        builder.append_ascii("data-");

        for character in Utf16View::of_string(&name).code_units() {
            // 1. For each ASCII upper alpha in name, insert a U+002D HYPHEN-MINUS character (-) before the character
            //    and replace the character with the same character converted to ASCII lowercase.
            if is_ascii_upper_alpha(character) {
                builder.append_ascii("-");
                builder.append_code_unit(to_ascii_lowercase(character));
                continue;
            }

            builder.append_code_unit(character);
        }

        // 3. Remove an attribute by name given name and the DOMStringMap's associated element.
        let data_name = Utf16FlyString::from_utf16_string(&builder.to_utf16_string());
        self.associated_element.remove_attribute(&data_name);

        Ok(())
    }
}

fn is_ascii_upper_alpha(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|character| character.is_ascii_uppercase())
}

fn is_ascii_lower_alpha(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|character| character.is_ascii_lowercase())
}

fn to_ascii_uppercase(code_unit: u16) -> u16 {
    u8::try_from(code_unit).map_or(code_unit, |character| u16::from(character.to_ascii_uppercase()))
}

fn to_ascii_lowercase(code_unit: u16) -> u16 {
    u8::try_from(code_unit).map_or(code_unit, |character| u16::from(character.to_ascii_lowercase()))
}

fn is_data_attribute_name(name: Utf16View<'_>) -> bool {
    // This implements the attribute-name predicate from getting the DOMStringMap's name-value pairs:
    //
    // 2. For each content attribute on the DOMStringMap's associated element whose first five characters are the
    //    string "data-" and whose remaining characters (if any) do not include any ASCII upper alphas, ...
    if !name.starts_with(Utf16View::Ascii(b"data-")) {
        return false;
    }

    let name_after_starting_data = name.substring_view(5, name.length_in_code_units() - 5);
    !name_after_starting_data.code_units().any(is_ascii_upper_alpha)
}

fn property_name_from_data_attribute_name(name: Utf16View<'_>) -> Option<Utf16String> {
    if !is_data_attribute_name(name) {
        return None;
    }

    let name_after_starting_data = name.substring_view(5, name.length_in_code_units() - 5);

    // 2. For each content attribute on the DOMStringMap's associated element whose first five characters are the
    //    string "data-" and whose remaining characters (if any) do not include any ASCII upper alphas, in the order
    //    that those attributes are listed in the element's attribute list, add a name-value pair to list whose name is
    //    the attribute's name with the first five characters removed and whose value is the attribute's value.
    //
    // 3. For each name in list, for each U+002D HYPHEN-MINUS character (-) in the name that is followed by an ASCII
    //    lower alpha, remove the U+002D HYPHEN-MINUS character (-) and replace the character that followed it by the
    //    same character converted to ASCII uppercase.
    let length = name_after_starting_data.length_in_code_units();
    let mut builder = Utf16StringBuilder::new();
    let mut character_index = 0;
    while character_index < length {
        let current_character = name_after_starting_data.code_unit_at(character_index);

        if character_index + 1 < length && current_character == u16::from(b'-') {
            let next_character = name_after_starting_data.code_unit_at(character_index + 1);

            if is_ascii_lower_alpha(next_character) {
                builder.append_code_unit(to_ascii_uppercase(next_character));

                // Skip the next character.
                character_index += 2;

                continue;
            }
        }

        builder.append_code_unit(current_character);
        character_index += 1;
    }

    Some(builder.to_utf16_string())
}

fn data_attribute_name_matches_property_name(attribute_name: Utf16View<'_>, property_name: Utf16View<'_>) -> bool {
    if !is_data_attribute_name(attribute_name) {
        return false;
    }

    // This performs the name transformation from getting the DOMStringMap's name-value pairs, but compares each UTF-16
    // code unit directly instead of constructing the transformed name.
    let name_after_starting_data = attribute_name.substring_view(5, attribute_name.length_in_code_units() - 5);
    let length = name_after_starting_data.length_in_code_units();
    let mut property_name_index = 0;

    let mut character_index = 0;
    while character_index < length {
        let mut current_character = name_after_starting_data.code_unit_at(character_index);

        if character_index + 1 < length && current_character == u16::from(b'-') {
            let next_character = name_after_starting_data.code_unit_at(character_index + 1);

            if is_ascii_lower_alpha(next_character) {
                current_character = to_ascii_uppercase(next_character);
                character_index += 1;
            }
        }

        if property_name_index >= property_name.length_in_code_units() {
            return false;
        }
        if property_name.code_unit_at(property_name_index) != current_character {
            return false;
        }
        property_name_index += 1;
        character_index += 1;
    }

    property_name_index == property_name.length_in_code_units()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_dom_string_map_create(vm: &Vm, element: &LentCppCell<cpp_class::Element>) -> *mut c_void {
    let associated_element = CppElement::from_lent(element);
    cell_into_abi(allocate(vm, |_| DOMStringMap { associated_element }))
}
