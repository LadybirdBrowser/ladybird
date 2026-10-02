/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host registers with a document's layout arena: the callback tables it answers
//! through. None of it is data layout reads, so it is kept beside the arena rather than in it, and
//! it is reached only through the main thread token, which entry points mint from the handle C++
//! holds. Code that borrows the arena and is not handed a token cannot reach a callback.

use super::LayoutNodeArena;
use super::formatting_context::FfiLayoutHostCallbacks;
use super::layout_node_arena::ShellStyleChangedHost;
use super::trace::DescribeNode;
use super::update_layout::FfiLayoutUpdateHostCallbacks;
use crate::painting::paintable_rows::ChromeStateCallback;
use std::cell::Cell;
use std::ffi::c_void;

#[derive(Default)]
pub(crate) struct HostTables {
    pub(super) layout_host: Cell<Option<FfiLayoutHostCallbacks>>,
    pub(super) layout_update_host: Cell<Option<FfiLayoutUpdateHostCallbacks>>,
    pub(super) shell_style_changed_host: Cell<Option<ShellStyleChangedHost>>,
    pub(crate) chrome_state_callback: Cell<Option<ChromeStateCallback>>,
    /// How the host names a node a layout trace mentions, set when tracing begins.
    pub(super) layout_trace_describe_node: Cell<Option<DescribeNode>>,
}

impl HostTables {
    /// The host tables of the arena `handle` names.
    ///
    /// # Safety
    ///
    /// `handle` must come from `layout_arena_create` and stay live for `'a`.
    pub(crate) unsafe fn from_handle<'a>(handle: *mut c_void) -> &'a Self {
        assert!(!handle.is_null(), "layout node arena handle is null");
        // SAFETY: Guaranteed by the caller. The projection does not borrow the arena beside it.
        unsafe { &(*handle.cast::<ArenaHandle>()).host_tables }
    }
}

/// What `layout_arena_create` hands C++: the arena, first, so that a handle is also a pointer to
/// it, and the host tables beside it.
#[repr(C)]
pub(crate) struct ArenaHandle {
    arena: LayoutNodeArena,
    host_tables: HostTables,
}

const _: () = assert!(std::mem::offset_of!(ArenaHandle, arena) == 0);

impl ArenaHandle {
    pub(crate) fn new() -> Self {
        Self {
            arena: LayoutNodeArena::new(),
            host_tables: HostTables::default(),
        }
    }

    pub(crate) fn arena(&self) -> &LayoutNodeArena {
        &self.arena
    }
}
