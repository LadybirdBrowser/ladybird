/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::layout::cell::Gc;
use crate::runtime::module::Module;
use crate::utf16::Utf16View;
use libjs_rust::ast;

// https://tc39.es/ecma262/#importattribute-record
#[derive(Clone, PartialEq, Eq, Trace)]
pub struct ImportAttribute {
    pub key: Utf16String,
    pub value: Utf16String,
}

impl ImportAttribute {
    pub fn new(key: Utf16String, value: Utf16String) -> Self {
        Self { key, value }
    }
}

// https://tc39.es/ecma262/#loadedmodulerequest-record
#[derive(Clone, Trace)]
pub struct LoadedModuleRequest {
    pub specifier: Utf16String,           // [[Specifier]]
    pub attributes: Vec<ImportAttribute>, // [[Attributes]]
    pub module: Gc<Module>,               // [[Module]]
}

// https://tc39.es/ecma262/#modulerequest-record
#[derive(Clone, PartialEq, Eq, Default, Trace)]
pub struct ModuleRequest {
    pub module_specifier: Utf16FlyString, // [[Specifier]]
    pub attributes: Vec<ImportAttribute>, // [[Attributes]]
}

impl ModuleRequest {
    pub fn new(specifier: Utf16FlyString) -> Self {
        Self {
            module_specifier: specifier,
            attributes: Vec::new(),
        }
    }

    pub fn new_with_attributes(specifier: Utf16FlyString, mut attributes: Vec<ImportAttribute>) -> Self {
        // 16.2.2.4 Static Semantics: WithClauseToAttributes, https://tc39.es/ecma262/#sec-withclausetoattributes
        // 2. Sort attributes according to the lexicographic order of their [[Key]] field, treating the value of each such
        //    field as a sequence of UTF-16 code unit values.
        attributes.sort_by(|lhs, rhs| {
            let (lhs, rhs) = (Utf16View::of_string(&lhs.key), Utf16View::of_string(&rhs.key));
            if lhs.is_code_unit_less_than(rhs) {
                core::cmp::Ordering::Less
            } else if rhs.is_code_unit_less_than(lhs) {
                core::cmp::Ordering::Greater
            } else {
                core::cmp::Ordering::Equal
            }
        });
        Self {
            module_specifier: specifier,
            attributes,
        }
    }

    pub fn add_attribute(&mut self, key: Utf16String, value: Utf16String) {
        self.attributes.push(ImportAttribute::new(key, value));
    }

    /// The ModuleRequest a module request of the frontend stands for: the attributes of a request that has any are
    /// sorted.
    pub fn from_frontend(request: &ast::ModuleRequest) -> Self {
        let specifier = Utf16FlyString::from_utf16(&request.module_specifier);
        let attributes: Vec<ImportAttribute> = request
            .attributes
            .iter()
            .map(|attribute| {
                ImportAttribute::new(
                    Utf16String::from_utf16(&attribute.key),
                    Utf16String::from_utf16(&attribute.value),
                )
            })
            .collect();
        if attributes.is_empty() {
            return Self::new(specifier);
        }
        Self::new_with_attributes(specifier, attributes)
    }

    /// The [[ModuleRequest]] of an import or export entry the frontend describes. There is none for an empty specifier,
    /// as if the entry had no module request.
    pub fn of_entry_from_frontend(request: Option<&ast::ModuleRequest>) -> Option<Self> {
        let request = request?;
        if request.module_specifier.is_empty() {
            return None;
        }
        Some(Self::from_frontend(request))
    }
}

/// What ModuleRequestsEqual compares of a ModuleRequest or a LoadedModuleRequest.
pub trait ModuleRequestLike {
    fn specifier_code_units(&self) -> std::borrow::Cow<'_, [u16]>;
    fn import_attributes(&self) -> &[ImportAttribute];
}

impl ModuleRequestLike for ModuleRequest {
    fn specifier_code_units(&self) -> std::borrow::Cow<'_, [u16]> {
        self.module_specifier.to_utf16()
    }

    fn import_attributes(&self) -> &[ImportAttribute] {
        &self.attributes
    }
}

impl ModuleRequestLike for LoadedModuleRequest {
    fn specifier_code_units(&self) -> std::borrow::Cow<'_, [u16]> {
        self.specifier.to_utf16()
    }

    fn import_attributes(&self) -> &[ImportAttribute] {
        &self.attributes
    }
}

// 16.2.1.3.1 ModuleRequestsEqual ( left, right ), https://tc39.es/ecma262/#sec-modulerequestsequal
pub fn module_requests_equal(left: &impl ModuleRequestLike, right: &impl ModuleRequestLike) -> bool {
    // 1. If left.[[Specifier]] is not right.[[Specifier]], return false.
    if left.specifier_code_units() != right.specifier_code_units() {
        return false;
    }

    // 2. Let leftAttrs be left.[[Attributes]].
    // 3. Let rightAttrs be right.[[Attributes]].
    let left_attrs = left.import_attributes();
    let right_attrs = right.import_attributes();

    // 4. Let leftAttrsCount be the number of elements in leftAttrs.
    // 5. Let rightAttrsCount be the number of elements in rightAttrs.
    // 6. If leftAttrsCount ≠ rightAttrsCount, return false.
    if left_attrs.len() != right_attrs.len() {
        return false;
    }

    // 7. For each ImportAttribute Record l of leftAttrs
    //    a. If rightAttrs does not contain an ImportAttribute Record r such that l.[[Key]] is r.[[Key]] and l.[[Value]] is r.[[Value]], return false.
    // 8. Return true.
    left_attrs
        .iter()
        .all(|l| right_attrs.iter().any(|r| l.key == r.key && l.value == r.value))
}
