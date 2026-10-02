/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The capability separating code that may call into C++ from code that computes layout or
//! records paint.

use std::marker::PhantomData;

/// Proof that a call entered Rust from the document's main thread.
///
/// Only FFI entry points mint one, and every wrapper around a C++ host callback requires it, so
/// code that is not handed a token cannot call into C++. The raw pointer marker makes the token
/// neither [`Send`] nor [`Sync`], so the proof cannot cross onto another thread.
pub(crate) struct MainThread {
    not_send_or_sync: PhantomData<*const ()>,
}

const _: () = assert!(size_of::<MainThread>() == 0);

mod private {
    pub trait FfiEntry {}
}

/// A marker that only a designated FFI entry module can construct. See [`from_ffi_entry`].
pub(crate) trait FfiEntry: private::FfiEntry {}

impl MainThread {
    /// Mint a token for a unit test, which runs on the thread that owns its arena.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// Mint a main thread token for an FFI entry point.
///
/// The marker's type can only be constructed by the module that owns it, and this module lists
/// those types below, so code elsewhere cannot mint a token with a marker of its own.
///
/// # Safety
///
/// The caller must be an FFI entry point whose C++ contract requires the document thread.
pub(crate) unsafe fn from_ffi_entry(_: &impl FfiEntry) -> MainThread {
    MainThread {
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
