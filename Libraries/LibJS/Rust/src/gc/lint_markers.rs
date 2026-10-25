/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The traits Ladybird's Rust GC lints (Meta/RustLints) find the types of the GC by, so that moving or renaming one
//! does not hide it from them. The lints stop with an error if one of these traits is missing. A GC pointer needs no
//! marker: the lints count every type that implements Trace by hand as one, as Gc, Value and PropertyKey do.

use super::defer_gc::{DeferGc, Deferred};
use super::root::{MarkedVec, Root};
use super::sweep::Sweep;
use super::visitor::Trace;
use crate::interpreter::vm::Vm;

/// A type whose values keep everything they hold alive, wherever they are.
///
/// # Safety
///
/// Nothing that a value of the type holds may be collected while the value lives.
pub unsafe trait KeepsAlive {}

// SAFETY: The value is registered as a root while the Root lives.
unsafe impl<T: Trace + 'static> KeepsAlive for Root<'_, T> {}
// SAFETY: The values are held by a Root.
unsafe impl<T: Trace + Clone + 'static> KeepsAlive for MarkedVec<'_, T> {}
// SAFETY: The value cannot outlive the deferral it is tied to, while which nothing is collected.
unsafe impl<T> KeepsAlive for Deferred<'_, T> {}
// SAFETY: The VM is the root set: the heap traces it through gather_roots() wherever it is held.
unsafe impl KeepsAlive for Vm {}

/// A guard that keeps the heap from collecting garbage while it lives.
///
/// # Safety
///
/// Nothing may be collected while a value of the type lives.
pub unsafe trait DefersCollections {}

// SAFETY: The heap defers collections from new() until drop().
unsafe impl DefersCollections for DeferGc<'_> {}
// SAFETY: The value cannot outlive the deferral it is tied to.
unsafe impl<T> DefersCollections for Deferred<'_, T> {}
// SAFETY: The sweep callback runs inside a collection, which no other collection interrupts.
unsafe impl DefersCollections for Sweep<'_> {}

/// A type whose values hold no cell, whatever the types of their fields can hold.
///
/// # Safety
///
/// No value of the type may hold a pointer to a cell.
pub unsafe trait HoldsNoCells {}
