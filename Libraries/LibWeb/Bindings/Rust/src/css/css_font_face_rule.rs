/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFontFaceRule, https://drafts.csswg.org/css-fonts/#om-fontface

use super::css_font_face_descriptors::CSSFontFaceDescriptors;
use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

/// AtRuleID::FontFace.
const FONT_FACE_AT_RULE: u8 = 0;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSFontFaceRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSFontFaceRule);

impl Inherits<CSSRule> for CSSFontFaceRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSFontFaceRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/css-fonts/#dom-cssfontfacerule-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFontFaceDescriptors>> {
        Ok(self.rule.declarations_object(cx))
    }

    fn descriptor(&self, name: &str) -> Option<Utf16String> {
        self.rule.native_rule().descriptor_text(FONT_FACE_AT_RULE, name)
    }

    // https://drafts.csswg.org/cssom/#ref-for-cssfontfacerule
    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        // The result of concatenating the following:

        // 1. The string "@font-face {".
        let mut text = RuleText::new("@font-face {");

        // 2. If the font-family descriptor is present:
        if let Some(font_family) = self.descriptor("font-family") {
            // 1. A single SPACE (U+0020), followed by the string "font-family:", followed by a single SPACE (U+0020).
            text.push_str(" font-family: ");

            // 2. The result of performing serialize a string on the rule’s font family name.
            text.push(&font_family);

            // 3. The string ";", i.e., SEMICOLON (U+003B).
            text.push_str(";");
        }

        // 3. If the rule’s associated source list is not empty, follow these substeps:
        if let Some(sources) = self.descriptor("src") {
            // 1. A single SPACE (U+0020), followed by the string "src:", followed by a single SPACE (U+0020).
            text.push_str(" src: ");

            // 2. The result of invoking serialize a comma-separated list on performing serialize a URL or serialize a
            //    LOCAL for each source on the source list.
            text.push(&sources);

            // 3. The string ";", i.e., SEMICOLON (U+003B).
            text.push_str(";");
        }

        // 4. If rule’s associated unicode-range descriptor is present, a single SPACE (U+0020), followed by the string
        //    "unicode-range:", followed by a single SPACE (U+0020), followed by the result of performing serialize a
        //    <'unicode-range'>, followed by the string ";", i.e., SEMICOLON (U+003B).
        self.append_descriptor(&mut text, "unicode-range", "unicode-range");

        // FIXME: 5. If rule’s associated font-variant descriptor is present, a single SPACE (U+0020),
        //        followed by the string "font-variant:", followed by a single SPACE (U+0020),
        //        followed by the result of performing serialize a <'font-variant'>,
        //        followed by the string ";", i.e., SEMICOLON (U+003B).

        // 6. If rule’s associated font-feature-settings descriptor is present, a single SPACE (U+0020),
        //    followed by the string "font-feature-settings:", followed by a single SPACE (U+0020),
        //    followed by the result of performing serialize a <'font-feature-settings'>,
        //    followed by the string ";", i.e., SEMICOLON (U+003B).
        self.append_descriptor(&mut text, "font-feature-settings", "font-feature-settings");

        // 7. If rule’s associated font-stretch descriptor is present, a single SPACE (U+0020),
        //    followed by the string "font-stretch:", followed by a single SPACE (U+0020),
        //    followed by the result of performing serialize a <'font-stretch'>,
        //    followed by the string ";", i.e., SEMICOLON (U+003B).
        // NB: font-stretch is now an alias for font-width, so we use that instead.
        self.append_descriptor(&mut text, "font-stretch", "font-width");

        // 8. If rule’s associated font-weight descriptor is present, a single SPACE (U+0020),
        //     followed by the string "font-weight:", followed by a single SPACE (U+0020),
        //     followed by the result of performing serialize a <'font-weight'>,
        //     followed by the string ";", i.e., SEMICOLON (U+003B).
        self.append_descriptor(&mut text, "font-weight", "font-weight");

        // 9. If rule’s associated font-style descriptor is present, a single SPACE (U+0020),
        //     followed by the string "font-style:", followed by a single SPACE (U+0020),
        //     followed by the result of performing serialize a <'font-style'>,
        //     followed by the string ";", i.e., SEMICOLON (U+003B).
        self.append_descriptor(&mut text, "font-style", "font-style");

        // 10. A single SPACE (U+0020), followed by the string "}", i.e., RIGHT CURLY BRACKET (U+007D).
        text.push_str(" }");
        text.finish()
    }

    fn append_descriptor(&self, text: &mut RuleText, label: &str, descriptor: &str) {
        if let Some(value) = self.descriptor(descriptor) {
            text.push_str(" ");
            text.push_str(label);
            text.push_str(": ");
            text.push(&value);
            text.push_str(";");
        }
    }
}
