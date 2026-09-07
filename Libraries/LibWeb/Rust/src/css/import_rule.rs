/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::query_parser::FfiQueryHandle;
use crate::css::scope_selectors::ScopeSelectors;
use crate::css::style_value::StyleValueData;
use std::ffi::c_void;
use std::sync::Arc;

pub struct ImportRuleData {
    pub(crate) url: Arc<StyleValueData>,
    pub(crate) layer: Option<CssString>,
    pub(crate) supports: Option<Arc<FfiQueryHandle>>,
    pub(crate) scope: Option<Arc<ScopeSelectors>>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ImportRuleData>();
};

#[repr(C)]
pub struct FfiImportRuleView {
    pub url: *const c_void,
    pub has_layer: bool,
    pub layer: FfiUtf16View,
    pub supports: *const FfiQueryHandle,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_import_rule_retain(rule: *const ImportRuleData) -> *const ImportRuleData {
    if !rule.is_null() {
        unsafe { Arc::increment_strong_count(rule) };
    }
    rule
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_import_rule_release(rule: *const ImportRuleData) {
    if !rule.is_null() {
        unsafe { Arc::decrement_strong_count(rule) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_import_rule_view(rule: &ImportRuleData) -> FfiImportRuleView {
    let layer = rule.layer.as_ref().map_or(&[][..], CssString::units);
    FfiImportRuleView {
        url: Arc::as_ptr(&rule.url).cast(),
        has_layer: rule.layer.is_some(),
        layer: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: layer.as_ptr(),
            length: layer.len(),
        },
        supports: rule.supports.as_ref().map_or(std::ptr::null(), Arc::as_ptr),
    }
}
