/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ reads the URLSearchParams objects that it receives from script, which
//! Libraries/LibWeb/DOMURL/URLSearchParams.h declares.

use core::ffi::c_void;

use super::url_search_params::URLSearchParams;
use crate::bindings::interface::{Ref, cell_from_abi};

/// Calls `on_complete` with the serialization of the list of the URLSearchParams whose cell C++ lends, an ASCII
/// string that it borrows for the call, https://url.spec.whatwg.org/#concept-urlencoded-serializer
///
/// # Safety
///
/// `cell` must be the live cell of a URLSearchParams.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_url_search_params_serialize(
    cell: *mut c_void,
    ctx: *mut c_void,
    on_complete: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    // SAFETY: The caller passes a live cell of a URLSearchParams.
    let object: Ref<URLSearchParams> = unsafe { cell_from_abi(cell) };
    let serialization = object.serialize();
    // SAFETY: on_complete is a valid function pointer; ctx is caller-provided.
    unsafe { on_complete(ctx, serialization.as_ptr(), serialization.len()) };
}
