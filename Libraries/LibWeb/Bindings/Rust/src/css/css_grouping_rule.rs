/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSGroupingRule, https://drafts.csswg.org/cssom/#the-cssgroupingrule-interface

use core::ffi::c_void;
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::{NativeRule, RuleInsertionResult, RuleRemovalResult};

use super::css_rule::CSSRule;
use super::css_rule_list::CSSRuleList;
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_css_grouping_rule_parse_rule(
        rule: *mut c_void,
        text: usize,
        declarations_failed: *mut bool,
    ) -> *const c_void;
    fn web_css_grouping_rule_inserted(rule: *mut c_void, inserted: *const c_void);
    fn web_css_grouping_rule_removed(rule: *mut c_void, removed: *const c_void);
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSGroupingRule {
    rule: CSSRule,
    /// The list of the child rules, which the first read makes, as most style rules nest none.
    rules: GcRefCell<Option<Ref<CSSRuleList>>>,
}

impl Wrappable for CSSGroupingRule {}

impl Inherits<CSSRule> for CSSGroupingRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

pub(crate) fn insertion_error(result: RuleInsertionResult) -> WebIdlResult<()> {
    match result {
        RuleInsertionResult::Success => Ok(()),
        RuleInsertionResult::IndexSizeError => Err(Exception::index_size_error(Utf16String::from_utf8(
            "CSS rule index out of bounds.",
        ))),
        RuleInsertionResult::HierarchyRequestError => Err(Exception::Dom(
            "HierarchyRequestError",
            Utf16String::from_utf8("Cannot insert rule at specified index."),
        )),
        RuleInsertionResult::InvalidStateError => Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_utf8("Cannot insert @namespace rule into a stylesheet with non-namespace/import rules"),
        )),
    }
}

/// https://drafts.csswg.org/cssom/#remove-a-css-rule
pub(crate) fn validate_rule_removal(result: RuleRemovalResult) -> WebIdlResult<()> {
    match result {
        RuleRemovalResult::Success => Ok(()),
        RuleRemovalResult::IndexSizeError => Err(Exception::index_size_error(Utf16String::from_utf8(
            "CSS rule index out of bounds.",
        ))),
        RuleRemovalResult::InvalidStateError => Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_utf8("Cannot remove @namespace rule from a stylesheet with non-namespace/import rules."),
        )),
    }
}

impl CSSGroupingRule {
    pub(crate) fn new(native_rule: Rc<NativeRule>) -> Self {
        Self {
            rule: CSSRule::new(native_rule),
            rules: GcRefCell::new(None),
        }
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    pub(crate) fn existing_css_rules(&self) -> Option<Ref<CSSRuleList>> {
        *self.rules.borrow()
    }

    pub(crate) fn child_rules(&self, vm: &Vm) -> Ref<CSSRuleList> {
        if let Some(rules) = self.existing_css_rules() {
            return rules;
        }
        let native_rules = self
            .rule
            .native_rule()
            .cssom_children()
            .expect("a grouping rule has child rules");
        let rules = CSSRuleList::create_for_rule(vm, native_rules, self.rule.object());
        *self.rules.borrow_mut(vm) = Some(rules);
        rules
    }

    // https://drafts.csswg.org/cssom/#dom-cssgroupingrule-cssrules
    pub(crate) fn css_rules(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSRuleList>> {
        Ok(self.child_rules(cx.vm()))
    }

    // https://drafts.csswg.org/cssom/#dom-cssgroupingrule-insertrule
    pub(crate) fn insert_rule(&self, cx: &mut CallContext, rule: Utf16String, index: u32) -> WebIdlResult<u32> {
        // The insertRule(rule, index) method must return the result of invoking insert a CSS rule rule into the child
        // CSS rules at index, with the nested flag set.
        let vm = cx.vm();
        let rules = self.child_rules(vm);

        // https://drafts.csswg.org/cssom/#insert-a-css-rule
        // 1. Set length to the number of items in list.
        let length = rules.native_length();

        // 2. If index is greater than length, then throw an IndexSizeError exception.
        if index as usize > length {
            return Err(Exception::index_size_error(Utf16String::from_utf8(
                "CSS rule index out of bounds.",
            )));
        }

        // 3. Set new rule to the results of performing parse a CSS rule on argument rule.
        // 4. If new rule is a syntax error, and nested is set, perform the following substeps:
        let object = self.rule.object();
        let mut declarations_failed = false;
        // SAFETY: The cell is that of this object, the text is handed over, and the out pointer is valid.
        let new_rule =
            unsafe { web_css_grouping_rule_parse_rule(object.cell(), rule.into_raw(), &raw mut declarations_failed) };

        // 5. If new rule is a syntax error, throw a SyntaxError exception.
        if new_rule.is_null() {
            let message = if declarations_failed {
                "Unable to parse CSS declarations block."
            } else {
                "Unable to parse CSS rule."
            };
            return Err(Exception::syntax_error(Utf16String::from_utf8(message)));
        }
        // SAFETY: LibWeb hands over a reference to the rule.
        let new_rule = unsafe { Rc::from_raw(new_rule.cast::<NativeRule>()) };

        insertion_error(
            rules
                .native_rules()
                .cssom_insert_rule(index as usize, Rc::clone(&new_rule), true),
        )?;

        // SAFETY: The cell is that of this object, and the rule is live.
        unsafe { web_css_grouping_rule_inserted(object.cell(), Rc::as_ptr(&new_rule).cast()) };
        Ok(index)
    }

    // https://drafts.csswg.org/cssom/#dom-cssgroupingrule-deleterule
    pub(crate) fn delete_rule(&self, cx: &mut CallContext, index: u32) -> WebIdlResult<()> {
        // The deleteRule(index) method must remove a CSS rule from the child CSS rules at index.
        let vm = cx.vm();
        let rules = self.child_rules(vm);
        let index = index as usize;
        validate_rule_removal(rules.native_rules().cssom_validate_removal(index))?;
        let removed_rule = rules.native_rules().cssom_rule_at(index);
        rules.remove_a_css_rule_without_validation(vm, index);

        // SAFETY: The cell is that of this object, and the rule is live.
        unsafe { web_css_grouping_rule_removed(self.rule.object().cell(), Rc::as_ptr(&removed_rule).cast()) };
        Ok(())
    }
}
