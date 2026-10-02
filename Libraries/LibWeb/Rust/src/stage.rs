/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The capability separating code that may call into C++ from code that computes layout or
//! records paint.

use crate::layout::HostTables;
use std::ffi::c_void;
use std::marker::PhantomData;

/// Proof that a call entered Rust from the document's main thread, and the way to the host
/// callbacks that document registered with its arena.
///
/// Only FFI entry points mint one, and every wrapper around a C++ host callback requires it, so
/// code that is not handed a token cannot call into C++. The host tables are reached only
/// through it. The raw pointer marker makes the token neither [`Send`] nor [`Sync`], so the proof
/// cannot cross onto another thread.
pub(crate) struct MainThread<'host> {
    host_tables: Option<&'host HostTables>,
    not_send_or_sync: PhantomData<*const ()>,
}

mod private {
    pub trait FfiEntry {}
}

/// A marker that only a designated FFI entry module can construct. See [`from_ffi_entry`].
pub(crate) trait FfiEntry: private::FfiEntry {}

impl<'host> MainThread<'host> {
    /// Mint a token for a unit test, which runs on the thread that owns its arena. An arena a
    /// test makes has no host.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self {
            host_tables: None,
            not_send_or_sync: PhantomData,
        }
    }

    /// Mint a token for a unit test whose arena answers to `host_tables`.
    #[cfg(test)]
    pub(crate) fn for_test_with_host(host_tables: &'host HostTables) -> Self {
        Self {
            host_tables: Some(host_tables),
            not_send_or_sync: PhantomData,
        }
    }

    /// The host tables of the arena the entry was called for, or none in a unit test.
    pub(crate) fn host_tables(&self) -> Option<&'host HostTables> {
        self.host_tables
    }
}

/// Mint a main thread token for an FFI entry point called on the arena `arena_handle` names.
///
/// The marker's type can only be constructed by the module that owns it, and this module lists
/// those types below, so code elsewhere cannot mint a token with a marker of its own. A module
/// whose layout or paint code runs without a token keeps its marker in a private
/// `main_thread_entries` child that holds only the entries minting with it, so that code can
/// neither mint a token nor call an entry that does.
///
/// # Safety
///
/// The caller must be an FFI entry point whose C++ contract requires the document thread, and
/// `arena_handle` must come from `render_state_create_document` and outlive the token.
pub(crate) unsafe fn from_ffi_entry<'host>(_: &impl FfiEntry, arena_handle: *mut c_void) -> MainThread<'host> {
    // SAFETY: Guaranteed by the caller.
    let host_tables = unsafe { HostTables::from_handle(arena_handle) };
    // A tree build's walk runs without a token, so the C++ its callbacks run must not mint one.
    assert!(
        !host_tables.tree_build_walk_is_open(),
        "a tree build walk's callback entered Rust again"
    );
    MainThread {
        host_tables: Some(host_tables),
        not_send_or_sync: PhantomData,
    }
}

macro_rules! ffi_entry {
    ($entry:path) => {
        impl private::FfiEntry for $entry {}
        impl FfiEntry for $entry {}
    };
}

ffi_entry!(crate::layout::ArenaMainThreadFfiEntry);
ffi_entry!(crate::layout::LayoutMainThreadFfiEntry);
ffi_entry!(crate::layout::UpdateMainThreadFfiEntry);
ffi_entry!(crate::layout::TreeBuildMainThreadFfiEntry);
ffi_entry!(crate::painting::ffi::MainThreadFfiEntry);
ffi_entry!(crate::painting::display_list::dump::MainThreadFfiEntry);
ffi_entry!(crate::painting::layout_tree_dump::MainThreadFfiEntry);
ffi_entry!(crate::painting::stacking_context::dump::MainThreadFfiEntry);

#[cfg(test)]
mod tests {
    use super::*;

    trait AmbiguousIfSend<A> {
        fn marker() {}
    }

    impl<T: ?Sized> AmbiguousIfSend<()> for T {}
    impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

    // Fails to compile, as the call is ambiguous, if the token ever becomes Send.
    #[test]
    fn main_thread_capability_is_not_send() {
        <MainThread as AmbiguousIfSend<_>>::marker();
    }
}
