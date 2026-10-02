/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host writes of a document's layout, as typed changes its render state applies to the arena. The host names
//! a node by its slot or its identity and queues the change on the document's host; it does not reach the arena.

use super::LayoutNodeArena;
use super::node_data::NodeSlotId;
use crate::css::style::tree::StyleNodeID;
use crate::render_state::{ArenaChange, DocumentHost};
use smallvec::SmallVec;

/// One write of the host to a document's layout marks or layout facts, which the render state applies to the arena
/// before anything that reads it.
pub(crate) enum LayoutChange {
    SetNeedsLayoutUpdate {
        node: NodeSlotId,
        propagate_through_ancestors: bool,
    },
    SetNeedsFullLayoutTreeUpdate(bool),
    /// What the node's content is sized from changed: its fragment caches and intrinsic sizes, and those of its
    /// ancestors, are stale.
    ResetCachedIntrinsicSizesOfSelfAndAncestors {
        node: NodeSlotId,
    },
    /// What an insertion under `parent` invalidates depends on the boxes it attaches, which only the layout tree build
    /// knows.
    DeferChildListInsertionLayoutUpdate {
        parent: NodeSlotId,
    },
    /// The text node's data changed.
    InvalidateTextContent {
        node: NodeSlotId,
    },
    /// The text under `root` renders with the language it now resolves: where any of it is cased by its language, the
    /// root lays out again.
    EnrollTextAfterLanguageChange {
        root: NodeSlotId,
    },
    RecordPartialRelayoutEscape,
    /// The absolutely positioned `child`, whose containing block is `parent`, is about to be removed.
    NoteContainedAbsposChildRemoval {
        parent: NodeSlotId,
        child: NodeSlotId,
    },
    /// The DOM node identified by `old` took `new`. Its rows, and those of the pseudo-elements `generated_for` lists,
    /// take the new identity along with their bindings; the old one leaves every row carrying it, and the new one
    /// leaves the layout tree update marks its previous holder left.
    StyleNodeChanged {
        old: Option<StyleNodeID>,
        new: Option<StyleNodeID>,
        generated_for: SmallVec<[u8; 4]>,
    },
}

impl LayoutChange {
    /// Applies the change to `arena`, the arena of the document it was queued for. A node freed since then has nothing
    /// left to change.
    pub(crate) fn apply(self, arena: &mut LayoutNodeArena) {
        match self {
            Self::SetNeedsLayoutUpdate {
                node,
                propagate_through_ancestors,
            } => {
                if arena.slot_is_live(node) {
                    arena.set_needs_layout_update(node, propagate_through_ancestors);
                }
            }
            Self::SetNeedsFullLayoutTreeUpdate(value) => arena.set_needs_full_layout_tree_update(value),
            Self::ResetCachedIntrinsicSizesOfSelfAndAncestors { node } => {
                if arena.slot_is_live(node) {
                    arena.bump_fragment_cache_epoch_of_self_and_ancestors(node);
                    arena.reset_cached_intrinsic_sizes_of_self_and_ancestors(node);
                }
            }
            Self::DeferChildListInsertionLayoutUpdate { parent } => {
                if arena.slot_is_live(parent) {
                    arena.defer_child_list_insertion_layout_update(parent);
                }
            }
            Self::InvalidateTextContent { node } => {
                if arena.slot_is_live(node) {
                    arena.invalidate_text_content(node);
                }
            }
            Self::EnrollTextAfterLanguageChange { root } => {
                if arena.slot_is_live(root) && super::rendered_text::enroll_text_after_language_change(arena, root) {
                    arena.set_needs_layout_update(root, true);
                }
            }
            Self::RecordPartialRelayoutEscape => arena.record_partial_relayout_escape(),
            Self::NoteContainedAbsposChildRemoval { parent, child } => {
                if arena.slot_is_live(parent) && arena.slot_is_live(child) {
                    arena.note_contained_abspos_child_removal(parent, child);
                }
            }
            Self::StyleNodeChanged {
                old,
                new,
                generated_for,
            } => {
                if let (Some(old), Some(new)) = (old, new) {
                    arena.move_bound_rows_to_style_node(old, new);
                    for generated_for in generated_for {
                        arena.move_bound_pseudo_element_rows_to_style_node(old, generated_for, new);
                    }
                }
                if let Some(old) = old {
                    arena.forget_style_node(old);
                }
                arena.clear_layout_tree_update_marks(new);
            }
        }
    }
}

/// Queues `change` for the render state of `host`'s document.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on the document's thread.
unsafe fn queue(host: *const DocumentHost, change: LayoutChange) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.queue_change(ArenaChange::Layout(change));
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_needs_layout_update(
    host: *const DocumentHost,
    node: NodeSlotId,
    propagate_through_ancestors: bool,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        queue(
            host,
            LayoutChange::SetNeedsLayoutUpdate {
                node,
                propagate_through_ancestors,
            },
        );
    }
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_needs_full_layout_tree_update(host: *const DocumentHost, value: bool) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetNeedsFullLayoutTreeUpdate(value)) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_reset_cached_intrinsic_sizes_of_self_and_ancestors(
    host: *const DocumentHost,
    node: NodeSlotId,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::ResetCachedIntrinsicSizesOfSelfAndAncestors { node }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_defer_child_list_insertion_layout_update(
    host: *const DocumentHost,
    parent: NodeSlotId,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::DeferChildListInsertionLayoutUpdate { parent }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_invalidate_text_content(host: *const DocumentHost, node: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::InvalidateTextContent { node }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_enroll_text_after_language_change(host: *const DocumentHost, root: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::EnrollTextAfterLanguageChange { root }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_record_partial_relayout_escape(host: *const DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::RecordPartialRelayoutEscape) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_note_contained_abspos_child_removal(
    host: *const DocumentHost,
    parent: NodeSlotId,
    child: NodeSlotId,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::NoteContainedAbsposChildRemoval { parent, child }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `generated_for` must point at `count` readable
/// pseudo-element kinds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_style_node_changed(
    host: *const DocumentHost,
    old: u32,
    new: u32,
    generated_for: *const u8,
    count: usize,
) {
    let generated_for = if count == 0 {
        SmallVec::new()
    } else {
        // SAFETY: Guaranteed by the caller.
        SmallVec::from_slice(unsafe { std::slice::from_raw_parts(generated_for, count) })
    };
    let change = LayoutChange::StyleNodeChanged {
        old: StyleNodeID::from_raw(old),
        new: StyleNodeID::from_raw(new),
        generated_for,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;

    #[test]
    fn a_change_to_a_freed_node_changes_nothing() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test();
        arena.write_shape(node.slot).set_kind(NodeKind::BlockContainer);
        let freed = node.slot;
        arena
            .free_subtree(freed)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        LayoutChange::SetNeedsLayoutUpdate {
            node: freed,
            propagate_through_ancestors: true,
        }
        .apply(&mut arena);
        assert!(!arena.slot_is_live(freed));
    }

    #[test]
    fn a_layout_mark_reaches_the_arena() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::BlockContainer);
        arena.reset_layout_update_flags_in_subtree(node);
        assert!(!arena.node_needs_layout_update(node));
        LayoutChange::SetNeedsLayoutUpdate {
            node,
            propagate_through_ancestors: false,
        }
        .apply(&mut arena);
        assert!(arena.node_needs_layout_update(node));
        arena
            .free_subtree(node)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }
}
