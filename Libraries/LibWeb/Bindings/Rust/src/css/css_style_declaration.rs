/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSStyleDeclaration, https://drafts.csswg.org/cssom/#the-cssstyledeclaration-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use super::css_rule::AnyCSSRule;
use super::exports::rule_from_abi;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Interface, Ref, Wrappable, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_css_declaration_set_owner(declaration: *mut c_void, interface_name: u16, owner: *mut c_void);
    fn web_css_declaration_get_by_name(declaration: *mut c_void, name: *const u8, name_length: usize) -> usize;
    fn web_css_declaration_set_by_name(
        declaration: *mut c_void,
        name: *const u8,
        name_length: usize,
        value: usize,
        error_message: *mut usize,
    ) -> bool;
    fn web_css_declaration_css_text(declaration: *mut c_void) -> usize;
    fn web_css_declaration_set_css_text(declaration: *mut c_void, text: usize, error_message: *mut usize) -> bool;
    fn web_css_declaration_length(declaration: *mut c_void) -> usize;
    fn web_css_declaration_item(declaration: *mut c_void, index: usize) -> usize;
    fn web_css_declaration_get_property_value(declaration: *mut c_void, property: usize) -> usize;
    fn web_css_declaration_get_property_priority(declaration: *mut c_void, property: usize) -> usize;
    fn web_css_declaration_set_property(
        declaration: *mut c_void,
        property: usize,
        value: usize,
        priority: usize,
        error_message: *mut usize,
    ) -> bool;
    fn web_css_declaration_remove_property(
        declaration: *mut c_void,
        property: usize,
        removed_value: *mut usize,
        error_message: *mut usize,
    ) -> bool;
    fn web_css_declaration_parent_rule(declaration: *mut c_void) -> *mut c_void;
}

/// What every interface that inherits from CSSStyleDeclaration has: the Web::CSS::CSSStyleDeclaration that holds the
/// declarations.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSStyleDeclaration {
    declaration: CppCell,
}

impl Wrappable for CSSStyleDeclaration {}

/// What a step that LibWeb runs says: `Ok` if it succeeded, and the message of the exception otherwise.
fn result_of_step(succeeded: bool, error_message: usize) -> WebIdlResult<()> {
    if succeeded {
        return Ok(());
    }
    // SAFETY: LibWeb hands over the message of the exception.
    let message = unsafe { Utf16String::from_raw_owned(error_message) };
    Err(Exception::Dom("NoModificationAllowedError", message))
}

impl CSSStyleDeclaration {
    pub(crate) fn new(declaration: CppCell) -> Self {
        Self { declaration }
    }

    /// The C++ object that the wrappers of the object hold as their wrappable, which the wrapper caches of LibWeb's C++
    /// bindings look for.
    pub(crate) fn as_cpp_parent(&self) -> Option<NonNull<c_void>> {
        NonNull::new(self.declaration.as_ptr())
    }

    pub(crate) fn set_owner<T: Interface>(&self, owner: Ref<T>) {
        // SAFETY: The cells are live, and the owner keeps the declaration alive.
        unsafe { web_css_declaration_set_owner(self.declaration.as_ptr(), T::INTERFACE_NAME, cell_into_abi(owner)) };
    }

    /// The value of the property or descriptor, whose name is one the bindings know.
    pub(crate) fn get(&self, name: &'static str) -> Utf16String {
        // SAFETY: The cell is live, the name is lent for the call, and LibWeb hands over the string.
        unsafe {
            Utf16String::from_raw_owned(web_css_declaration_get_by_name(
                self.declaration.as_ptr(),
                name.as_ptr(),
                name.len(),
            ))
        }
    }

    /// Sets the property or descriptor, whose name is one the bindings know.
    pub(crate) fn set(&self, name: &'static str, value: Utf16String) -> WebIdlResult<()> {
        let mut error_message = 0;
        // SAFETY: The cell is live, the name is lent for the call, and the value is handed over.
        let succeeded = unsafe {
            web_css_declaration_set_by_name(
                self.declaration.as_ptr(),
                name.as_ptr(),
                name.len(),
                value.into_raw(),
                &raw mut error_message,
            )
        };
        result_of_step(succeeded, error_message)
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
    pub(crate) fn css_text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // SAFETY: The cell is live, and LibWeb hands over the string.
        Ok(unsafe { Utf16String::from_raw_owned(web_css_declaration_css_text(self.declaration.as_ptr())) })
    }

    pub(crate) fn set_css_text(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        let mut error_message = 0;
        // SAFETY: The cell is live, and the value is handed over.
        let succeeded = unsafe {
            web_css_declaration_set_css_text(self.declaration.as_ptr(), value.into_raw(), &raw mut error_message)
        };
        result_of_step(succeeded, error_message)
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // SAFETY: The cell is live.
        let length = unsafe { web_css_declaration_length(self.declaration.as_ptr()) };
        Ok(u32::try_from(length).expect("a declaration block has fewer than 2^32 declarations"))
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Utf16String> {
        // SAFETY: The cell is live, and LibWeb hands over the string.
        Ok(unsafe { Utf16String::from_raw_owned(web_css_declaration_item(self.declaration.as_ptr(), index as usize)) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
    pub(crate) fn get_property_value(&self, _: &mut CallContext, property: Utf16String) -> WebIdlResult<Utf16String> {
        // SAFETY: The cell is live, the name is handed over, and LibWeb hands over the string.
        Ok(unsafe {
            Utf16String::from_raw_owned(web_css_declaration_get_property_value(
                self.declaration.as_ptr(),
                property.into_raw(),
            ))
        })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
    pub(crate) fn get_property_priority(
        &self,
        _: &mut CallContext,
        property: Utf16String,
    ) -> WebIdlResult<Utf16String> {
        // SAFETY: The cell is live, the name is handed over, and LibWeb hands over the string.
        Ok(unsafe {
            Utf16String::from_raw_owned(web_css_declaration_get_property_priority(
                self.declaration.as_ptr(),
                property.into_raw(),
            ))
        })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
    pub(crate) fn set_property(
        &self,
        _: &mut CallContext,
        property: Utf16String,
        value: Utf16String,
        priority: Utf16String,
    ) -> WebIdlResult<()> {
        let mut error_message = 0;
        // SAFETY: The cell is live, and the strings are handed over.
        let succeeded = unsafe {
            web_css_declaration_set_property(
                self.declaration.as_ptr(),
                property.into_raw(),
                value.into_raw(),
                priority.into_raw(),
                &raw mut error_message,
            )
        };
        result_of_step(succeeded, error_message)
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
    pub(crate) fn remove_property(&self, _: &mut CallContext, property: Utf16String) -> WebIdlResult<Utf16String> {
        let mut removed_value = 0;
        let mut error_message = 0;
        // SAFETY: The cell is live, and the name is handed over.
        let succeeded = unsafe {
            web_css_declaration_remove_property(
                self.declaration.as_ptr(),
                property.into_raw(),
                &raw mut removed_value,
                &raw mut error_message,
            )
        };
        result_of_step(succeeded, error_message)?;
        // SAFETY: LibWeb hands over the value.
        Ok(unsafe { Utf16String::from_raw_owned(removed_value) })
    }

    // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-parentrule
    pub(crate) fn parent_rule(&self, _: &mut CallContext) -> WebIdlResult<Option<AnyCSSRule>> {
        // SAFETY: The cell is live.
        let rule = unsafe { web_css_declaration_parent_rule(self.declaration.as_ptr()) };
        if rule.is_null() {
            return Ok(None);
        }
        // SAFETY: LibWeb returns the live cell of a rule.
        Ok(Some(unsafe { rule_from_abi(rule) }))
    }
}
