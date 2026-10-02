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

/// Builds or incrementally updates a document's layout tree and applies table fixup.
///
/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_layout_tree(
    arena: *mut c_void,
    document_style_node: u32,
    document_style_record: u64,
) -> FfiLayoutTreeBuildOutcome {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    let job = TreeBuildJob {
        document_style_node: StyleNodeID::from_raw(document_style_node)
            .expect("a document that lays out is named in the style mirror"),
        document_style_record: (document_style_record != 0).then_some(document_style_record),
    };
    // Nothing of the host runs while the walk does.
    let walk = main_thread
        .host_tables()
        .expect("an FFI entry's token names its arena's host tables")
        .open_tree_build_walk();
    // SAFETY: Guaranteed by the entry point's contract.
    let answer = unsafe { crate::layout::run_tree_build_job(&main_thread, arena, job) };
    drop(walk);

    // The walk is over. What it owes the host is paid first, as it would have been while the walk
    // ran: the boxes nodes gained or lost, the host-owned objects of the rows it freed, and the
    // style changes of the shells of the boxes it kept. The image resources of the rows it stamped
    // wait for the layout update to be over.
    // SAFETY: The arena outlives the build, and no arena borrow is held across the host calls.
    answer
        .work
        .apply(&main_thread, unsafe { LayoutNodeArena::from_handle(arena) });

    // What the build found out goes to the document now that the walk that could clear DOM update
    // flags is complete, in the order the build found it out.
    // SAFETY: The tree build runs outside any layout pass, the document outlives the build, and no
    // arena borrow is held here.
    unsafe {
        FfiLayoutHostCallbacks::of(&main_thread).deliver_commit_messages(&main_thread, &answer.reports);
    }

    // The scroll containers the build gave a style come last, before any style the document
    // applies after the build.
    // SAFETY: As above.
    unsafe {
        FfiLayoutHostCallbacks::of(&main_thread)
            .take_built_scroll_containers(&main_thread, &answer.built_scroll_containers);
    }
    answer.outcome
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
