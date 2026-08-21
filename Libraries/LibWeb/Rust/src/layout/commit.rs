/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommittedBoxMetrics {
    pub fragment_identity: u64,
    pub reuses_committed_subtree: bool,
    pub content_offset: crate::layout::FfiCssPixelPoint,
    pub content_inline_size: crate::layout::CssPixels,
    pub content_block_size: crate::layout::CssPixels,
    pub margin_left: crate::layout::CssPixels,
    pub margin_right: crate::layout::CssPixels,
    pub margin_top: crate::layout::CssPixels,
    pub margin_bottom: crate::layout::CssPixels,
    pub border_left: crate::layout::CssPixels,
    pub border_right: crate::layout::CssPixels,
    pub border_top: crate::layout::CssPixels,
    pub border_bottom: crate::layout::CssPixels,
    pub padding_left: crate::layout::CssPixels,
    pub padding_right: crate::layout::CssPixels,
    pub padding_top: crate::layout::CssPixels,
    pub padding_bottom: crate::layout::CssPixels,
    pub inset_left: crate::layout::CssPixels,
    pub inset_right: crate::layout::CssPixels,
    pub inset_top: crate::layout::CssPixels,
    pub inset_bottom: crate::layout::CssPixels,
    pub containing_line_box_index: usize,
    pub has_containing_line_box_index: bool,
    pub uses_collapsing_borders_model: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiPaintableGeometry {
    pub content_inline_size: crate::layout::CssPixels,
    pub content_block_size: crate::layout::CssPixels,
    pub content_offset: crate::layout::FfiCssPixelPoint,
    pub svg_viewport_size: crate::layout::FfiCssPixelSize,
    pub margin_left: crate::layout::CssPixels,
    pub margin_right: crate::layout::CssPixels,
    pub margin_top: crate::layout::CssPixels,
    pub margin_bottom: crate::layout::CssPixels,
    pub border_left: crate::layout::CssPixels,
    pub border_right: crate::layout::CssPixels,
    pub border_top: crate::layout::CssPixels,
    pub border_bottom: crate::layout::CssPixels,
    pub padding_left: crate::layout::CssPixels,
    pub padding_right: crate::layout::CssPixels,
    pub padding_top: crate::layout::CssPixels,
    pub padding_bottom: crate::layout::CssPixels,
    pub inset_left: crate::layout::CssPixels,
    pub inset_right: crate::layout::CssPixels,
    pub inset_top: crate::layout::CssPixels,
    pub inset_bottom: crate::layout::CssPixels,
}

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
        // SVG roots are the only non-abspos partial relayout boundaries; save their committed
        // geometry so a later subtree pass can seed itself without reading the old paintable.
        if callbacks.node_data(node).kind == NodeKind::SVGSVGBox {
            let fragment = &link.fragment;
            debug_assert!(
                fragment.svg_viewport_size.is_some(),
                "committed SVG root fragment carries no viewport size"
            );
            callbacks.set_saved_committed_geometry(
                node,
                FfiPaintableGeometry {
                    content_inline_size: fragment.content_inline_size,
                    content_block_size: fragment.content_block_size,
                    content_offset: link.committed_offset,
                    svg_viewport_size: fragment.svg_viewport_size.unwrap_or_default(),
                    margin_left: fragment.margin_left,
                    margin_right: fragment.margin_right,
                    margin_top: fragment.margin_top,
                    margin_bottom: fragment.margin_bottom,
                    border_left: fragment.border_left,
                    border_right: fragment.border_right,
                    border_top: fragment.border_top,
                    border_bottom: fragment.border_bottom,
                    padding_left: fragment.padding_left,
                    padding_right: fragment.padding_right,
                    padding_top: fragment.padding_top,
                    padding_bottom: fragment.padding_bottom,
                    inset_left: link.inset_left,
                    inset_right: link.inset_right,
                    inset_top: link.inset_top,
                    inset_bottom: link.inset_bottom,
                },
            );
        }
    }
    let prepared = paintables.prepare_node(node, entry.is_some(), reuses_committed_subtree);
    let paintable_slot = prepared.slot;

    let mut has_pending_inline_box_geometry = false;
    if let Some(link) = entry
        && !paintable_slot.is_invalid()
    {
        let fragment = &link.fragment;
        let box_metrics = FfiCommittedBoxMetrics {
            fragment_identity: fragment.identity,
            reuses_committed_subtree,
            content_offset: link.committed_offset,
            content_inline_size: fragment.content_inline_size,
            content_block_size: fragment.content_block_size,
            margin_left: fragment.margin_left,
            margin_right: fragment.margin_right,
            margin_top: fragment.margin_top,
            margin_bottom: fragment.margin_bottom,
            border_left: fragment.border_left,
            border_right: fragment.border_right,
            border_top: fragment.border_top,
            border_bottom: fragment.border_bottom,
            padding_left: fragment.padding_left,
            padding_right: fragment.padding_right,
            padding_top: fragment.padding_top,
            padding_bottom: fragment.padding_bottom,
            inset_left: link.inset_left,
            inset_right: link.inset_right,
            inset_top: link.inset_top,
            inset_bottom: link.inset_bottom,
            containing_line_box_index: link.containing_line_box_index.unwrap_or(0),
            has_containing_line_box_index: link.containing_line_box_index.is_some(),
            uses_collapsing_borders_model: fragment.uses_collapsing_borders_model,
        };
        let content_size_change = paintables.set_box_metrics(paintable_slot, &box_metrics);
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

        if !reuses_committed_subtree {
            if let Some(transform) = fragment.svg_viewport_transform {
                paintables.set_svg_viewport_transform(paintable_slot, transform);
            }
            if let Some(viewport_size) = fragment.svg_viewport_size {
                paintables.set_svg_viewport_size(paintable_slot, viewport_size);
            }
            // The paintable keeps its committed path across relayout and only swaps it on an
            // identity change, which is sound only while every committed path-like fragment
            // carries a path.
            debug_assert!(
                fragment.computed_svg_path.is_some()
                    || !matches!(
                        callbacks.node_data(node).kind,
                        NodeKind::SVGGeometryBox | NodeKind::SVGTextBox | NodeKind::SVGTextPathBox
                    ),
                "committed path-like fragment carries no computed SVG path"
            );
            if let Some(path) = &fragment.computed_svg_path {
                paintables.set_computed_svg_path(paintable_slot, path);
            }
        }
        if !reuses_committed_subtree && let Some(data) = &fragment.grid_layout_data {
            paintables.set_grid_layout_data(paintable_slot, data);
        }
        if !reuses_committed_subtree && let Some(data) = &fragment.flex_layout_data {
            paintables.set_flex_layout_data(paintable_slot, data);
        }
        if !reuses_committed_subtree && let Some(tracks) = &fragment.used_grid_tracks {
            paintables.set_used_grid_tracks(paintable_slot, tracks);
        }
        if !reuses_committed_subtree && let Some(borders) = &fragment.collapsed_table_borders {
            paintables.set_collapsed_table_borders(paintable_slot, borders);
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
