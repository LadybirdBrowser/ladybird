/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;

// Anonymous layer identities and qualified names belong to the document, not
// this immutable, shareable representation of the names written in a rule.
pub struct LayerNames {
    pub(crate) names: Box<[CssString]>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<LayerNames>();
};

#[unsafe(no_mangle)]
pub extern "C" fn rust_layer_names_count(names: &LayerNames) -> usize {
    names.names.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_layer_names_at(names: &LayerNames, index: usize) -> FfiUtf16View<'_> {
    FfiUtf16View::from_utf16(names.names[index].units())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_layer_names_external_memory_size(names: &LayerNames) -> usize {
    names.names.iter().fold(
        size_of::<LayerNames>().saturating_add(size_of_val(&*names.names)),
        |size, name| size.saturating_add(size_of_val(name.units())),
    )
}
