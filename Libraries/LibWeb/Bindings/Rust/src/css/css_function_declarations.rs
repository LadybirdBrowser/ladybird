/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSFunctionDeclarations, https://drafts.csswg.org/css-mixins-1/#cssfunctiondeclarations

use super::css_function_descriptors::CSSFunctionDescriptors;
use super::css_rule::CSSRule;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSFunctionDeclarations {
    rule: CSSRule,
}

impl_rule_wrappable!(CSSFunctionDeclarations);

impl Inherits<CSSRule> for CSSFunctionDeclarations {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSFunctionDeclarations {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/css-mixins-1/#dom-cssfunctiondeclarations-style
    pub(crate) fn style(&self, cx: &mut CallContext) -> WebIdlResult<Ref<CSSFunctionDescriptors>> {
        Ok(self.rule.declarations_object(cx))
    }

    pub(crate) fn serialized(&self, vm: &Vm) -> Utf16String {
        // https://drafts.csswg.org/css-mixins-1/#the-function-declarations-interface
        // The CSSFunctionDeclarations rule, like CSSNestedDeclarations, serializes as if its declaration block had been
        // serialized directly.
        self.rule.serialized_declarations(vm)
    }
}
