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
use super::layout_node_arena::{ShellFactory, ShellStyleChangedHost};
use super::node_data::NodeSlotId;
use super::trace::DescribeNode;
use super::update_layout::FfiLayoutUpdateHostCallbacks;
use crate::css::style::fast_hash::FastMap as HashMap;
use crate::painting::host::FfiGeometryHostCallbacks;
use crate::painting::paintable_rows::ChromeStateCallback;
use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;

#[derive(Default)]
pub(crate) struct HostTables {
    pub(super) layout_host: Cell<Option<FfiLayoutHostCallbacks>>,
    pub(super) layout_update_host: Cell<Option<FfiLayoutUpdateHostCallbacks>>,
    /// Makes the shell of an anonymous row the first time something asks for it.
    pub(super) shell_factory: Cell<Option<ShellFactory>>,
    pub(super) shell_style_changed_host: Cell<Option<ShellStyleChangedHost>>,
    pub(crate) chrome_state_callback: Cell<Option<ChromeStateCallback>>,
    /// What the overflow pass asks the document once it has measured a box holding a scroll offset.
    pub(crate) geometry_host: Cell<Option<FfiGeometryHostCallbacks>>,
    /// How the host names a node a layout trace mentions, set when tracing begins.
    pub(super) layout_trace_describe_node: Cell<Option<DescribeNode>>,
    /// The image provider each row whose image comes from its style owns, made for the row and
    /// destroyed when the row is freed.
    pub(super) owned_image_providers: RefCell<HashMap<NodeSlotId, *mut c_void>>,
    /// The image observers and cursor values each row's style asks for, for a style that holds an
    /// image, destroyed when the row is freed if the row did not drop them before.
    pub(super) image_observer_sets: RefCell<HashMap<NodeSlotId, *mut c_void>>,
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

    /// Gives `slot` the image provider it owns. A row is given one once, while it is built.
    pub(crate) fn set_owned_image_provider(&self, slot: NodeSlotId, provider: *mut c_void) {
        assert!(!provider.is_null(), "a row was given a null owned image provider");
        let previous = self.owned_image_providers.borrow_mut().insert(slot, provider);
        assert!(previous.is_none(), "a row was given a second owned image provider");
    }

    /// The image provider `slot` owns, or null for a row whose image comes from its DOM element.
    pub(crate) fn owned_image_provider(&self, slot: NodeSlotId) -> *mut c_void {
        self.owned_image_providers
            .borrow()
            .get(&slot)
            .copied()
            .unwrap_or(std::ptr::null_mut())
    }

    /// Gives `slot` the image observer set `observers`, or none, and answers the set it held, or
    /// null. The caller registers the new set before it drops the old one, so a resource both
    /// observe is never dropped and fetched again in between.
    pub(crate) fn replace_image_observers(&self, slot: NodeSlotId, observers: *mut c_void) -> *mut c_void {
        let mut sets = self.image_observer_sets.borrow_mut();
        let previous = if observers.is_null() {
            sets.remove(&slot)
        } else {
            sets.insert(slot, observers)
        };
        previous.unwrap_or(std::ptr::null_mut())
    }

    /// The image observer set `slot` holds, or null.
    pub(crate) fn image_observers(&self, slot: NodeSlotId) -> *mut c_void {
        self.image_observer_sets
            .borrow()
            .get(&slot)
            .copied()
            .unwrap_or(std::ptr::null_mut())
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::tree_mutation::UnplacedLayoutNode;
    use crate::stage::MainThread;

    fn object(address: usize) -> *mut c_void {
        std::ptr::without_provenance_mut(address)
    }

    #[test]
    fn replacing_image_observers_hands_back_the_set_a_row_held() {
        let host_tables = HostTables::default();
        let mut arena = LayoutNodeArena::new();
        let row = arena.allocate_for_test().slot;
        assert!(host_tables.replace_image_observers(row, object(8)).is_null());
        assert_eq!(host_tables.replace_image_observers(row, object(16)), object(8));
        assert_eq!(host_tables.image_observers(row), object(16));
        assert_eq!(
            host_tables.replace_image_observers(row, std::ptr::null_mut()),
            object(16)
        );
        assert!(host_tables.image_observers(row).is_null());
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&MainThread::for_test_with_host(&host_tables));
    }

    #[test]
    fn freeing_a_subtree_takes_its_rows_image_objects_from_the_host_tables() {
        let host_tables = HostTables::default();
        let main_thread = MainThread::for_test_with_host(&host_tables);
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let kept = arena.allocate_for_test().slot;
        arena.attach_child(root, UnplacedLayoutNode::new(child), NodeSlotId::INVALID);
        host_tables.set_owned_image_provider(child, object(8));
        host_tables.replace_image_observers(root, object(16));
        host_tables.replace_image_observers(kept, object(24));

        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&main_thread);

        assert!(host_tables.owned_image_provider(child).is_null());
        assert!(host_tables.image_observers(root).is_null());
        assert_eq!(host_tables.image_observers(kept), object(24));
        arena
            .free_subtree(kept)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        assert!(host_tables.image_observer_sets.borrow().is_empty());
    }

    #[test]
    fn preparing_a_subtree_for_detach_drops_the_image_observers_of_its_styled_rows() {
        let host_tables = HostTables::default();
        let main_thread = MainThread::for_test_with_host(&host_tables);
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let text = arena.allocate_for_test().slot;
        arena
            .data(root)
            .kind
            .set(super::super::node_data::NodeKind::BlockContainer);
        arena.data(text).kind.set(super::super::node_data::NodeKind::TextNode);
        arena.attach_child(root, UnplacedLayoutNode::new(text), NodeSlotId::INVALID);
        host_tables.replace_image_observers(root, object(8));
        host_tables.replace_image_observers(text, object(16));

        super::super::layout_node_arena::prepare_subtree_for_detach(&main_thread, &arena, root);

        assert!(host_tables.image_observers(root).is_null());
        assert_eq!(host_tables.image_observers(text), object(16));
        host_tables.replace_image_observers(text, std::ptr::null_mut());
        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }
}
