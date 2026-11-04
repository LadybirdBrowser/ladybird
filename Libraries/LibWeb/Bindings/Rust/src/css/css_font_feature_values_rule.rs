/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFontFeatureValuesRule, https://drafts.csswg.org/css-fonts/#cssfontfeaturevaluesrule

use super::css_rule::CSSRule;
use super::css_rule::impl_rule_wrappable;
use super::css_rule::{RuleText, wrap_cpp_cell};
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
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
    fn web_css_font_feature_values_map_create(rule: *mut c_void, kind: u8) -> *mut c_void;
}

/// https://infra.spec.whatwg.org/#ascii-whitespace
fn is_ascii_whitespace(unit: u16) -> bool {
    matches!(unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20)
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSFontFeatureValuesRule {
    rule: CSSRule,
    /// The cells of the CSSFontFeatureValuesMaps of the rule, by the kind of its feature values.
    maps: GcRefCell<[Option<CppCell>; 7]>,
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

    fn map(&self, cx: &CallContext, kind: FontFeatureValuesRuleKind) -> CppInterface {
        let index = kind as usize;
        if let Some(map) = &self.maps.borrow()[index] {
            return wrap_cpp_cell(cx, map);
        }
        // SAFETY: The cell is that of this object, which is live.
        let map = unsafe { web_css_font_feature_values_map_create(self.rule.object().cell(), kind as u8) };
        // SAFETY: LibWeb returns a live cell.
        let map = unsafe { CppCell::from_raw(map) };
        let result = wrap_cpp_cell(cx, &map);
        self.maps.borrow_mut(cx.vm())[index] = Some(map);
        result
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
                text.push(&serialize_string_value(&units));
            } else {
                text.push(&serialize_identifier(&units));
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
                family[start..end].to_vec()
            })
            .collect();
        self.rule.native_rule().set_font_feature_values_families(families);
        Ok(())
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-annotation
    pub(crate) fn annotation(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Annotation))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-ornaments
    pub(crate) fn ornaments(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Ornaments))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-stylistic
    pub(crate) fn stylistic(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Stylistic))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-swash
    pub(crate) fn swash(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Swash))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-charactervariant
    pub(crate) fn character_variant(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::CharacterVariant))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-styleset
    pub(crate) fn styleset(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.map(cx, FontFeatureValuesRuleKind::Styleset))
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfeaturevaluesrule-historicalforms
    pub(crate) fn historical_forms(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
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
