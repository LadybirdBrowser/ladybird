/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSCounterStyleRule, https://drafts.csswg.org/css-counter-styles/#the-csscounterstylerule-interface

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use core::ffi::c_void;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use libweb_rust::cssom::serialize_identifier;
use std::rc::Rc;

unsafe extern "C" {
    fn web_css_counter_style_rule_changed(rule: *mut c_void);
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSCounterStyleRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSCounterStyleRule);

impl Inherits<CSSRule> for CSSCounterStyleRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSCounterStyleRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    fn changed(&self) {
        // SAFETY: The cell is that of this object, which is live.
        unsafe { web_css_counter_style_rule_changed(self.rule.object().cell()) };
    }

    fn set_descriptor(&self, name: &str, value: &Utf16String) {
        if self.rule.native_rule().set_counter_style_descriptor(name, value) {
            self.changed();
        }
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_name())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-name
    pub(crate) fn set_name(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        if self.rule.native_rule().set_counter_style_name(&value) {
            self.changed();
        }
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-system
    pub(crate) fn system(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("system"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-system
    pub(crate) fn set_system(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("system", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-symbols
    pub(crate) fn symbols(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("symbols"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-symbols
    pub(crate) fn set_symbols(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("symbols", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-additivesymbols
    pub(crate) fn additive_symbols(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("additive-symbols"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-additivesymbols
    pub(crate) fn set_additive_symbols(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("additive-symbols", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-negative
    pub(crate) fn negative(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("negative"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-negative
    pub(crate) fn set_negative(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("negative", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-prefix
    pub(crate) fn prefix(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("prefix"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-prefix
    pub(crate) fn set_prefix(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("prefix", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-suffix
    pub(crate) fn suffix(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("suffix"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-suffix
    pub(crate) fn set_suffix(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("suffix", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-range
    pub(crate) fn range(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("range"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-range
    pub(crate) fn set_range(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("range", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-pad
    pub(crate) fn pad(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("pad"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-pad
    pub(crate) fn set_pad(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("pad", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-speakas
    pub(crate) fn speak_as(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("speak-as"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-speakas
    pub(crate) fn set_speak_as(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("speak-as", &value);
        Ok(())
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-fallback
    pub(crate) fn fallback(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().counter_style_descriptor("fallback"))
    }

    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-fallback
    pub(crate) fn set_fallback(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.set_descriptor("fallback", &value);
        Ok(())
    }

    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        let native_rule = self.rule.native_rule();
        let mut text = RuleText::new("@counter-style ");
        text.push(&serialize_identifier(&native_rule.counter_style_name()));
        text.push_str(" {");
        for name in [
            "system",
            "negative",
            "prefix",
            "suffix",
            "range",
            "pad",
            "fallback",
            "symbols",
            "additive-symbols",
            "speak-as",
        ] {
            let value = native_rule.counter_style_descriptor(name);
            if !value.is_empty() {
                text.push_str(" ");
                text.push_str(name);
                text.push_str(": ");
                text.push(&value);
                text.push_str(";");
            }
        }
        text.push_str(" }");
        text.finish()
    }
}
