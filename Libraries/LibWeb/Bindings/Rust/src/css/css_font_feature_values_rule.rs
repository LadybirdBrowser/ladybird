/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFontFeatureValuesRule, https://drafts.csswg.org/css-fonts/#cssfontfeaturevaluesrule

use super::css_font_feature_values_map::CSSFontFeatureValuesMap;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{AnyCSSRule, CSSRule, RuleText};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use core::ffi::c_void;
use libjs_rust::api::GcRefCell;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::FontFeatureValuesRuleKind;
use libweb_rust::NativeRule;
use libweb_rust::cssom::{FONT_FEATURE_VALUES_KINDS, serialize_identifier, serialize_string_value};
use std::rc::Rc;

unsafe extern "C" {
    fn web_css_font_feature_values_rule_clear_caches(rule: *mut c_void);
}

/// https://infra.spec.whatwg.org/#ascii-whitespace
fn is_ascii_whitespace(unit: u16) -> bool {
    matches!(unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20)
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSFontFeatureValuesRule {
    rule: CSSRule,
    /// The CSSFontFeatureValuesMaps of the rule, by the kind of its feature values.
    maps: GcRefCell<[Option<Ref<CSSFontFeatureValuesMap>>; 7]>,
}

impl_rule_wrappable!(CSSFontFeatureValuesRule);

impl Inherits<CSSRule> for CSSFontFeatureValuesRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSFontFeatureValuesRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
            maps: GcRefCell::new(Default::default()),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    /// Drops what the fonts of the document cached from the values of the rule.
    pub(crate) fn clear_caches(&self) {
        // SAFETY: The cell is that of this object, which is live.
        unsafe { web_css_font_feature_values_rule_clear_caches(self.rule.object().cell()) };
    }

    fn map(&self, cx: &CallContext, kind: FontFeatureValuesRuleKind) -> Ref<CSSFontFeatureValuesMap> {
        let index = kind as usize;
        if let Some(map) = self.maps.borrow()[index] {
            return map;
        }
        let AnyCSSRule::CSSFontFeatureValuesRule(rule) = self.rule.object() else {
            unreachable!("this rule is a CSSFontFeatureValuesRule");
        };
        let map = CSSFontFeatureValuesMap::create(cx.vm(), kind, rule);
        self.maps.borrow_mut(cx.vm())[index] = Some(map);
        map
    }

    fn serialized_font_family(&self) -> Utf16String {
        let mut text = RuleText::default();
        for (index, family) in self
            .rule
            .native_rule()
            .font_feature_values_families()
            .iter()
            .enumerate()
        {
            if index != 0 {
                text.push_str(", ");
            }
            let units = family.to_utf16();
            if units.iter().any(|unit| is_ascii_whitespace(*unit)) {
                text.push(&serialize_string_value(family));
            } else {
                text.push(&serialize_identifier(family));
            }
        }
        text.finish()
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-fontfamily
    pub(crate) fn font_family(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.serialized_font_family())
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-fontfamily
    pub(crate) fn set_font_family(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        let units = value.to_utf16();
        let families = units
            .split(|unit| *unit == u16::from(b','))
            .filter(|family| !family.is_empty())
            .map(|family| {
                let start = family
                    .iter()
                    .position(|unit| !is_ascii_whitespace(*unit))
                    .unwrap_or(family.len());
                let end = family
                    .iter()
                    .rposition(|unit| !is_ascii_whitespace(*unit))
                    .map_or(start, |end| end + 1);
                Utf16String::from_utf16(&family[start..end])
            })
            .collect();
        self.rule.native_rule().set_font_feature_values_families(families);
        Ok(())
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-annotation
    pub(crate) fn annotation(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Annotation))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-ornaments
    pub(crate) fn ornaments(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Ornaments))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-stylistic
    pub(crate) fn stylistic(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Stylistic))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-swash
    pub(crate) fn swash(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Swash))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-charactervariant
    pub(crate) fn character_variant(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::CharacterVariant))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-styleset
    pub(crate) fn styleset(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Styleset))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-historicalforms
    pub(crate) fn historical_forms(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFeatureValuesMap>> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::HistoricalForms))
    }

    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        let mut text = RuleText::new("@font-feature-values ");
        text.push(&self.serialized_font_family());
        text.push_str(" {");
        for (kind, at_rule_name) in FONT_FEATURE_VALUES_KINDS {
            let entries = self.rule.native_rule().font_feature_values_entries(kind);
            if entries.is_empty() {
                continue;
            }
            text.push_str("  @");
            text.push_str(at_rule_name);
            text.push_str(" {");
            for (name, values) in entries {
                text.push_str(" ");
                text.push(&serialize_identifier(&name));
                text.push_str(":");
                for value in values {
                    text.push_str(&format!(" {value}"));
                }
                text.push_str(";");
            }
            text.push_str(" }");
        }
        text.push_str(" }");
        text.finish()
    }
}
