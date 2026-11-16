/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! An XML parser.
//!
//! `parse` takes a whole document as UTF-8 and returns a `Document`: a flat tape of events in document order (see
//! `tape`), the table of the names in it, and the errors.

#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

mod chars;
mod dtd;
pub mod dump;
pub mod ffi;
pub mod names;
mod parser;
mod scan;
pub mod tape;
pub mod verify;
pub mod xpath;

pub use parser::Document;
pub use parser::Error;
pub use parser::HtmlEntityResolver;
pub use parser::Options;

/// Parses `source`, which has to be the whole document, as UTF-8.
pub fn parse(source: &[u8], options: Options) -> Document {
    let mut parser = parser::Parser::new(source, options);
    parser.parse();
    parser.finish()
}
