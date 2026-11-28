/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The garbage collector as Rust reaches it: a small layer over LibGC's C interface (Libraries/LibGC/CAPI.h), which
//! every Rust crate that defines or traces cells goes through.
//!
//! - [`Visitor`] and [`Trace`], with `#[derive(Trace)]`, for visiting the edges of cells.
//! - [`MemberSlot`], [`CellPointer`] and [`NanBoxedValue`], which a crate implements for its own pointer types to have
//!   the visitor take them, and the compressed pointer encoding of [`member`].
//! - [`WeakImpl`], weak references to cells.
//! - [`cell`], the header, type info, allocation and destruction of cells whose types Rust defines.
//! - [`capi`], the raw declarations, for what the rest does not cover.

extern crate self as libgc_api;

pub mod capi;
pub mod cell;
pub mod layout;
pub mod member;
pub mod visitor;
pub mod weak;

pub use libgc_api_macros::Trace;
pub use visitor::{CellPointer, MemberSlot, NanBoxedValue, Trace, Visitor};
pub use weak::WeakImpl;
