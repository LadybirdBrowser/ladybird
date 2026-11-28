/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The derive whose helper attributes the lints read, as `#[gc(untraced)]` on fields. It derives nothing; the tests
//! implement Trace themselves.

extern crate proc_macro;

use proc_macro::TokenStream;

#[proc_macro_derive(Trace, attributes(gc))]
pub fn derive_trace(_: TokenStream) -> TokenStream {
    TokenStream::new()
}
