/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCommitSink {
    pub context: *mut c_void,
    pub content_size_changed_for_container_queries: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub finish_commit: unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize),
}

fn commit_subtree(
    node: Node,
    callbacks: &FfiLayoutFcCallbacks,
    sink: &FfiCommitSink,
    paintables: &mut crate::painting::paintable_build::PaintableCommit<'_>,
    links_by_slot: &HashMap<u32, &FragmentLink>,
    pass_fragments: &fragment_tree::CompletedPassFragments,
    enclosing_line_root_content_changed: bool,
) {
    let slot_index = callbacks.slot_index(node);
    let entry = links_by_slot.get(&slot_index).copied();
    let reuses_committed_subtree = pass_fragments.subtree_was_reused(slot_index);
    debug_assert!(!reuses_committed_subtree || entry.is_some());
    if let Some(link) = entry {
        callbacks.set_saved_abspos_layout_inputs(node, link.abspos_layout_inputs);
    }
    let prepared = paintables.prepare_node(
        node,
        entry.is_some(),
        reuses_committed_subtree,
        enclosing_line_root_content_changed,
    );

    let mut has_pending_inline_box_geometry = false;
    let mut line_root_content_changed_for_children = enclosing_line_root_content_changed;
    if let Some(link) = entry
        && prepared.has_paintable_row
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
        let replaced = paintables.replace_committed_fragment_link(
            node,
            link,
            reuses_committed_subtree,
            enclosing_line_root_content_changed,
        );
        if fragment.line_data.is_some() {
            line_root_content_changed_for_children = replaced.committed_fragment_identity_changed;
        }
        if prepared.row_existed_before_this_commit
            && let Some((old_content_size, new_content_size)) = replaced.content_size_change
            && crate::layout::node_facts::node_style_view(callbacks.node_data(node)).is_some_and(|style| {
                content_size_change_affects_container_queries(style, old_content_size, new_content_size)
            })
        {
            // SAFETY: Every callback below copies its plain-data argument or
            // consumes one retained handle synchronously.
            unsafe {
                (sink.content_size_changed_for_container_queries)(sink.context, callbacks.shell(node));
            }
        }

        if !reuses_committed_subtree && let Some(line_data) = &fragment.line_data {
            has_pending_inline_box_geometry = paintables.set_line_data(node, line_data, fragment.content_inline_size);
        }
    }

    paintables.stamp_containing_block(node);

    if reuses_committed_subtree {
        return;
    }

    let mut child = callbacks.first_child(node);
    while !child.is_invalid() {
        let next = callbacks.next_sibling(child);
        commit_subtree(
            child,
            callbacks,
            sink,
            &mut *paintables,
            links_by_slot,
            pass_fragments,
            line_root_content_changed_for_children,
        );
        child = next;
    }

    if has_pending_inline_box_geometry {
        // Inline box geometry unites this block's piece rects with the box
        // models of its descendant inline paintables, which exist only now
        // that the whole subtree has committed.
        paintables.assign_inline_box_geometry(node);
    }
}

fn content_size_change_affects_container_queries(
    style: crate::css::computed_value_views::ComputedValuesView<'_>,
    old_size: FfiCssPixelSize,
    new_size: FfiCssPixelSize,
) -> bool {
    let box_values = style.box_values();
    if box_values.is_size_container {
        return old_size.width != new_size.width || old_size.height != new_size.height;
    }
    if !box_values.is_inline_size_container {
        return false;
    }
    if style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB {
        old_size.width != new_size.width
    } else {
        old_size.height != new_size.height
    }
}

pub(crate) fn commit_replacing(
    root: Node,
    callbacks: &FfiLayoutFcCallbacks,
    sink: &FfiCommitSink,
    pass_fragments: &fragment_tree::CompletedPassFragments,
) {
    let links_by_slot = pass_fragments.links_by_slot();
    let mut paintables = crate::painting::paintable_build::PaintableCommit::new(callbacks);
    paintables.begin_commit();
    commit_subtree(
        root,
        callbacks,
        sink,
        &mut paintables,
        &links_by_slot,
        pass_fragments,
        false,
    );
    paintables.discard_absolute_rects_memoized_during_commit();
    let viewport_shells = paintables.committed_navigable_container_viewport_shells();
    unsafe {
        (sink.finish_commit)(sink.context, viewport_shells.as_ptr(), viewport_shells.len());
    }
}
