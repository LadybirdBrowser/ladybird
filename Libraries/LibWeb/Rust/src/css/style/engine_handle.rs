/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How C++ and the layout arena name a document's style engine.

use super::StyleEngine;

/// What C++ holds for one document's style engine, opaque to C++. Rust reaches the engine through
/// it only by the methods below, each of which says what its caller guarantees, rather than by
/// casting a pointer wherever it pleases.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StyleEngineHandle(*mut StyleEngine);

// SAFETY: The handle only names the engine; every borrow of the engine through it is unsafe, and its caller guarantees
// that nothing else borrows the engine meanwhile, on whichever thread.
unsafe impl Send for StyleEngineHandle where StyleEngine: Send {}

impl StyleEngineHandle {
    /// A handle that names no engine.
    #[must_use]
    pub const fn null() -> Self {
        Self(std::ptr::null_mut())
    }

    #[must_use]
    pub fn is_null(self) -> bool {
        self.0.is_null()
    }

    /// Hands `engine` over to the handle, which names it until [`Self::destroy`].
    pub(crate) fn create(engine: Box<StyleEngine>) -> Self {
        Self(Box::into_raw(engine))
    }

    /// A handle for an engine that its caller owns and keeps alive while the handle is used.
    pub fn from_raw(engine: *mut StyleEngine) -> Self {
        Self(engine)
    }

    /// Takes the engine back from the handle.
    ///
    /// # Safety
    ///
    /// The handle must come from [`Self::create`] and not be used again.
    pub(crate) unsafe fn destroy(self) -> Box<StyleEngine> {
        assert!(!self.is_null(), "style engine handle is null");
        // SAFETY: Guaranteed by the caller.
        unsafe { Box::from_raw(self.0) }
    }

    /// The engine, to read.
    ///
    /// # Safety
    ///
    /// The handle must name a live engine, and no mutable borrow of it may be live while the
    /// returned one is used.
    pub unsafe fn get<'a>(self) -> &'a StyleEngine {
        assert!(!self.is_null(), "style engine handle is null");
        // SAFETY: Guaranteed by the caller.
        unsafe { &*self.0 }
    }

    /// The engine, to change.
    ///
    /// # Safety
    ///
    /// The handle must name a live engine, and no other borrow of it may be live while the
    /// returned one is used.
    pub unsafe fn get_mut<'a>(self) -> &'a mut StyleEngine {
        assert!(!self.is_null(), "style engine handle is null");
        // SAFETY: Guaranteed by the caller.
        unsafe { &mut *self.0 }
    }
}
