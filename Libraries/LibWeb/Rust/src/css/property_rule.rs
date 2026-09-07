/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::syntax::SyntaxNode;
use crate::css::style_value::StyleValueData;
use std::ffi::c_void;
use std::sync::Arc;

pub struct PropertyRuleData {
    pub(crate) name: CssString,
    pub(crate) syntax_source: CssString,
    pub(crate) syntax: Arc<SyntaxNode>,
    pub(crate) inherits: bool,
    pub(crate) initial_value: Option<Arc<StyleValueData>>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<PropertyRuleData>();
};

#[repr(C)]
pub struct FfiPropertyRuleView {
    pub name: FfiUtf16View,
    pub syntax_source: FfiUtf16View,
    pub syntax: *const c_void,
    pub inherits: bool,
    pub initial_value: *const c_void,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_property_rule_view(rule: &PropertyRuleData) -> FfiPropertyRuleView {
    let name = rule.name.units();
    let syntax_source = rule.syntax_source.units();
    FfiPropertyRuleView {
        name: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: name.as_ptr(),
            length: name.len(),
        },
        syntax_source: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: syntax_source.as_ptr(),
            length: syntax_source.len(),
        },
        syntax: Arc::as_ptr(&rule.syntax).cast(),
        inherits: rule.inherits,
        initial_value: rule
            .initial_value
            .as_ref()
            .map_or(std::ptr::null(), |value| Arc::as_ptr(value).cast()),
    }
}
