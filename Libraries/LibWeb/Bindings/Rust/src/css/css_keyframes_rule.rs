/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSKeyframesRule, https://drafts.csswg.org/css-animations/#interface-csskeyframesrule

use super::css_keyframe_rule::CSSKeyframeRule;
use super::css_rule::CSSRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{AnyCSSRule, RuleText};
use super::css_rule_list::CSSRuleList;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use core::ffi::c_void;
use libjs_rust::api::GcRefCell;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use libweb_rust::cssom::{is_valid_animation_name, serialize_identifier, serialize_string_value};
use std::rc::Rc;

unsafe extern "C" {
    fn web_css_keyframes_rule_changed(rule: *mut c_void);
    fn web_css_keyframes_rule_will_rename(rule: *mut c_void);
    fn web_css_keyframes_rule_renamed(rule: *mut c_void);
    fn web_css_parse_keyframe_rule(text: usize) -> *const c_void;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSKeyframesRule {
    rule: CSSRule,
    rules: GcRefCell<Option<Ref<CSSRuleList>>>,
}

impl_rule_wrappable!(CSSKeyframesRule);

impl Inherits<CSSRule> for CSSKeyframesRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSKeyframesRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
            rules: GcRefCell::new(None),
        })
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
            .expect("a @keyframes rule has keyframes");
        let rules = CSSRuleList::create_for_rule(vm, native_rules, self.rule.object());
        *self.rules.borrow_mut(vm) = Some(rules);
        rules
    }

    fn native_length(&self) -> usize {
        self.rule
            .native_rule()
            .cssom_children()
            .expect("a @keyframes rule has keyframes")
            .cssom_length()
    }

    fn keyframe(&self, vm: &Vm, index: usize) -> Option<Ref<CSSKeyframeRule>> {
        if index >= self.native_length() {
            return None;
        }
        match self.child_rules(vm).rule_at(vm, index) {
            AnyCSSRule::CSSKeyframeRule(keyframe) => Some(keyframe),
            _ => None,
        }
    }

    fn keyframes_changed(&self) {
        if self.rule.parent_sheet().is_some() {
            // SAFETY: The cell is that of this object, which is live.
            unsafe { web_css_keyframes_rule_changed(self.rule.object().cell()) };
        }
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframesrule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().keyframes_name())
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframesrule-name
    pub(crate) fn set_name(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        let native_rule = self.rule.native_rule();
        if native_rule.keyframes_name() == value {
            return Ok(());
        }
        let cell = self.rule.object().cell();
        // SAFETY: The cell is that of this object, which is live.
        unsafe { web_css_keyframes_rule_will_rename(cell) };
        native_rule.set_keyframes_name(&value);
        // SAFETY: The cell is that of this object, which is live.
        unsafe { web_css_keyframes_rule_renamed(cell) };
        Ok(())
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframesrule-cssrules
    pub(crate) fn css_rules(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSRuleList>> {
        Ok(self.child_rules(cx.vm()))
    }

    // https://drafts.csswg.org/css-animations/#dom-csskeyframesrule-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.native_length()).expect("a rule has fewer than 2^32 keyframes"))
    }

    // https://drafts.csswg.org/css-animations/#interface-csskeyframesrule
    pub(crate) fn item(&self, cx: &mut CallContext, index: u32) -> WebIdlResult<Ref<CSSKeyframeRule>> {
        Ok(self
            .keyframe(cx.vm(), index as usize)
            .expect("the bindings check the index"))
    }

    // https://drafts.csswg.org/css-animations/#interface-csskeyframesrule-appendrule
    pub(crate) fn append_rule(&self, _: &mut CallContext, rule: Utf16String) -> WebIdlResult<()> {
        // The appendRule method appends the passed CSSKeyframeRule at the end of the keyframes rule.
        // SAFETY: The string is handed over.
        let parsed_rule = unsafe { web_css_parse_keyframe_rule(rule.into_raw()) };
        if parsed_rule.is_null() {
            return Ok(());
        }
        // SAFETY: LibWeb hands over a reference to the rule.
        let parsed_rule = unsafe { Rc::from_raw(parsed_rule.cast::<NativeRule>()) };
        let children = self
            .rule
            .native_rule()
            .cssom_children()
            .expect("a @keyframes rule has keyframes");
        children.cssom_append_rule(parsed_rule);
        self.keyframes_changed();
        Ok(())
    }

    // https://drafts.csswg.org/css-animations-1/#interface-csskeyframesrule-deleterule
    pub(crate) fn delete_rule(&self, cx: &mut CallContext, select: Utf16String) -> WebIdlResult<()> {
        // The deleteRule method deletes the last declared CSSKeyframeRule matching the specified keyframe selector. If
        // no matching rule exists, the method does nothing.
        let native_rule = self.rule.native_rule();
        let Some(index) = native_rule.find_keyframe(&select) else {
            return Ok(());
        };
        if let Some(rules) = self.existing_css_rules() {
            rules.remove_a_css_rule_without_validation(cx.vm(), index);
        } else {
            native_rule
                .cssom_children()
                .expect("a @keyframes rule has keyframes")
                .cssom_remove(index);
        }
        self.keyframes_changed();
        Ok(())
    }

    // https://drafts.csswg.org/css-animations-1/#interface-csskeyframesrule-findrule
    pub(crate) fn find_rule(
        &self,
        cx: &mut CallContext,
        select: Utf16String,
    ) -> WebIdlResult<Option<Ref<CSSKeyframeRule>>> {
        // The findRule returns the last declared CSSKeyframeRule matching the specified keyframe selector. If no
        // matching rule exists, the method does nothing.
        let index = self.rule.native_rule().find_keyframe(&select);
        Ok(index.and_then(|index| self.keyframe(cx.vm(), index)))
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        let mut text = RuleText::new("@keyframes ");

        // https://drafts.csswg.org/css-animations-1/#keyframes
        // When serialized, the value is serialized as an <ident> unless it’s a disallowed keyword, in which case
        // it’s serialized as a <string>.
        let name = self.rule.native_rule().keyframes_name();
        if is_valid_animation_name(&name) {
            text.push(&serialize_identifier(&name));
        } else {
            text.push(&serialize_string_value(&name));
        }

        text.push_str(" { ");
        let rules = self.child_rules(vm);
        for index in 0..rules.native_length() {
            text.push(&rules.rule_at(vm, index).serialized(vm));
            text.push_str(" ");
        }
        text.push_str("}");
        text.finish()
    }
}
