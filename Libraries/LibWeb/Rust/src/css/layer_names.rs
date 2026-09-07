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
pub unsafe extern "C" fn rust_layer_names_count(names: *const LayerNames) -> usize {
    unsafe { &*names }.names.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layer_names_at(names: *const LayerNames, index: usize) -> FfiUtf16View {
    let units = unsafe { &*names }.names[index].units();
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: units.as_ptr(),
        length: units.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layer_names_external_memory_size(names: *const LayerNames) -> usize {
    let names = unsafe { &*names };
    names.names.iter().fold(
        size_of::<LayerNames>().saturating_add(size_of_val(&*names.names)),
        |size, name| size.saturating_add(size_of_val(name.units())),
    )
}
