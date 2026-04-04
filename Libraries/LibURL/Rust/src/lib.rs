/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

mod ffi;
pub mod pattern;
mod textcodec;
pub mod url;

pub use url::BasicParseOptions;
pub use url::Host;
pub use url::State;
pub use url::Url;
pub use url::basic_parse;
pub use url::basic_parse_into;
