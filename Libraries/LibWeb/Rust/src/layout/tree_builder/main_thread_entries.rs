/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// Detaches a top-layer element's layout placement and clears every stale projected subtree.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_detach_top_layer_element_layout_subtree(arena: *mut c_void, element: u32) {
    assert!(!arena.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let arena = arena.cast::<LayoutNodeArena>();
    let host = StaleSubtreeHost {
        arena,
        host_calls: HostCalls::Now(&main_thread),
    };
    // A top layer member the style engine no longer tracks has left the DOM. Nothing of it is in the
    // mirror, and nothing of it is bound to a row, so there is nothing to detach or clear.
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    // NB: Called at DOM mutation processing time, outside layout tree construction.
    let element_layout_node = host.arena().bound_row(element);
    if !element_layout_node.is_invalid() {
        let topmost = topmost_layout_node_of_top_layer_placement(arena, element_layout_node);
        let layout_node_to_detach = if topmost.is_invalid() {
            element_layout_node
        } else {
            topmost
        };
        // SAFETY: The arena outlives this call, and the shared borrow ends before the subtree is
        // freed.
        super::super::layout_node_arena::prepare_subtree_for_detach(
            HostCalls::Now(&main_thread),
            unsafe { &*arena },
            layout_node_to_detach,
        );
        if unsafe { &*arena }.detach_from_parent(layout_node_to_detach) {
            free_subtree_and_destroy_shells(&main_thread, arena, layout_node_to_detach);
        }
    }

    clear_stale_subtree(host, element, StaleSubtreeClearScope::InclusiveBoundedToRoot);
    clear_stale_assigned_slottables(host, element);
}

/// Builds or incrementally updates a document's layout tree and applies table fixup.
///
/// # Safety
///
/// The callback table, arena, and document must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_layout_tree(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    arena: *mut c_void,
    document: *mut c_void,
    document_style_node: u32,
    document_style_record: u64,
) -> FfiLayoutTreeBuildOutcome {
    assert!(!document.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // What the walk owes the host, which the walk, holding no main thread token, can only queue.
    let work = TreeBuildHostWork::default();
    let walk = main_thread
        .host_tables()
        .expect("an FFI entry's token names its arena's host tables")
        .open_tree_build_walk();
    // SAFETY: Guaranteed by the entry point's contract.
    let host = unsafe { dom_tree_builder_host(callbacks, arena, &work, &walk) };
    host.arena().queue_box_presence_for_tree_build();
    let document_identity =
        StyleNodeID::from_raw(document_style_node).expect("a document that lays out is named in the style mirror");
    host.layout().arena().set_document_style_node(document_identity);
    let mut state = TreeBuilderState::default();
    // The viewport's style is the document's, which the host makes rather than publishes, so a
    // build that may build the viewport is handed it before it starts.
    if document_style_record != 0 {
        state.document_style = Some(
            host.layout()
                .arena()
                .with_style_engine(|engine| DerivedStyleRecord::pin(engine, document_style_record)),
        );
    }
    let mut context = TreeBuilderContext {
        document_needs_full_layout_tree_update: host.layout().arena().needs_full_layout_tree_update(),
        ..Default::default()
    };
    // Whether the document already had a viewport, read before the build replaces it.
    let document_had_layout_node = !host.layout().arena().layout_root().is_invalid();

    update_layout_tree_from(
        &host,
        &mut state,
        document_identity,
        &mut context,
        false,
        FfiInsertionMode::Append,
        true,
    );

    let document_layout_node = host.layout().arena().layout_root();
    let rebuilt_subtrees_were_updated_individually = !document_layout_node.is_invalid()
        && !(context.document_needs_full_layout_tree_update
            || !document_had_layout_node
            || state.layout_tree_update_escaped_rebuild_roots);
    if !document_layout_node.is_invalid() {
        let layout_host = host.layout();
        if rebuilt_subtrees_were_updated_individually {
            fixup_tables_in_rebuilt_subtrees(
                &layout_host,
                &state.rebuilt_subtree_roots,
                &state.reused_child_list_update_roots,
                &state.additional_table_fixup_roots,
            );
        } else {
            layout_host.arena().set_needs_full_scrollable_overflow_recalculation();
            fixup_tables(&layout_host, document_layout_node);
        }

        // https://drafts.csswg.org/css-scrollbars/#scrollbar-width
        // UAs must apply the scrollbar-color value set on the root element to the viewport.
        // The document element is the document's only DOM child the style mirror holds: a doctype, a
        // comment and a processing instruction hold no place in its child sequence, and a document
        // can have no text child.
        let root_layout_node = host
            .first_dom_child(document_identity)
            .map_or(NodeSlotId::INVALID, |document_element| {
                layout_host.arena().bound_row(document_element)
            });
        if !root_layout_node.is_invalid() {
            let scrollbar_width = layout_host
                .style(root_layout_node)
                .expect("the document element's box publishes its style during the build")
                .misc_reset()
                .scrollbar_width;
            layout_host
                .arena()
                .update_layout_style(layout_host.host_calls(), document_layout_node, |style| {
                    style.set_scrollbar_width(scrollbar_width);
                });
        }
    }

    for &element in &state.layout_tree_rebuild_requests {
        // A request that names no element asks for the whole tree, which the arena answers itself.
        let Some(element) = element else {
            host.layout().arena().set_needs_full_layout_tree_update(true);
            continue;
        };
        state.reports.push(crate::layout::commit::FfiCommitMessage::new(
            element.raw(),
            crate::layout::commit::FfiCommitMessageKind::LayoutTreeRebuildRequested,
        ));
    }

    // The walk is over. What it owes the host is paid first, as it would have been while the walk
    // ran: the boxes nodes gained or lost, the host-owned objects of the rows it freed, and the
    // style changes of the shells of the boxes it kept. The image resources of the rows it stamped
    // wait for the layout update to be over.
    drop(walk);
    // SAFETY: The arena outlives the build, and no arena borrow is held across the host calls.
    work.apply(&main_thread, unsafe { LayoutNodeArena::from_handle(arena) });

    // What the build found out goes to the document now that the walk that could clear DOM update
    // flags is complete, in the order the build found it out.
    // SAFETY: The tree build runs outside any layout pass, the document outlives the build, and no
    // arena borrow is held here.
    unsafe {
        FfiLayoutHostCallbacks::of(&main_thread).deliver_commit_messages(&main_thread, &state.reports);
    }

    // The scroll containers the build gave a style come last, before any style the document
    // applies after the build.
    // SAFETY: As above.
    let built_scroll_containers = unsafe { LayoutNodeArena::from_handle(arena) }.take_built_scroll_containers();
    // SAFETY: As above.
    unsafe {
        FfiLayoutHostCallbacks::of(&main_thread).take_built_scroll_containers(&main_thread, &built_scroll_containers);
    }

    // SAFETY: As above.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    if rebuilt_subtrees_were_updated_individually {
        let attached_roots = arena.derive_facts_after_tree_update(&state.rebuilt_subtree_roots);
        arena.resolve_deferred_child_list_insertions(&attached_roots);
    } else {
        // NB: The full layout entry must derive the facts of this tree.
        arena.record_partial_relayout_escape();
        arena.resolve_deferred_child_list_insertions(&Default::default());
    }

    // Table fixup can free a rebuilt root after it was recorded, such as whitespace at the edge of a
    // row group, so only the roots that are still live wait for the partial relayout plan.
    let live_rebuilt_subtree_roots: Vec<NodeSlotId> = state
        .rebuilt_subtree_roots
        .iter()
        .copied()
        .filter(|root| arena.slot_is_live(*root))
        .collect();
    let rebuilt_subtree_root_count = live_rebuilt_subtree_roots.len();
    arena.set_pending_rebuilt_subtree_roots(
        live_rebuilt_subtree_roots,
        state.layout_tree_update_escaped_rebuild_roots,
    );
    let viewport = arena.layout_root();
    assert!(!viewport.is_invalid(), "a layout tree build places the viewport");
    state.release_pinned_style_records(arena);
    FfiLayoutTreeBuildOutcome {
        viewport,
        rebuilt_subtree_root_count,
        layout_tree_update_escaped_rebuild_roots: state.layout_tree_update_escaped_rebuild_roots,
        needs_another_build_pass: !state.layout_tree_rebuild_requests.is_empty(),
    }
}

/// Detaches what is left of the boxes of the node `style_node` names as the node leaves the
/// document, while its identity still names them. Its box is read until the parent's rebuild
/// frees it, so its style record is pinned and its committed box cleared now. Its box's top layer
/// placement is a viewport child rather than part of the parent's box subtree, so the parent's
/// rebuild never reaches it, and it is detached and freed here. The rows are found by identity,
/// so this makes no shell.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_detach_remaining_layout_rows_for_removal(arena: *mut c_void, style_node: u32) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let Some(node) = StyleNodeID::from_raw(style_node) else {
        return;
    };
    // A pseudo-element's boxes are found through its generator's identity, so they go while the
    // identity still finds them. A ::backdrop box sits outside the generator's box, so no rebuild
    // of the parent would free it.
    if node.element_index().is_some() {
        clear_synthetic_pseudo_element_boxes(HostCalls::Now(&main_thread), arena.cast(), node);
    }
    // SAFETY: As above.
    let row = unsafe { LayoutNodeArena::from_handle(arena) }.bound_row(node);
    if row.is_invalid() {
        return;
    }
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.pin_style_record_for_detachment(row);
    // SAFETY: As above; the clear borrows the arena for itself.
    unsafe { crate::painting::ffi::paintable_cleared_from_node(HostCalls::Now(&main_thread), arena, row) };
    let arena = arena.cast::<LayoutNodeArena>();
    let top_layer_placement = topmost_layout_node_of_top_layer_placement(arena, row);
    if !top_layer_placement.is_invalid() {
        // SAFETY: As above; the shared borrow ends before the subtree is freed.
        super::super::layout_node_arena::prepare_subtree_for_detach(
            HostCalls::Now(&main_thread),
            unsafe { &*arena },
            top_layer_placement,
        );
        let was_attached = unsafe { &*arena }.detach_from_parent(top_layer_placement);
        assert!(was_attached, "a top layer placement is a viewport child");
        free_subtree_and_destroy_shells(&main_thread, arena, top_layer_placement);
    }
}
