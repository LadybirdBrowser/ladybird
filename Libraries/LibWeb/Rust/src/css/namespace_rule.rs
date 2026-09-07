/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;

pub struct NamespaceRuleData {
    pub(crate) prefix: CssString,
    pub(crate) uri: CssString,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<NamespaceRuleData>();
};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_namespace_rule_prefix(rule: &NamespaceRuleData) -> FfiUtf16View {
    let units = rule.prefix.units();
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: units.as_ptr(),
        length: units.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_namespace_rule_uri(rule: &NamespaceRuleData) -> FfiUtf16View {
    let units = rule.uri.units();
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: units.as_ptr(),
        length: units.len(),
    }
}
