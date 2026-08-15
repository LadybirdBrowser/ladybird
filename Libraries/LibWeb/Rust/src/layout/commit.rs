/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiTableCellCoordinates {
    pub row_index: usize,
    pub column_index: usize,
    pub row_span: usize,
    pub column_span: usize,
}

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
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommitNodeResult {
    pub paintable: *mut c_void,
    pub paintable_for_children: *mut c_void,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommitPosition {
    pub parent_paintable: *mut c_void,
    pub insert_before_paintable: *mut c_void,
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
    pub begin_commit: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiCommitPosition,
    pub finish_commit: unsafe extern "C" fn(*mut c_void),
    pub prepare_node: unsafe extern "C" fn(*mut c_void, *mut c_void, bool, bool) -> *mut c_void,
    pub set_box_metrics: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiCommittedBoxMetrics),
    pub set_override_borders: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiBordersData),
    pub set_table_cell_coordinates: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiTableCellCoordinates),
    pub begin_line_data: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub begin_line: unsafe extern "C" fn(*mut c_void, FfiLineRecord),
    pub emit_fragment: unsafe extern "C" fn(*mut c_void, FfiCommittedFragment),
    pub emit_inline_box_piece: unsafe extern "C" fn(*mut c_void, FfiInlineBoxPiece),
    pub finish_line_data: unsafe extern "C" fn(*mut c_void),
    pub set_svg_viewport_transform: unsafe extern "C" fn(*mut c_void, *mut c_void, crate::layout::FfiAffineTransform),
    pub set_svg_viewport_size: unsafe extern "C" fn(*mut c_void, *mut c_void, crate::layout::FfiCssPixelSize),
    pub set_computed_svg_path: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, u64),
    pub set_grid_layout_data: unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiGridLayoutData),
    pub set_flex_layout_data: unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiFlexLayoutData),
    pub set_used_grid_tracks:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiUsedGridTrackList, *const FfiUsedGridTrackList),
    pub finish_node:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut c_void, *mut c_void) -> FfiCommitNodeResult,
    pub assign_inline_box_geometry: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

fn commit_subtree(
    node: Node,
    parent_paintable: *mut c_void,
    insert_before_paintable: *mut c_void,
    callbacks: &FfiLayoutFcCallbacks,
    sink: &FfiCommitSink,
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
    // SAFETY: The C++ sink owns paintables and copies every plain-data
    // input synchronously.
    let node_shell = callbacks.shell(node);
    let paintable = unsafe {
        (sink.prepare_node)(
            sink.context,
            node_shell,
            entry.is_some(),
            reuses_committed_subtree,
        )
    };

    let mut has_pending_inline_box_geometry = false;
    if let Some(link) = entry
        && !paintable.is_null()
    {
        let fragment = &link.fragment;
        // SAFETY: Every callback below copies its plain-data argument or
        // consumes one retained handle synchronously.
        unsafe {
            (sink.set_box_metrics)(
                sink.context,
                paintable,
                FfiCommittedBoxMetrics {
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
                },
            );
        }

        if !reuses_committed_subtree {
            unsafe {
            if let Some(borders) = fragment.override_borders_data {
                (sink.set_override_borders)(sink.context, paintable, borders);
            }
            if let Some(coordinates) = fragment.table_cell_coordinates {
                (sink.set_table_cell_coordinates)(sink.context, paintable, coordinates);
            }
            }
        }

        if !reuses_committed_subtree && let Some(line_data) = &fragment.line_data {
            // SAFETY: The sink keeps one line accumulator live between
            // begin_line_data() and finish_line_data().
            let accepts_lines = unsafe { (sink.begin_line_data)(sink.context, paintable) };
            if accepts_lines {
                let line_sink = FfiLineSinkCallbacks {
                    context: sink.context,
                    begin_line: sink.begin_line,
                    emit_fragment: sink.emit_fragment,
                    emit_inline_box_piece: sink.emit_inline_box_piece,
                };
                push_line_data(line_data, fragment.content_inline_size, callbacks, line_sink);
                unsafe {
                    (sink.finish_line_data)(sink.context);
                }
                has_pending_inline_box_geometry = !line_data.inline_box_pieces.is_empty();
            }
        }

        if !reuses_committed_subtree {
            unsafe {
            if let Some(transform) = fragment.svg_viewport_transform {
                (sink.set_svg_viewport_transform)(sink.context, paintable, transform);
            }
            if let Some(viewport_size) = fragment.svg_viewport_size {
                (sink.set_svg_viewport_size)(sink.context, paintable, viewport_size);
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
                (sink.set_computed_svg_path)(sink.context, paintable, path.as_raw(), path.identity());
            }
            }
        }
        if !reuses_committed_subtree && let Some(data) = &fragment.grid_layout_data {
            data.with_ffi_view(|view| {
                unsafe { (sink.set_grid_layout_data)(sink.context, paintable, view) };
            });
        }
        if !reuses_committed_subtree && let Some(data) = &fragment.flex_layout_data {
            data.with_ffi_view(|view| {
                unsafe { (sink.set_flex_layout_data)(sink.context, paintable, view) };
            });
        }
        if !reuses_committed_subtree && let Some(tracks) = &fragment.used_grid_tracks {
            tracks.with_ffi_views(|columns, rows| {
                unsafe { (sink.set_used_grid_tracks)(sink.context, paintable, columns, rows) };
            });
        }
    }

    // SAFETY: Wiring uses only live layout and paintable pointers for this
    // synchronous commit.
    let result = unsafe {
        (sink.finish_node)(
            sink.context,
            node_shell,
            paintable,
            parent_paintable,
            insert_before_paintable,
        )
    };
    assert_eq!(result.paintable, paintable);

    if reuses_committed_subtree {
        return;
    }

    if let Some(link) = entry {
        scopes.open_scope(&link.fragment.children);
    }
    let mut child = callbacks.first_child(node);
    while !child.is_invalid() {
        let next = callbacks.next_sibling(child);
        commit_subtree(child, result.paintable_for_children, null_mut(), callbacks, sink, scopes);
        child = next;
    }
    if entry.is_some() {
        scopes.close_scope();
    }

    if has_pending_inline_box_geometry {
        // Inline box geometry unites this block's piece rects with the box
        // models of its descendant inline paintables, which exist only now
        // that the whole subtree has committed.
        unsafe { (sink.assign_inline_box_geometry)(sink.context, paintable) };
    }
}

pub(crate) fn commit_replacing(
    root: Node,
    callbacks: &FfiLayoutFcCallbacks,
    sink: &FfiCommitSink,
    pass_fragments: &crate::layout::CompletedPassFragments,
) {
    let mut scopes = crate::layout::CommitScopes::for_pass(pass_fragments);
    // SAFETY: The sink resolves and retains the paintable to replace from the
    // root, detaches it, and returns borrowed insertion pointers that stay
    // live until finish_commit().
    let position = unsafe { (sink.begin_commit)(sink.context, callbacks.shell(root)) };
    commit_subtree(
        root,
        position.parent_paintable,
        position.insert_before_paintable,
        callbacks,
        sink,
        &mut scopes,
    );
    unsafe {
        (sink.finish_commit)(sink.context);
    }
}
