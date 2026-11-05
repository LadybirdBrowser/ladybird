/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StylePropertyMap, https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymap

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace, Vm};
use libweb_rust::typed_om::{RetainedStyleValue, property_is_shorthand};

use super::css_style_value::AnyCSSStyleValue;
use super::style_property_map_read_only::{Property, SERIALIZATION_MODE_NORMAL, StylePropertyMapReadOnly};
use super::typed_om::{comma_separated_style_value_list, parse_values, to_rust_string};
use crate::bindings::conversions::UsvString;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::css_style_value::PerformTypeCheck;

unsafe extern "C" {
    fn web_css_style_map_parse(declaration: *mut c_void, property_id: u16, css_text: usize) -> *const c_void;
    fn web_css_style_map_declared_value(declaration: *mut c_void, name: usize) -> *const c_void;
    fn web_css_style_map_set_value(
        declaration: *mut c_void,
        name: usize,
        value: *const c_void,
        message: *mut usize,
    ) -> u8;
    fn web_css_style_map_remove(declaration: *mut c_void, name: usize, message: *mut usize) -> u8;
    fn web_css_style_map_clear(declaration: *mut c_void, message: *mut usize) -> u8;
}

/// What a change to a declaration reported, as the ChangeResult of C++.
const CHANGE_RESULT_SUCCESS: u8 = 0;
const CHANGE_RESULT_TYPE_ERROR: u8 = 1;

/// Runs a change to a declaration, which writes a message if it fails.
fn change_declaration(change: impl FnOnce(*mut usize) -> u8) -> WebIdlResult<()> {
    let mut message = 0;
    let result = change(&raw mut message);
    if result == CHANGE_RESULT_SUCCESS {
        return Ok(());
    }
    // SAFETY: C++ hands over a reference to a string when a change fails.
    let message = unsafe { Utf16String::from_raw_owned(message) };
    if result == CHANGE_RESULT_TYPE_ERROR {
        return Err(Exception::TypeError(message));
    }
    Err(Exception::Dom("NoModificationAllowedError", message))
}

/// The union (CSSStyleValue or USVString) of CSS/StylePropertyMap.idl.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CSSStyleValueOrUSVString {
    CSSStyleValue(AnyCSSStyleValue),
    USVString(#[gc(untraced)] UsvString),
}

fn any_have_non_matching_associated_property(property: &Utf16String, values: &[CSSStyleValueOrUSVString]) -> bool {
    values.iter().any(|value| {
        let CSSStyleValueOrUSVString::CSSStyleValue(value) = value else {
            return false;
        };
        value
            .style_value()
            .associated_property()
            .is_some_and(|associated_property| associated_property != property)
    })
}

/// The value of the text according to the grammar of the property, parsed for the declaration if one is given.
fn parse(declaration: *mut c_void, property_id: u16, css_text: Utf16String) -> Option<RetainedStyleValue> {
    // SAFETY: The declaration is live or null, and this hands over a reference to the string.
    let value = unsafe { web_css_style_map_parse(declaration, property_id, css_text.into_raw()) };
    // SAFETY: C++ hands over a strong reference, if it parsed the value.
    (!value.is_null()).then(|| unsafe { RetainedStyleValue::from_raw(value) })
}

fn is_custom_property(property: &Property) -> bool {
    libweb_rust::typed_om::is_a_custom_property_name_string(&property.name)
}

fn is_list_valued(property: &Property) -> bool {
    super::typed_om::property_is_list_valued(property.id)
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
fn create_an_internal_representation(
    vm: &Vm,
    property: &Property,
    value: &CSSStyleValueOrUSVString,
) -> WebIdlResult<RetainedStyleValue> {
    let property_name = to_rust_string(&property.name);

    // To create an internal representation, given a string property and a string or CSSStyleValue value:
    let internal_representation = match value {
        CSSStyleValueOrUSVString::CSSStyleValue(css_style_value) => {
            let internal_representation = css_style_value.create_an_internal_representation(
                vm,
                property.id,
                &property.name,
                PerformTypeCheck::Yes,
            )?;
            // INTEROP: Custom properties, including registered ones, only accept CSSUnparsedValue objects and
            //          CSS-wide keywords.
            if is_custom_property(property)
                && !matches!(css_style_value, AnyCSSStyleValue::CSSUnparsedValue(_))
                && !internal_representation.is_css_wide_keyword()
            {
                return Err(Exception::type_error(
                    "Custom properties only accept unparsed values and CSS-wide keywords",
                ));
            }
            internal_representation
        }
        CSSStyleValueOrUSVString::USVString(css_text) => {
            // If value is a USVString,
            //     Parse a CSSStyleValue with property property, cssText value, and parseMultiple set to false, and
            //     return the result.
            let values = parse_values(vm, property.name.clone(), css_text.clone().into_utf16_string())?;
            let result = values.get(0).expect("a parsed value has an iteration");
            // AD-HOC: Result is a CSSStyleValue but we want an internal representation, so... convert it again I guess?
            result.create_an_internal_representation(vm, property.id, &property.name, PerformTypeCheck::Yes)?
        }
    };

    if let CSSStyleValueOrUSVString::CSSStyleValue(css_style_value) = value
        && let Some(numeric_value) = css_style_value.as_numeric()
        && !is_custom_property(property)
    {
        // INTEROP: Shape image thresholds accept typed numbers but not typed percentages.
        if property_name == "shape-image-threshold" && !numeric_value.numeric_type().matches_number(None) {
            return Err(Exception::type_error("Shape image thresholds require a number"));
        }

        // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
        // If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
        // NB: A property's component types do not establish that a numeric value alone matches its whole grammar.
        let serialized_value = internal_representation.serialize(SERIALIZATION_MODE_NORMAL);
        if parse(core::ptr::null_mut(), property.id, serialized_value).is_none() {
            return Err(Exception::type_error(
                "Numeric value does not match the property's grammar",
            ));
        }
    }

    // NB: Step easing keywords are custom identifiers outside the property parser. Store the parsed iteration so
    //     multi-value set() and append() use the easing representation expected by style computation.
    if internal_representation.is_custom_ident()
        && matches!(
            property_name.as_str(),
            "animation-timing-function" | "transition-timing-function"
        )
    {
        let serialized_value = internal_representation.serialize(SERIALIZATION_MODE_NORMAL);
        if let Some(items) = parse(core::ptr::null_mut(), property.id, serialized_value)
            .and_then(|parsed_value| parsed_value.value_list_items())
            && let [item] = items.as_slice()
        {
            return Ok(item.clone());
        }
    }

    // AD-HOC: Our parser represents a text-indent that is just a length-percentage as a text-indent value, and style
    //         computation expects that.
    if property_name == "text-indent" && internal_representation.is_length_percentage() {
        return Ok(internal_representation.into_text_indent());
    }

    Ok(internal_representation)
}

fn normalize_overflow_clip_margin_internal_representation(
    declaration: *mut c_void,
    property: &Property,
    value: RetainedStyleValue,
) -> WebIdlResult<RetainedStyleValue> {
    let property_name = to_rust_string(&property.name);
    if !matches!(
        property_name.as_str(),
        "overflow-clip-margin" | "overflow-clip-margin-block" | "overflow-clip-margin-inline"
    ) {
        return Ok(value);
    }

    if value.is_shorthand()
        || value.is_unresolved()
        || value.is_pending_substitution()
        || value.is_guaranteed_invalid()
        || value.is_css_wide_keyword()
    {
        return Ok(value);
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    // If value is a CSSStyleValue subclass, if value does not match the grammar of a list-valued property
    // iteration of property, throw a TypeError.
    let serialized_value = value.serialize(SERIALIZATION_MODE_NORMAL);
    parse(declaration, property.id, serialized_value.clone()).ok_or_else(|| {
        Exception::type_error(&format!(
            "Property '{property_name}' does not accept the value '{}'",
            to_rust_string(&serialized_value)
        ))
    })
}

/// AD-HOC: To match the behavior of our parser we should store values of list-valued longhands as lists even if there
///         is only one value, except in some rare circumstances.
fn should_wrap_value_in_list(property: &Property, value: &RetainedStyleValue) -> bool {
    if property_is_shorthand(property.id) {
        return false;
    }

    if !is_list_valued(property) {
        return false;
    }

    // Values which are not yet fully resolved should not be wrapped in lists.
    !(value.is_unresolved()
        || value.is_pending_substitution()
        || value.is_guaranteed_invalid()
        || value.is_css_wide_keyword())
}

/// AD-HOC: A single keyword or identifier should be stored the way our parser stores it for the property. That is
///         either the keyword on its own, as for `box-shadow: none`, or a list with the identifier as its only item, as
///         for `container-name: foo`. So we parse it as a value of the property.
fn parsed_identifier_value(
    declaration: *mut c_void,
    property: &Property,
    value: &RetainedStyleValue,
) -> Option<RetainedStyleValue> {
    if is_custom_property(property) {
        return None;
    }
    if !value.is_custom_ident() && !(value.is_keyword() && !value.is_css_wide_keyword()) {
        return None;
    }
    parse(declaration, property.id, value.serialize(SERIALIZATION_MODE_NORMAL))
}

/// AD-HOC: Our parser represents the track sizes of grid-auto-columns and grid-auto-rows as a single track size list,
///         and style computation expects that.
fn grid_track_size_list(property: &Property, values: &[RetainedStyleValue]) -> Option<RetainedStyleValue> {
    let property_name = to_rust_string(&property.name);
    if !matches!(property_name.as_str(), "grid-auto-columns" | "grid-auto-rows") {
        return None;
    }
    let is_not_a_track_size = |value: &RetainedStyleValue| {
        value.is_grid_track_size_list()
            || value.is_unresolved()
            || value.is_pending_substitution()
            || value.is_guaranteed_invalid()
            || value.is_css_wide_keyword()
    };
    if values.iter().any(is_not_a_track_size) {
        return None;
    }
    Some(RetainedStyleValue::grid_track_size_list(values.to_vec()))
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymap
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StylePropertyMap {
    read_only: StylePropertyMapReadOnly,
}

impl Wrappable for StylePropertyMap {}

impl Inherits<StylePropertyMapReadOnly> for StylePropertyMap {
    fn parent(&self) -> &StylePropertyMapReadOnly {
        &self.read_only
    }
}

impl StylePropertyMap {
    pub(crate) fn create(vm: &Vm, declaration: CppCell) -> Ref<Self> {
        allocate(vm, |_| Self {
            read_only: StylePropertyMapReadOnly::new_declared(declaration),
        })
    }

    fn set_property_style_value(&self, property: &Property, value: RetainedStyleValue) -> WebIdlResult<()> {
        change_declaration(|message| {
            // SAFETY: The declaration is live, and this hands over references to the string and the value.
            unsafe {
                web_css_style_map_set_value(
                    self.read_only.declaration(),
                    property.name.clone().into_raw(),
                    value.into_raw(),
                    message,
                )
            }
        })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-set
    pub(crate) fn set(
        &self,
        cx: &mut CallContext,
        property: UsvString,
        values: MarkedVec<'_, CSSStyleValueOrUSVString>,
    ) -> WebIdlResult<()> {
        // The set(property, ...values) method, when called on a StylePropertyMap this, must perform the following
        // steps:

        // 1. If property is not a custom property name string, set property to property ASCII lowercased.
        // 2. If property is not a valid CSS property, throw a TypeError.
        let property_name = property.into_utf16_string();
        let property = Property::from_name(&property_name)?;
        let name = to_rust_string(&property_name);
        let values = values.values();

        // 3. If property is a single-valued property and values has more than one item, throw a TypeError.
        // NB: Custom properties should all be single-valued.
        if (is_custom_property(&property) || !is_list_valued(&property)) && values.len() > 1 {
            return Err(Exception::type_error(&format!(
                "Property '{name}' only accepts a single value"
            )));
        }

        // FIXME: The spec doesn't say how to handle empty `values`, but other browsers throw a TypeError so let's do
        //        that too - see https://github.com/w3c/css-houdini-drafts/issues/1176
        if values.is_empty() {
            return Err(Exception::type_error(&format!(
                "Property '{name}' requires at least one value"
            )));
        }

        // 4. If any of the items in values have a non-null [[associatedProperty]] internal slot, and that slot’s value
        //    is anything other than property, throw a TypeError.
        if any_have_non_matching_associated_property(&property.name, &values) {
            return Err(Exception::type_error(
                "One of the passed CSSStyleValues has a different associated property",
            ));
        }

        // 5. If the size of values is two or more, and one or more of the items are a CSSUnparsedValue or
        //    CSSVariableReferenceValue object, throw a TypeError.
        // NOTE: Having 2+ values implies that you’re setting multiple items of a list-valued property, but the presence
        //       of a var() function in the string-based OM disables all syntax parsing, including splitting into
        //       individual iterations (because there might be more commas inside of the var() value, so you can’t tell
        //       how many items are actually going to show up). This step’s restriction preserves the same semantics in
        //       the Typed OM.
        // FIXME: This is done as part of step 9, because we need to detect if a string value would be an
        //        CSSUnparsedValue or CSSVariableReferenceValue. Spec issue:
        //        https://github.com/w3c/css-houdini-drafts/issues/1157

        // 6. Let props be the value of this’s [[declarations]] internal slot.
        let props = self.read_only.declaration();

        // 8. Let values to set be an empty list.
        let mut values_to_set = Vec::with_capacity(values.len());

        // 9. For each value in values, create an internal representation for property and value, and append the result
        //    to values to set.
        for value in values.iter() {
            // AD-HOC: Step 5 is done here, see above.
            let internal_representation = create_an_internal_representation(cx.vm(), &property, value)?;
            let internal_representation =
                normalize_overflow_clip_margin_internal_representation(props, &property, internal_representation)?;

            if values.len() >= 2 && internal_representation.is_unresolved() {
                return Err(Exception::type_error(
                    "Cannot provide multiple values if one is an CSSUnparsedValue or CSSVariableReferenceValue",
                ));
            }

            values_to_set.push(internal_representation);
        }

        // 7. If props[property] exists, remove it.
        // NB: Removal and replacement happen together in set_property_style_value(), after validation, so observers
        //     receive a single style-attribute update.

        // 10. Set props[property] to values to set.
        // NOTE: The property is deleted then added back so that it gets put at the end of the ordered map, which gives
        //       the expected behavior in the face of shorthand properties.
        if let [value] = values_to_set.as_slice()
            && let Some(parsed_value) = parsed_identifier_value(props, &property, value)
        {
            return self.set_property_style_value(&property, parsed_value);
        }
        if let Some(track_size_list) = grid_track_size_list(&property, &values_to_set) {
            return self.set_property_style_value(&property, track_size_list);
        }
        if let [value] = values_to_set.as_slice()
            && !should_wrap_value_in_list(&property, value)
        {
            return self.set_property_style_value(&property, value.clone());
        }
        self.set_property_style_value(&property, comma_separated_style_value_list(values_to_set))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-append
    pub(crate) fn append(
        &self,
        cx: &mut CallContext,
        property: UsvString,
        values: MarkedVec<'_, CSSStyleValueOrUSVString>,
    ) -> WebIdlResult<()> {
        // The append(property, ...values) method, when called on a StylePropertyMap this, must perform the following
        // steps:

        // 1. If property is not a custom property name string, set property to property ASCII lowercased.
        // 2. If property is not a valid CSS property, throw a TypeError.
        let property_name = property.into_utf16_string();
        let property = Property::from_name(&property_name)?;
        let name = to_rust_string(&property_name);
        let values = values.values();

        // 3. If property is not a list-valued property, throw a TypeError.
        if !is_list_valued(&property) {
            return Err(Exception::type_error(&format!(
                "'{name}' is not a list-valued property"
            )));
        }

        // 4. If any of the items in values have a non-null [[associatedProperty]] internal slot, and that slot’s value
        //    is anything other than property, throw a TypeError.
        if any_have_non_matching_associated_property(&property.name, &values) {
            return Err(Exception::type_error(
                "One of the passed CSSStyleValues has a different associated property",
            ));
        }

        // 5. If any of the items in values are a CSSUnparsedValue or CSSVariableReferenceValue object, throw a
        //    TypeError.
        // NOTE: When a property is set via string-based APIs, the presence of var() in a property prevents the entire
        //       thing from being interpreted. In other words, everything besides the var() is a plain component value,
        //       not a meaningful type. This step’s restriction preserves the same semantics in the Typed OM.
        // FIXME: This is done as part of step 10, because we need to detect if a string value would be an
        //        CSSUnparsedValue or CSSVariableReferenceValue. Spec issue:
        //        https://github.com/w3c/css-houdini-drafts/issues/1157

        // 6. Let props be the value of this’s [[declarations]] internal slot.
        let props = self.read_only.declaration();

        // 7. If props[property] does not exist, set props[property] to an empty list.
        // SAFETY: The declaration is live, and this hands over a reference to the string.
        let existing_value = unsafe { web_css_style_map_declared_value(props, property.name.clone().into_raw()) };
        // SAFETY: C++ hands over a strong reference, if the property has a value.
        let existing_value =
            (!existing_value.is_null()).then(|| unsafe { RetainedStyleValue::from_raw(existing_value) });

        // 8. If props[property] contains a var() reference, throw a TypeError.
        if existing_value.as_ref().is_some_and(RetainedStyleValue::is_unresolved) {
            return Err(Exception::type_error(&format!(
                "Existing value for '{name}' contains var() references."
            )));
        }

        // NB: Appending no values must not replace the declaration or update the style attribute.
        if values.is_empty() {
            return Ok(());
        }

        // 9. Let temp values be an empty list.
        // NB: StyleValues are immutable, so we always create a new one. We add directly to it instead of using "temp
        //     values".
        let mut value_list = Vec::new();
        if let Some(existing_value) = existing_value {
            match existing_value.value_list_items() {
                Some(items) => value_list = items,
                None => value_list.push(existing_value),
            }
        }

        // 10. For each value in values, create an internal representation with property and value, and append the
        //     returned value to temp values.
        for value in values.iter() {
            // AD-HOC: Step 5 is done here, see above.
            let internal_representation = create_an_internal_representation(cx.vm(), &property, value)?;

            if internal_representation.is_unresolved() {
                return Err(Exception::type_error(
                    "Cannot append a CSSUnparsedValue or CSSVariableReferenceValue",
                ));
            }

            value_list.push(internal_representation);
        }

        // 11. Append the entries of temp values to props[property].
        // NB: StyleValues are immutable, so we create a new one instead.
        self.set_property_style_value(&property, comma_separated_style_value_list(value_list))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-delete
    pub(crate) fn delete(&self, _: &mut CallContext, property: UsvString) -> WebIdlResult<()> {
        // The delete(property) method, when called on a StylePropertyMap this, must perform the following steps:

        // 1. If property is not a custom property name string, set property to property ASCII lowercased.
        // 2. If property is not a valid CSS property, throw a TypeError.
        let property = Property::from_name(&property.into_utf16_string())?;

        // 3. If this’s [[declarations]] internal slot contains property, remove it.
        change_declaration(|message| {
            // SAFETY: The declaration is live, and this hands over a reference to the string.
            unsafe { web_css_style_map_remove(self.read_only.declaration(), property.name.into_raw(), message) }
        })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymap-clear
    pub(crate) fn clear(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The clear() method, when called on a StylePropertyMap this, must perform the following steps:

        // 1. Remove all of the declarations in this’s [[declarations]] internal slot.
        change_declaration(|message| {
            // SAFETY: The declaration is live.
            unsafe { web_css_style_map_clear(self.read_only.declaration(), message) }
        })
    }
}
