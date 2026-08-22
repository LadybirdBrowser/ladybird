/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCommitSink {
    pub context: *mut c_void,
    pub content_size_changed: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        crate::layout::FfiCssPixelSize,
        crate::layout::FfiCssPixelSize,
    ),
    pub finish_commit: unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize),
}

fn commit_subtree(
    node: Node,
    callbacks: &FfiLayoutFcCallbacks,
    sink: &FfiCommitSink,
    paintables: &crate::painting::paintable_build::PaintableCommit<'_>,
    scopes: &mut crate::layout::CommitScopes<'_>,
) {
    let slot_index = callbacks.slot_index(node);
    let entry = scopes.link_for_slot(slot_index);
    let reuses_committed_subtree = scopes.subtree_was_reused(slot_index);
    debug_assert!(!reuses_committed_subtree || entry.is_some());
    if let Some(link) = entry {
        callbacks.set_saved_abspos_layout_inputs(node, link.abspos_layout_inputs);
    }
    let prepared = paintables.prepare_node(node, entry.is_some(), reuses_committed_subtree);
    let paintable_slot = prepared.slot;

    let mut has_pending_inline_box_geometry = false;
    if let Some(link) = entry
        && !paintable_slot.is_invalid()
    {
        let fragment = &link.fragment;
        debug_assert!(
            fragment.computed_svg_path.is_some()
                || !matches!(
                    callbacks.node_data(node).kind,
                    NodeKind::SVGGeometryBox | NodeKind::SVGTextBox | NodeKind::SVGTextPathBox
                ),
            "committed path-like fragment carries no computed SVG path"
        );
        let content_size_change =
            paintables.replace_committed_fragment_link(node, paintable_slot, link, reuses_committed_subtree);
        if prepared.row_existed_before_this_commit
            && let Some((old_content_size, new_content_size)) = content_size_change
        {
            // SAFETY: Every callback below copies its plain-data argument or
            // consumes one retained handle synchronously.
            unsafe {
                (sink.content_size_changed)(sink.context, callbacks.shell(node), old_content_size, new_content_size);
            }
        }

        if !reuses_committed_subtree && let Some(line_data) = &fragment.line_data {
            has_pending_inline_box_geometry = paintables.set_line_data(paintable_slot, line_data, fragment.content_inline_size);
        }
    }

    paintables.stamp_containing_block(node, paintable_slot);

    if reuses_committed_subtree {
        return;
    }

    if let Some(link) = entry {
        scopes.open_scope(&link.fragment.children);
    }
    let mut child = callbacks.first_child(node);
    while !child.is_invalid() {
        let next = callbacks.next_sibling(child);
        commit_subtree(child, callbacks, sink, paintables, scopes);
        child = next;
    }
    if entry.is_some() {
        scopes.close_scope();
    }

    if has_pending_inline_box_geometry {
        // Inline box geometry unites this block's piece rects with the box
        // models of its descendant inline paintables, which exist only now
        // that the whole subtree has committed.
        paintables.assign_inline_box_geometry(paintable_slot);
    }
}

pub(crate) fn commit_replacing(
    root: Node,
    callbacks: &FfiLayoutFcCallbacks,
    sink: &FfiCommitSink,
    pass_fragments: &crate::layout::CompletedPassFragments,
) {
    let mut scopes = crate::layout::CommitScopes::for_pass(pass_fragments);
    let paintables = crate::painting::paintable_build::PaintableCommit::new(callbacks);
    paintables.begin_commit(root);
    commit_subtree(root, callbacks, sink, &paintables, &mut scopes);
    paintables.translate_reused_subtrees();
    paintables.discard_absolute_rects_memoized_during_commit();
    let viewport_shells = paintables.committed_navigable_container_viewport_shells();
    unsafe {
        (sink.finish_commit)(sink.context, viewport_shells.as_ptr(), viewport_shells.len());
    }
}
