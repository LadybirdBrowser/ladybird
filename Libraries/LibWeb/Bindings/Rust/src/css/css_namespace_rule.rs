/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSNamespaceRule, https://drafts.csswg.org/cssom/#the-cssnamespacerule-interface

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use libweb_rust::cssom::{serialize_identifier, serialize_string_value};
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSNamespaceRule {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSNamespaceRule);

impl Inherits<CSSRule> for CSSNamespaceRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSNamespaceRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/cssom/#dom-cssnamespacerule-namespaceuri
    pub(crate) fn namespace_uri(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().namespace_uri())
    }

    // https://drafts.csswg.org/cssom/#dom-cssnamespacerule-prefix
    pub(crate) fn prefix(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.rule.native_rule().namespace_prefix())
    }

    // https://drafts.csswg.org/cssom/#serialize-a-css-rule
    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        let native_rule = self.rule.native_rule();

        // The literal string "@namespace", followed by a single SPACE (U+0020),
        let mut text = RuleText::new("@namespace ");

        // followed by the serialization as an identifier of the prefix attribute (if any),
        let prefix = native_rule.namespace_prefix();
        if !prefix.is_empty() {
            text.push(&serialize_identifier(&prefix));
            // followed by a single SPACE (U+0020) if there is a prefix,
            text.push_str(" ");
        }

        //  followed by the serialization as URL of the namespaceURI attribute,
        text.push_str("url(");
        text.push(&serialize_string_value(&native_rule.namespace_uri()));
        text.push_str(")");

        // followed the character ";" (U+003B).
        text.push_str(";");
        text.finish()
    }
}
