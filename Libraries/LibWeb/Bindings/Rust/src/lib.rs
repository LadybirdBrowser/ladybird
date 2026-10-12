/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Rust code of LibWeb that runs on the LibJS runtime. The crate is the Rust part of LibWeb's library: it depends
//! on the runtime and on libweb_rust, and the library links its archive, so that the runtime is in a process once and
//! this crate reaches it as any Rust code reaches a crate it depends on.
//!
//! The crate's tests cover the code that does not touch the runtime. A test binary keeps every extern "C" function of
//! every crate it links, so linking the runtime would make it link every C function the runtime imports as well; the
//! runtime and libweb_rust are left out of the tests instead, as the runtime's own crate leaves out its engine.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../RustPanic.rs"]
mod rust_panic;

// NB: Only C++ calls into the runtime and into libweb_rust so far, so nothing in this crate names them. Linking them
//     keeps their extern "C" functions in the static library.
#[cfg(not(test))]
extern crate libjs_rust;
#[cfg(not(test))]
extern crate libweb_rust;
