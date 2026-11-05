/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS namespace, https://www.w3.org/TR/cssom-1/#namespacedef-css

use core::ffi::c_void;

use ak::Utf16String;
use libweb_rust::typed_om::{self, RegisteredSyntax, RetainedStyleValue};

use super::css_rule::RuleText;
use super::css_unit_value::CSSUnitValue;
use super::highlight_registry::HighlightRegistry;
use crate::bindings::interface::{Ref, cell_from_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_css_realm_document(realm: *mut c_void) -> *mut c_void;
    fn web_css_namespace_highlight_registry(document: *mut c_void) -> *mut c_void;
    fn web_css_namespace_is_registered(document: *mut c_void, name: usize) -> bool;
    fn web_css_namespace_parse_initial_value_descriptor(document: *mut c_void, value: usize) -> *const c_void;
    fn web_css_namespace_parse_with_a_syntax(
        document: *mut c_void,
        value: usize,
        syntax: *const c_void,
    ) -> *const c_void;
    fn web_css_namespace_register(
        document: *mut c_void,
        name: usize,
        syntax: *const c_void,
        inherits: bool,
        initial_value: *const c_void,
    );
}

// https://drafts.css-houdini.org/css-properties-values-api-1/#dictdef-propertydefinition
pub(crate) struct PropertyDefinition {
    pub(crate) name: Utf16String,
    pub(crate) syntax: Utf16String,
    pub(crate) inherits: bool,
    pub(crate) initial_value: Option<Utf16String>,
}

fn syntax_error(message: &str) -> Exception {
    Exception::syntax_error(Utf16String::from_utf8(message))
}

fn optional_style_value(pointer: *const c_void) -> Option<RetainedStyleValue> {
    // SAFETY: C++ hands over a strong reference to a style value.
    (!pointer.is_null()).then(|| unsafe { RetainedStyleValue::from_raw(pointer) })
}

// https://www.w3.org/TR/cssom-1/#namespacedef-css
#[allow(clippy::upper_case_acronyms)]
pub(crate) struct CSS;

impl CSS {
    fn document(cx: &CallContext) -> *mut c_void {
        // SAFETY: The realm is live.
        unsafe { web_css_realm_document(cx.realm().as_ptr().cast()) }
    }

    // https://www.w3.org/TR/cssom-1/#dom-css-escape
    pub(crate) fn escape(_: &mut CallContext, ident: Utf16String) -> WebIdlResult<Utf16String> {
        // The escape(ident) operation must return the result of invoking serialize an identifier of ident.
        Ok(typed_om::serialize_an_identifier(&ident))
    }

    // https://www.w3.org/TR/css-conditional-3/#dom-css-supports
    pub(crate) fn supports_property(
        _: &mut CallContext,
        property: Utf16String,
        value: Utf16String,
    ) -> WebIdlResult<bool> {
        // 1. If property is an ASCII case-insensitive match for any defined CSS property that the UA supports, or is a
        //    custom property name string, and value successfully parses according to that property’s grammar, return
        //    true.
        // 2. Otherwise, return false.
        let Some((_, property_id)) = typed_om::property_from_name(&property) else {
            return Ok(false);
        };
        Ok(typed_om::parse_value_for_property(property_id, &value).is_some())
    }

    // https://www.w3.org/TR/css-conditional-3/#dom-css-supports
    pub(crate) fn supports_condition(_: &mut CallContext, condition_text: Utf16String) -> WebIdlResult<bool> {
        // 1. If conditionText, parsed and evaluated as a <supports-condition>, would return true, return true.
        if typed_om::supports_condition_matches(&condition_text) {
            return Ok(true);
        }

        // 2. Otherwise, If conditionText, wrapped in parentheses and then parsed and evaluated as a
        //    <supports-condition>, would return true, return true.
        let mut wrapped_condition_text = RuleText::new("(");
        wrapped_condition_text.push(&condition_text);
        wrapped_condition_text.push_str(")");
        if typed_om::supports_condition_matches(&wrapped_condition_text.finish()) {
            return Ok(true);
        }

        // 3. Otherwise, return false.
        Ok(false)
    }

    // https://www.w3.org/TR/css-properties-values-api-1/#the-registerproperty-function
    pub(crate) fn register_property(cx: &mut CallContext, definition: PropertyDefinition) -> WebIdlResult<()> {
        // 1. Let property set be the value of the current global object’s associated Document’s
        //    [[registeredPropertySet]] slot.
        let document = Self::document(cx);

        // 2. If name is not a custom property name string, throw a SyntaxError and exit this algorithm.
        if !typed_om::is_a_custom_property_name_string(&definition.name) {
            return Err(syntax_error("Invalid property name"));
        }

        // If property set already contains an entry with name as its property name (compared codepoint-wise),
        // throw an InvalidModificationError and exit this algorithm.
        // SAFETY: The document is live, and this hands over a reference to the string.
        if unsafe { web_css_namespace_is_registered(document, definition.name.clone().into_raw()) } {
            return Err(Exception::Dom(
                "InvalidModificationError",
                Utf16String::from_ascii(b"Property already registered"),
            ));
        }

        // 3. Attempt to consume a syntax definition from syntax. If it returns failure, throw a SyntaxError.
        //    Otherwise, let syntax definition be the returned syntax definition.
        let Some(syntax_definition) = RegisteredSyntax::parse(&definition.syntax) else {
            return Err(syntax_error("Invalid syntax definition"));
        };

        // 4. If syntax definition is the universal syntax definition, and initialValue is not present,
        let parsed_initial_value = match (syntax_definition.is_universal(), definition.initial_value) {
            // let parsed initial value be empty.
            // This must be treated identically to the "default" initial value of custom properties, as defined in
            // [css-variables].
            (true, None) => None,
            // Otherwise, if syntax definition is the universal syntax definition,
            // parse initialValue as a <declaration-value>
            (true, Some(initial_value)) => {
                // AD-HOC: Parse this as the equivalent descriptor value in order to disallow arbitrary substitution
                //         functions.
                // SAFETY: The document is live, and this hands over a reference to the string.
                let parsed = optional_style_value(unsafe {
                    web_css_namespace_parse_initial_value_descriptor(document, initial_value.into_raw())
                });

                // If this fails, throw a SyntaxError and exit this algorithm.
                // Otherwise, let parsed initial value be the parsed result.
                Some(parsed.ok_or_else(|| syntax_error("Invalid initial value"))?)
            }
            // Otherwise, if initialValue is not present, throw a SyntaxError and exit this algorithm.
            (false, None) => {
                return Err(syntax_error("Initial value must be provided for non-universal syntax"));
            }
            // Otherwise, parse initialValue according to syntax definition.
            (false, Some(initial_value)) => {
                // NB: We don't need to worry about explicitly rejecting arbitrary substitution functions here since all
                //     supported syntaxes implicitly reject them
                // SAFETY: The document and the syntax are live, and this hands over a reference to the string.
                let parsed = optional_style_value(unsafe {
                    web_css_namespace_parse_with_a_syntax(
                        document,
                        initial_value.into_raw(),
                        syntax_definition.as_ptr(),
                    )
                });

                // If this fails, throw a SyntaxError and exit this algorithm.
                // Otherwise, let parsed initial value be the parsed result.
                let parsed = parsed
                    .filter(|value| !value.is_guaranteed_invalid())
                    .ok_or_else(|| syntax_error("Invalid initial value"))?;

                // If parsed initial value is not computationally independent, throw a SyntaxError and exit this
                // algorithm.
                if !parsed.is_computationally_independent() {
                    return Err(syntax_error("Initial value must be computationally independent"));
                }
                Some(parsed)
            }
        };

        // 5. Set inherit flag to the value of inherits.
        // 6. Let registered property be a struct with a property name of name, a syntax of syntax definition,
        //    an initial value of parsed initial value, and an inherit flag of inherit flag.
        //    Append registered property to property set.
        // SAFETY: The document is live, and this hands over references to the string, the syntax and the value.
        unsafe {
            web_css_namespace_register(
                document,
                definition.name.into_raw(),
                syntax_definition.into_raw(),
                definition.inherits,
                parsed_initial_value.map_or(core::ptr::null(), RetainedStyleValue::into_raw),
            );
        }
        Ok(())
    }

    // https://drafts.csswg.org/css-highlight-api-1/#dom-css-highlights
    pub(crate) fn highlights(cx: &mut CallContext) -> WebIdlResult<Ref<HighlightRegistry>> {
        // The highlight registry is accessed via the highlights attribute of the CSS namespace, and represents all the
        // custom highlights registered for the current global object’s associated Document.
        // SAFETY: The document is live, and it returns its live registry.
        Ok(unsafe { cell_from_abi::<HighlightRegistry>(web_css_namespace_highlight_registry(Self::document(cx))) })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory
    fn numeric_factory(cx: &CallContext, value: f64, unit: &str) -> WebIdlResult<Ref<CSSUnitValue>> {
        // All of the above methods must, when called with a double value, return a new CSSUnitValue whose value
        // internal slot is set to value and whose unit internal slot is set to the name of the method as defined here.
        Ok(CSSUnitValue::create(cx.vm(), value, unit))
    }
}

// NB: The numeric factories of the units in CSS/Units.json.
include!(concat!(env!("OUT_DIR"), "/css_numeric_factories.rs"));
