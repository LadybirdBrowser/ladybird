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
pub extern "C" fn rust_namespace_rule_prefix(rule: &NamespaceRuleData) -> FfiUtf16View<'_> {
    let units = rule.prefix.units();
    FfiUtf16View::from_utf16(units)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_namespace_rule_uri(rule: &NamespaceRuleData) -> FfiUtf16View<'_> {
    let units = rule.uri.units();
    FfiUtf16View::from_utf16(units)
}
