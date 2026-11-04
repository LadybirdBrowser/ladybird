/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSNestedDeclarations, https://drafts.csswg.org/css-nesting-1/#cssnesteddeclarations

use super::css_rule::CSSRule;
use super::css_rule::impl_rule_wrappable;
use super::css_style_properties::CSSStyleProperties;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSNestedDeclarations {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSNestedDeclarations);

impl Inherits<CSSRule> for CSSNestedDeclarations {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSNestedDeclarations {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/css-nesting-1/#dom-cssnesteddeclarations-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSStyleProperties>> {
        Ok(self.rule.declarations_object(cx))
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // NB: There's no proper spec for this yet, only this note:
        //     "The CSSNestedDeclarations rule serializes as if its declaration block had been serialized directly."
        //     - https://drafts.csswg.org/css-nesting-1/#ref-for-cssnesteddeclarations%E2%91%A1
        //     So, we'll do the simple thing and hope it's good.
        self.rule.serialized_declarations(vm)
    }
}
