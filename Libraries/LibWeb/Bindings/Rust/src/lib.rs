/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Rust code of LibWeb that runs on the LibJS runtime: the WebIDL bindings of the interfaces implemented in Rust,
//! and those implementations. The crate is the Rust part of LibWeb's library: it depends on the runtime and on
//! libweb_rust, and the library links its archive, so that the runtime is in a process once and this crate reaches it
//! as any Rust code reaches a crate it depends on.
//!
//! The crate's tests cover the code that does not touch the runtime. A test binary keeps every extern "C" function of
//! every crate it links, so linking the runtime would make it link every C function the runtime imports as well; the
//! modules that use the runtime are left out of the tests instead, as the runtime's own crate leaves out its engine.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../RustPanic.rs"]
mod rust_panic;

pub(crate) mod ak_format;

#[cfg(not(test))]
#[allow(
    dead_code,
    reason = "the generated bindings use only the conversions their interfaces need"
)]
pub(crate) mod bindings;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod canvas_2d;
#[cfg(not(test))]
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod compression;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod domurl;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
pub(crate) mod encoding;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod file_api;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod geometry;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod html;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
pub(crate) mod speech;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
pub(crate) mod streams;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
pub(crate) mod url_pattern;
#[cfg_attr(
    not(web_idl_bindings),
    allow(dead_code, reason = "only the generated bindings use the interfaces")
)]
#[allow(
    clippy::unnecessary_wraps,
    reason = "the bindings call every attribute and operation as one that can throw"
)]
pub(crate) mod web_gl;

// The generators of the build script, whose tests run with those of the crate.
#[cfg(test)]
#[path = "../generate/webgl_constants.rs"]
mod webgl_constants;
#[cfg(test)]
#[path = "../generate/webgl_proxy.rs"]
mod webgl_proxy;
#[cfg(test)]
#[allow(
    dead_code,
    reason = "the tests do not run the writer of the vocabulary, which the model is for as well"
)]
#[path = "../generate/webgl_wire_model.rs"]
mod webgl_wire_model;

// NB: Little in this crate names libweb_rust, which C++ calls into. Linking it keeps its extern "C" functions in the
//     static library.
#[cfg(not(test))]
extern crate libweb_rust;
