/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSRuleList, https://drafts.csswg.org/cssom/#the-cssrulelist-interface

use core::ffi::c_void;
use core::ptr::null_mut;
use std::collections::HashMap;
use std::rc::Rc;

use libjs_rust::api::{DeferGc, GcRefCell, MarkedVec, Trace, Visitor, Vm};
use libweb_rust::NativeRuleList;

use super::css_rule::AnyCSSRule;
use super::exports::rule_from_abi;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_style_sheet_import_rule(sheet: *mut c_void, identity: u64) -> *mut c_void;
    fn web_css_style_sheet_disconnect_font_faces(sheet: *mut c_void, rule: *const c_void);
    fn web_css_style_sheet_rules_changed(sheet: *mut c_void);
}

/// The objects of the rules of a list that have one, by the identity of the rule.
#[derive(Default)]
struct RuleObjects(HashMap<u64, AnyCSSRule>);

// SAFETY: Forwards to every object.
unsafe impl Trace for RuleObjects {
    fn trace(&self, visitor: &mut Visitor) {
        for object in self.0.values() {
            object.trace(visitor);
        }
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSRuleList {
    #[gc(untraced)]
    rules: Rc<NativeRuleList>,
    objects: GcRefCell<RuleObjects>,
    owner_rule: GcRefCell<Option<AnyCSSRule>>,
    /// The cell of the CSSStyleSheet.
    parent_style_sheet: GcRefCell<Option<CppCell>>,
    /// The cell of the Document that the @import rules of the list load for.
    document: GcRefCell<Option<CppCell>>,
}

impl Wrappable for CSSRuleList {
    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        self.rules.cssom_external_memory_size()
    }
}

impl CSSRuleList {
    pub(crate) fn new(_: &DeferGc<'_>, rules: Rc<NativeRuleList>, document: Option<CppCell>) -> Self {
        Self {
            rules,
            objects: GcRefCell::new(RuleObjects::default()),
            owner_rule: GcRefCell::new(None),
            parent_style_sheet: GcRefCell::new(None),
            document: GcRefCell::new(document),
        }
    }

    /// The list of the child rules of `owner`.
    pub(crate) fn create_for_rule(vm: &Vm, rules: Rc<NativeRuleList>, owner: AnyCSSRule) -> Ref<Self> {
        let list = allocate(vm, |defer_gc| Self::new(defer_gc, rules, None));
        *list.owner_rule.borrow_mut(vm) = Some(owner);
        *list.parent_style_sheet.borrow_mut(vm) = owner.parent_sheet();
        list
    }

    pub(crate) fn native_rules(&self) -> &Rc<NativeRuleList> {
        &self.rules
    }

    pub(crate) fn native_length(&self) -> usize {
        self.rules.cssom_length()
    }

    pub(crate) fn existing_object(&self, identity: u64) -> Option<AnyCSSRule> {
        self.objects.borrow().0.get(&identity).copied()
    }

    /// The object of the rule at the index, which the first call creates.
    pub(crate) fn rule_at(&self, vm: &Vm, index: usize) -> AnyCSSRule {
        let identity = self.rules.cssom_identity_at(index);
        if let Some(object) = self.existing_object(identity) {
            return object;
        }

        let sheet = self.parent_style_sheet.borrow().clone();
        // SAFETY: The cell is that of a live CSSStyleSheet.
        let import = sheet.as_ref().map_or(null_mut(), |sheet| unsafe {
            web_css_style_sheet_import_rule(sheet.as_ptr(), identity)
        });
        let object = if import.is_null() {
            AnyCSSRule::create(vm, self.rules.cssom_rule_at(index), self.document.borrow().as_ref())
        } else {
            // SAFETY: LibWeb returns the live cell of the CSSImportRule of the rule.
            unsafe { rule_from_abi(import) }
        };
        self.objects.borrow_mut(vm).0.insert(identity, object);
        let owner = *self.owner_rule.borrow();
        if owner.is_some() {
            object.set_parent_rule(vm, owner);
        } else {
            object.set_parent_style_sheet(vm, sheet);
        }
        object
    }

    fn rule_objects<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, AnyCSSRule> {
        let objects = MarkedVec::new(vm);
        for object in self.objects.borrow().0.values() {
            objects.push(*object);
        }
        objects
    }

    pub(crate) fn set_parent_style_sheet(&self, vm: &Vm, sheet: Option<CppCell>) {
        *self.parent_style_sheet.borrow_mut(vm) = sheet.clone();
        for object in self.rule_objects(vm).values().iter() {
            object.set_parent_style_sheet(vm, sheet.clone());
        }
    }

    /// Replaces the rules of the list with those of the other list, whose objects are new.
    pub(crate) fn set_rules(&self, vm: &Vm, rules: &NativeRuleList, document: Option<CppCell>) {
        for object in self.rule_objects(vm).values().iter() {
            object.set_parent_style_sheet(vm, None);
        }
        self.rules.cssom_replace(rules);
        self.objects.borrow_mut(vm).0.clear();
        *self.document.borrow_mut(vm) = document;
    }

    /// What a change of the rules of a sheet means for the sheet: only the list of the sheet itself has no owner rule.
    fn notify_change(&self) {
        if self.owner_rule.borrow().is_some() {
            return;
        }
        if let Some(sheet) = self.parent_style_sheet.borrow().as_ref() {
            // SAFETY: The cell is that of a live CSSStyleSheet.
            unsafe { web_css_style_sheet_rules_changed(sheet.as_ptr()) };
        }
    }

    // https://drafts.csswg.org/cssom/#remove-a-css-rule
    pub(crate) fn remove_a_css_rule_without_validation(&self, vm: &Vm, index: usize) {
        assert!(index < self.native_length(), "the caller validated the index");

        let old_rule = self.rules.cssom_rule_at(index);
        let identity = old_rule.cssom_identity();
        let old_object = self.existing_object(identity);

        // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
        // If a @font-face rule is removed from the document, its corresponding FontFace object is no longer
        // CSS-connected. The connection is not restorable by any means (but adding the @font-face back to the
        // stylesheet will create a brand new FontFace object which is CSS-connected).
        if let Some(sheet) = self.parent_style_sheet.borrow().as_ref() {
            // SAFETY: The cell is that of a live CSSStyleSheet, and the rule is live.
            unsafe { web_css_style_sheet_disconnect_font_faces(sheet.as_ptr(), Rc::as_ptr(&old_rule).cast()) };
        }

        // 5. Remove rule old rule from list at the zero-indexed position index.
        self.rules.cssom_remove(index);
        self.objects.borrow_mut(vm).0.remove(&identity);

        // 6. Set old rule’s parent CSS rule and parent CSS style sheet to null.
        // NB: We set the parent stylesheet to null within set_parent_rule.
        if let Some(old_object) = old_object {
            old_object.set_parent_rule(vm, None);
        }

        self.notify_change();
    }

    // https://drafts.csswg.org/cssom/#dom-cssrulelist-item
    pub(crate) fn item(&self, cx: &mut CallContext, index: u32) -> WebIdlResult<Option<AnyCSSRule>> {
        let vm = cx.vm();
        let index = index as usize;
        if index >= self.native_length() {
            return Ok(None);
        }
        Ok(Some(self.rule_at(vm, index)))
    }

    // https://drafts.csswg.org/cssom/#dom-cssrulelist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.native_length()).expect("a list has fewer than 2^32 rules"))
    }
}
