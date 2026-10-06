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
use super::node_data::{NodeKind, NodeSlotId};
use super::trace::DescribeNode;
use super::update_layout::FfiLayoutUpdateHostCallbacks;
use crate::css::style::fast_hash::FastMap as HashMap;
use crate::painting::host::FfiGeometryHostCallbacks;
use crate::painting::paintable_rows::ChromeStateCallback;
use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;

/// What the shell factory needs to know of a row to make its layout node.
#[derive(Clone, Copy)]
pub(crate) struct ShellFacts {
    pub(crate) id: NodeSlotId,
    pub(crate) kind: NodeKind,
}

#[derive(Default)]
pub(crate) struct HostTables {
    pub(super) layout_host: Cell<Option<FfiLayoutHostCallbacks>>,
    pub(super) layout_update_host: Cell<Option<FfiLayoutUpdateHostCallbacks>>,
    /// Makes the shell of an anonymous row the first time something asks for it.
    pub(super) shell_factory: Cell<Option<ShellFactory>>,
    pub(super) shell_style_changed_host: Cell<Option<ShellStyleChangedHost>>,
    pub(crate) chrome_state_callback: Cell<Option<ChromeStateCallback>>,
    /// What the overflow pass tells the document once it has settled a box's scroll offset.
    pub(crate) geometry_host: Cell<Option<FfiGeometryHostCallbacks>>,
    /// How the host names a node a layout trace mentions, set when tracing begins.
    pub(super) layout_trace_describe_node: Cell<Option<DescribeNode>>,
    /// The image provider each row whose image comes from its style owns, made for the row and
    /// destroyed when the row is freed.
    pub(super) owned_image_providers: RefCell<HashMap<NodeSlotId, *mut c_void>>,
    /// The image observers and cursor values each row's style asks for, for a style that holds an
    /// image, destroyed when the row is freed if the row did not drop them before.
    pub(super) image_observer_sets: RefCell<HashMap<NodeSlotId, *mut c_void>>,
    /// The layout node C++ made for each row something asked for one, which rows themselves do
    /// not name. A row is keyed with its generation, so a slot restamped before the layout node of
    /// the row it replaced is destroyed holds both apart.
    pub(crate) shells: RefCell<HashMap<NodeSlotId, NonNull<c_void>>>,
    /// Whether the document runs a layout update, which it does one at a time.
    pub(crate) update_layout_running: Cell<bool>,
    /// The image resources the tree builds of the rounds the host was paid for owe the rows they stamped, which the
    /// host attaches once its layout update is over.
    pub(super) owed_images: RefCell<Vec<super::update_layout::OwedImage>>,
}

impl HostTables {
    pub(super) fn clear_callbacks(&self) {
        self.layout_host.set(None);
        self.layout_update_host.set(None);
        self.shell_factory.set(None);
        self.shell_style_changed_host.set(None);
        self.chrome_state_callback.set(None);
        self.geometry_host.set(None);
    }

    /// The layout node of the row `facts` describes, made by the shell factory the first time
    /// something asks for it.
    pub(crate) fn shell_of(&self, facts: ShellFacts) -> *mut c_void {
        if let Some(shell) = self.shells.borrow().get(&facts.id) {
            return shell.as_ptr();
        }
        let Some((context, factory)) = self.shell_factory.get() else {
            return std::ptr::null_mut();
        };
        assert!(
            !crate::stage_thread::style_layout_thread().is_current(),
            "a layout node is made on the main thread, whose heap it is allocated from"
        );
        // SAFETY: Registration and unregistration keep the factory context live. The factory's
        // layout node attaches itself to the table, which no borrow is held of across the call.
        unsafe { factory(context, facts.id, facts.kind) };
        self.shells
            .borrow()
            .get(&facts.id)
            .map_or(std::ptr::null_mut(), |shell| shell.as_ptr())
    }

    /// Records the layout node C++ made for the row `id`.
    pub(crate) fn attach_shell(&self, id: NodeSlotId, shell: *mut c_void) {
        let shell = NonNull::new(shell).expect("a row was given a null layout node");
        let previous = self.shells.borrow_mut().insert(id, shell);
        assert!(previous.is_none(), "a row was given a second layout node");
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

/// The arena of a document's render state, and the layout stage's scratch beside it.
pub(crate) struct ArenaHandle {
    arena: LayoutNodeArena,
    layout_scratch: super::run_records::LayoutScratch,
}

impl ArenaHandle {
    pub(crate) fn new() -> Self {
        Self {
            arena: LayoutNodeArena::new(),
            layout_scratch: Default::default(),
        }
    }

    pub(crate) fn arena(&self) -> &LayoutNodeArena {
        &self.arena
    }

    /// The layout stage's own scratch.
    pub(crate) fn layout_scratch(&self) -> &super::run_records::LayoutScratch {
        &self.layout_scratch
    }

    pub(crate) fn arena_mut(&mut self) -> &mut LayoutNodeArena {
        &mut self.arena
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
        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
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
            .destroy_shells_and_invoke_callbacks(&MainThread::for_test_with_host(&host));
    }

    #[test]
    fn freeing_a_subtree_takes_its_rows_image_objects_from_the_host_tables() {
        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
        let main_thread = MainThread::for_test_with_host(&host);
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
    fn a_restamped_slot_does_not_answer_with_the_layout_node_of_the_row_it_replaced() {
        use crate::layout::tree_mutation::{HostCalls, OwedHostWork};

        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
        let main_thread = MainThread::for_test_with_host(&host);
        let mut arena = LayoutNodeArena::new();
        arena.queue_box_presence();
        let work = OwedHostWork::default();
        let freed = arena.allocate_for_test().slot;
        host_tables.attach_shell(freed, object(8));

        // A walk frees the row and stamps another in its slot before its host work is applied.
        HostCalls(&work).free_subtree(&mut arena, freed);
        let reused = arena.allocate_for_test().slot;
        assert_eq!(reused.slot_index(), freed.slot_index());
        assert!(host_tables.shells.borrow().get(&reused).is_none());
        assert_eq!(
            host_tables.shells.borrow().get(&freed).map(|shell| shell.as_ptr()),
            Some(object(8))
        );
        // The walk can ask for the new row's layout node before the old one is destroyed.
        host_tables.attach_shell(reused, object(16));

        work.resolve(&arena).pay(&main_thread);
        assert_eq!(
            host_tables.shells.borrow().get(&reused).map(|shell| shell.as_ptr()),
            Some(object(16))
        );
        assert_eq!(host_tables.shells.borrow().len(), 1);
        arena
            .free_subtree(reused)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }

    #[test]
    fn freeing_rows_destroys_exactly_their_layout_nodes() {
        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
        let main_thread = MainThread::for_test_with_host(&host);
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let kept = arena.allocate_for_test().slot;
        arena.attach_child(root, UnplacedLayoutNode::new(child), NodeSlotId::INVALID);
        host_tables.attach_shell(child, object(8));
        host_tables.attach_shell(kept, object(16));

        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        assert!(host_tables.shells.borrow().get(&child).is_none());
        assert_eq!(
            host_tables.shells.borrow().get(&kept).map(|shell| shell.as_ptr()),
            Some(object(16))
        );
        arena
            .free_subtree(kept)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        assert_eq!(host_tables.shells.borrow().len(), 0);
    }

    #[test]
    fn a_tree_build_lets_go_of_a_freed_rows_image_objects_once_the_walk_is_over() {
        use crate::layout::tree_mutation::{HostCalls, OwedHostWork};

        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
        let main_thread = MainThread::for_test_with_host(&host);
        let mut arena = LayoutNodeArena::new();
        arena.queue_box_presence();
        let work = OwedHostWork::default();
        let freed = arena.allocate_for_test().slot;
        arena
            .write_shape(freed)
            .set_kind(super::super::node_data::NodeKind::BlockContainer);
        host_tables.replace_image_observers(freed, object(8));

        let host_calls = HostCalls(&work);
        super::super::layout_node_arena::prepare_subtree_for_detach(host_calls, &arena, freed);
        host_calls.free_subtree(&mut arena, freed);
        // The walk owes the host the observers it let go of, so they stay until the walk is over.
        assert_eq!(host_tables.image_observers(freed), object(8));

        // A row the walk builds in the freed row's slot is another row, whose objects stay.
        let reused = arena.allocate_for_test().slot;
        assert_eq!(reused.slot_index(), freed.slot_index());
        host_tables.replace_image_observers(reused, object(16));

        work.resolve(&arena).pay(&main_thread);
        assert!(host_tables.image_observers(freed).is_null());
        assert_eq!(host_tables.image_observers(reused), object(16));
        host_tables.replace_image_observers(reused, std::ptr::null_mut());
        arena
            .free_subtree(reused)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }

    #[test]
    fn preparing_a_subtree_for_detach_drops_the_image_observers_of_its_styled_rows() {
        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
        let main_thread = MainThread::for_test_with_host(&host);
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let text = arena.allocate_for_test().slot;
        arena
            .write_shape(root)
            .set_kind(super::super::node_data::NodeKind::BlockContainer);
        arena
            .write_shape(text)
            .set_kind(super::super::node_data::NodeKind::TextNode);
        arena.attach_child(root, UnplacedLayoutNode::new(text), NodeSlotId::INVALID);
        host_tables.replace_image_observers(root, object(8));
        host_tables.replace_image_observers(text, object(16));

        let work = crate::layout::tree_mutation::OwedHostWork::default();
        arena.queue_box_presence();
        super::super::layout_node_arena::prepare_subtree_for_detach(
            crate::layout::tree_mutation::HostCalls(&work),
            &arena,
            root,
        );
        work.resolve(&arena).pay(&main_thread);

        assert!(host_tables.image_observers(root).is_null());
        assert_eq!(host_tables.image_observers(text), object(16));
        host_tables.replace_image_observers(text, std::ptr::null_mut());
        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }
}
