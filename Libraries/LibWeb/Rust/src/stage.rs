/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The capability separating code that may call into C++ from code that computes layout or
//! records paint.

use crate::layout::HostTables;
use crate::render_state::DocumentHost;
use std::marker::PhantomData;

/// Proof that a call entered Rust from the document's main thread, and the way to that document's
/// host: the host callbacks it registered, and what it keeps of its recordings.
///
/// Only FFI entry points mint one, and every wrapper around a C++ host callback requires it, so
/// code that is not handed a token cannot call into C++. The host is reached only through it.
/// The raw pointer marker makes the token neither [`Send`] nor [`Sync`], so the proof
/// cannot cross onto another thread.
pub(crate) struct MainThread<'host> {
    host: Option<&'host DocumentHost>,
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
            host: None,
            not_send_or_sync: PhantomData,
        }
    }

    /// Mint a token for a unit test whose arena answers to `host`.
    #[cfg(test)]
    pub(crate) fn for_test_with_host(host: &'host DocumentHost) -> Self {
        Self {
            host: Some(host),
            not_send_or_sync: PhantomData,
        }
    }

    /// The host of the document the entry was called for, or none in a unit test.
    pub(crate) fn host(&self) -> Option<&'host DocumentHost> {
        self.host
    }

    /// The host tables of the document the entry was called for, or none in a unit test.
    pub(crate) fn host_tables(&self) -> Option<&'host HostTables> {
        self.host.map(DocumentHost::host_tables)
    }
}

/// Mint a main thread token for an FFI entry point called with the document host `host`.
///
/// The marker's type can only be constructed by the module that owns it, and this module lists
/// those types below, so code elsewhere cannot mint a token with a marker of its own. A module
/// whose layout or paint code runs without a token keeps its marker in a private
/// `main_thread_entries` child that holds only the entries minting with it, so that code can
/// neither mint a token nor call an entry that does.
///
/// # Safety
///
/// The caller must be an FFI entry point whose C++ contract requires the document thread.
pub(crate) unsafe fn from_ffi_entry<'host>(_: &impl FfiEntry, host: &'host DocumentHost) -> MainThread<'host> {
    MainThread {
        host: Some(host),
        not_send_or_sync: PhantomData,
    }
}

/// Defines `MainThreadFfiEntry`, the marker of a module whose FFI entry points mint main thread tokens, which only that
/// module can make, and `main_thread()`, which mints the token with it. The module's marker is listed below.
macro_rules! main_thread_ffi_entries {
    () => {
        /// Mints the main thread token for this module's FFI entry points; only this module can make one.
        pub(crate) struct MainThreadFfiEntry {
            _private: (),
        }

        /// The main thread token for one of this module's FFI entry points, called with the document host `host`.
        ///
        /// # Safety
        ///
        /// As for [`crate::stage::from_ffi_entry`].
        unsafe fn main_thread(host: &$crate::render_state::DocumentHost) -> $crate::stage::MainThread<'_> {
            // SAFETY: Guaranteed by the caller.
            unsafe { $crate::stage::from_ffi_entry(&MainThreadFfiEntry { _private: () }, host) }
        }
    };
}

pub(crate) use main_thread_ffi_entries;

macro_rules! ffi_entry {
    ($entry:path) => {
        impl private::FfiEntry for $entry {}
        impl FfiEntry for $entry {}
    };
}

ffi_entry!(crate::layout::ArenaMainThreadFfiEntry);
ffi_entry!(crate::layout::UpdateMainThreadFfiEntry);
ffi_entry!(crate::painting::ffi::MainThreadFfiEntry);
ffi_entry!(crate::painting::display_list::dump::MainThreadFfiEntry);
ffi_entry!(crate::painting::layout_tree_dump::MainThreadFfiEntry);
ffi_entry!(crate::painting::stacking_context::dump::MainThreadFfiEntry);
ffi_entry!(crate::render_state::OwedWorkPayment);

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
