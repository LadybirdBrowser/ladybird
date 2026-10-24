/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries script APIs call for an element's geometry: its boxes and client rects, the rects of a text range and of
//! a caret, and what an intersection observer computes. Each spends the one forced read of the script call that
//! reached it, and answers from the rows the document's render state published, which the host asks for again only
//! where it wrote them since.

use super::FfiCssPixelRect;
use super::node_data::NodeSlotId;
use super::used_values::FfiCssPixelSize;
use crate::css::css_pixels::CssPixelRect;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::style::tree::StyleNodeID;
use crate::layout::text_queries::FfiDomTextRange;
use crate::painting::ffi::{
    FfiBoxModelMetrics, FfiCaretRectResult, FfiEmptyLineCaretRect, FfiOptionalCssPixelRect, FfiRectToViewportTransform,
};
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
use crate::render_state::{DocumentHost, ScriptForcedRead};
use std::ffi::c_void;

/// Mints the forced read of a script call that reaches the host through one of this module's entries.
pub(crate) struct ScriptEntry {
    _private: (),
}

const SCRIPT_ENTRY: ScriptEntry = ScriptEntry { _private: () };

/// Answers `read` from the rows of `host`'s document as of every write the host made, spending the script call's
/// forced read. Every entry here is called with a live document host, on its document's thread.
fn read_rows<R>(host: &DocumentHost, read: impl FnOnce(&PaintSource<'_>) -> R) -> R {
    host.read_rows(ScriptForcedRead::at_script_entry(&SCRIPT_ENTRY), false, read)
}

/// # Safety
///
/// The callback must be sound to call with `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_client_rects(
    host: &DocumentHost,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: FfiRectToViewportTransform<'_>,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    read_rows(host, |arena| {
        let rect_to_viewport_transform = rect_to_viewport_transform.transform();
        crate::painting::client_rects::for_each_client_rect(
            arena,
            layout_node,
            rect_to_viewport_transform.as_ref(),
            |rect| {
                // SAFETY: The consumer copies the plain-data rect synchronously.
                unsafe { push_rect(context, rect.into()) };
            },
        );
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_bounding_client_rect(
    host: &DocumentHost,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: FfiRectToViewportTransform<'_>,
    out: &mut FfiCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        let rect_to_viewport_transform = rect_to_viewport_transform.transform();
        crate::painting::client_rects::bounding_client_rect(arena, layout_node, rect_to_viewport_transform.as_ref())
            .into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_transform_subtree_is_clipped_outside(
    host: &DocumentHost,
    target: NodeSlotId,
    root_bounds: FfiCssPixelRect,
    rect_to_viewport_transform: FfiRectToViewportTransform<'_>,
) -> bool {
    read_rows(host, |arena| {
        let rect_to_viewport_transform = rect_to_viewport_transform.transform();
        crate::painting::intersection_observer::transform_subtree_is_clipped_outside(
            arena,
            target,
            root_bounds.into(),
            rect_to_viewport_transform.as_ref(),
        )
    })
}

/// # Safety
///
/// The callback must be sound to call with `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_intersection_observer_intersection_rect(
    host: &DocumentHost,
    target: NodeSlotId,
    target_rect: FfiCssPixelRect,
    intersection_root: NodeSlotId,
    root_bounds: FfiCssPixelRect,
    rect_to_viewport_transform: FfiRectToViewportTransform<'_>,
    context: *mut c_void,
    inflate_scroll_container_clip_rect_by_scroll_margin: unsafe extern "C" fn(
        *mut c_void,
        FfiCssPixelRect,
    ) -> FfiCssPixelRect,
    out: &mut FfiCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        let rect_to_viewport_transform = rect_to_viewport_transform.transform();
        crate::painting::intersection_observer::intersection_rect(
            arena,
            target,
            target_rect.into(),
            intersection_root,
            root_bounds.into(),
            rect_to_viewport_transform.as_ref(),
            |clip_rect: CssPixelRect| -> CssPixelRect {
                // SAFETY: The callback copies the plain-data rect synchronously.
                unsafe { inflate_scroll_container_clip_rect_by_scroll_margin(context, clip_rect.into()) }.into()
            },
        )
        .into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_can_compute_client_rects_without_visual_context_update(
    host: &DocumentHost,
    layout_node: NodeSlotId,
    viewport_scroll_offset_is_zero: bool,
) -> bool {
    read_rows(host, |arena| {
        crate::painting::client_rects::can_compute_client_rects_without_visual_context_update(
            arena,
            layout_node,
            viewport_scroll_offset_is_zero,
        )
    })
}

/// # Safety
///
/// The callback must be sound to call with `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_text_range_rects(
    host: &DocumentHost,
    primary: NodeSlotId,
    selection_state: u8,
    range_start_offset: usize,
    range_end_offset: usize,
    filter_dom_start: usize,
    filter_dom_end: usize,
    rect_to_viewport_transform: FfiRectToViewportTransform<'_>,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    read_rows(host, |arena| {
        let paintable_rows = arena;
        let rect_to_viewport_transform = rect_to_viewport_transform.transform();

        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        crate::painting::text_fragment::for_each_fragment_of_nodes(paintable_rows, node_slots, |block, _, fragment| {
            let fragment_dom_start = fragment.dom_start_offset_in_node;
            let fragment_dom_end = fragment.dom_end_offset_in_node;
            if fragment_dom_end <= filter_dom_start || fragment_dom_start >= filter_dom_end {
                return true;
            }

            let rect = crate::painting::text_fragment::range_rect(
                paintable_rows,
                fragment,
                selection_state,
                range_start_offset,
                range_end_offset,
            );

            let rect_in_viewport_space = if arena.slot_is_live(block) {
                crate::painting::rect_to_viewport_transform::transform_rect_to_viewport_or_identity(
                    rect_to_viewport_transform.as_ref(),
                    paintable_rows,
                    block,
                    rect,
                )
            } else {
                rect
            };

            // SAFETY: The consumer copies the plain-data rect synchronously.
            unsafe { push_rect(context, rect_in_viewport_space.into()) };
            true
        });
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_text_caret_rect_in_dom_range(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
    out: &mut FfiOptionalCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        match crate::painting::caret::caret_rect_in_dom_range(paintable_rows, node_slots, offset) {
            Some(rect) => FfiOptionalCssPixelRect {
                has_value: true,
                rect: rect.into(),
            },
            None => FfiOptionalCssPixelRect {
                has_value: false,
                rect: FfiCssPixelRect::default(),
            },
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_text_caret_rect_for_position(
    host: &DocumentHost,
    primary: NodeSlotId,
    offset: usize,
    affinity_is_downstream: bool,
    out: &mut FfiCaretRectResult,
) {
    let mut result = FfiCaretRectResult {
        found: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
        owner_paintable: NodeSlotId::INVALID,
        nearest_self_painting_inline: NodeSlotId::INVALID,
    };
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        let Some(answer) =
            crate::painting::caret::caret_rect_for_position(paintable_rows, node_slots, offset, affinity_is_downstream)
        else {
            return result;
        };
        result.found = true;
        result.rect = answer.rect.into();
        result.style_source = answer.style_source;
        result.owner_paintable = answer.owner;
        result.nearest_self_painting_inline =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(paintable_rows, answer.node)
                .unwrap_or(NodeSlotId::INVALID);
        result
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_atomic_inline_caret_rect_for_position(
    host: &DocumentHost,
    primary: NodeSlotId,
    after: bool,
    out: &mut FfiCaretRectResult,
) {
    let mut result = FfiCaretRectResult {
        found: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
        owner_paintable: NodeSlotId::INVALID,
        nearest_self_painting_inline: NodeSlotId::INVALID,
    };
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        let Some(answer) = crate::painting::caret::caret_rect_for_atomic_inline(paintable_rows, primary, after) else {
            return result;
        };
        result.found = true;
        result.rect = answer.rect.into();
        result.style_source = answer.style_source;
        result.owner_paintable = answer.owner;
        result.nearest_self_painting_inline =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(paintable_rows, answer.node)
                .unwrap_or(NodeSlotId::INVALID);
        result
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_empty_line_caret_rect(
    host: &DocumentHost,
    block: NodeSlotId,
    primary: NodeSlotId,
    offset: usize,
    out: &mut FfiEmptyLineCaretRect,
) {
    let mut result = FfiEmptyLineCaretRect {
        has_value: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
    };
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(block) {
            return result;
        }
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        let side = arena.committed_side_data(block);
        let Some(first_fragment) = side.fragments().first() else {
            return result;
        };
        if !node_slots.contains(&first_fragment.layout_node) {
            return result;
        }
        for target in crate::painting::visual_lines::empty_line_caret_targets(paintable_rows, block) {
            if target.offset == offset {
                result.has_value = true;
                result.rect = target.rect.into();
                result.style_source = crate::painting::text_fragment::style_source(paintable_rows, first_fragment);
                break;
            }
        }
        result
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_absolute_rect(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        crate::painting::paintable_geometry::absolute_rect_or_default(arena, slot).into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_absolute_padding_box_rect(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::first_fragment_padding_box_rect(paintable_rows, slot).into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_absolute_border_box_rect(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::first_fragment_border_box_rect(paintable_rows, slot).into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_border_box_rect_of_fragments(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelRect,
) {
    *out = read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_border_box_rect(arena, slot).into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_box_fragment_count(host: &DocumentHost, slot: NodeSlotId) -> usize {
    read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(slot) {
            return 0;
        }
        crate::painting::paintable_geometry::fragment_count(arena, slot)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_unbroken_border_box_size(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelSize,
) {
    *out = read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(slot) {
            return FfiCssPixelSize::default();
        }
        crate::painting::paintable_geometry::unbroken_border_box_size(arena, slot)
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_empty_line_ended_by_forced_break_position(
    host: &DocumentHost,
    block: NodeSlotId,
    forced_break: NodeSlotId,
    out: &mut super::used_values::FfiCssPixelPoint,
) -> bool {
    let position = read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(block) {
            return None;
        }
        let side = arena.committed_side_data(block);
        let line = side
            .lines()
            .iter()
            .find(|line| line.forced_break_node == forced_break && line.fragment_count == 0)?;
        let root_position =
            crate::painting::paintable_geometry::LineRootPositions::new(arena, block).of_fragment(line.box_fragment);
        Some(CssPixelRect::from(line.rect).translated_by(root_position).location())
    });
    let Some(position) = position else {
        return false;
    };
    *out = position.into();
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_box_model(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiBoxModelMetrics,
) {
    *out = read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(slot) {
            return FfiBoxModelMetrics::default();
        }
        let (margin, padding, border) = crate::painting::paintable_geometry::unbroken_box_model(arena, slot);
        FfiBoxModelMetrics {
            margin,
            padding,
            border,
            inset: crate::painting::paintable_geometry::committed_inset(arena, slot),
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_paintable_content_size(
    host: &DocumentHost,
    slot: NodeSlotId,
    out: &mut FfiCssPixelSize,
) {
    *out = read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelSize::default();
        }
        crate::painting::paintable_geometry::unbroken_content_size(paintable_rows, slot)
    });
}

/// Answers `read` of the layout arena of `host`'s document, spending the script call's forced read.
fn read_arena<R>(host: &DocumentHost, read: impl FnOnce(&mut crate::layout::LayoutNodeArena) -> R) -> R {
    host.ask(ScriptForcedRead::at_script_entry(&SCRIPT_ENTRY), |state| {
        read(state.arena_mut())
    })
}

#[repr(C)]
pub struct FfiRenderedTextView {
    pub text: *const u16,
    pub length_in_code_units: usize,
}

#[repr(C)]
pub struct FfiRenderedTextOffset {
    pub dom_start_offset: usize,
    pub dom_end_offset: usize,
}

#[repr(C)]
pub struct FfiRenderedTextWithOffsetsView {
    pub text: *const u16,
    pub length_in_code_units: usize,
    pub offset_mapping: *const FfiRenderedTextOffset,
    pub offset_mapping_length: usize,
}

/// The text the rows of the text node whose primary row is `primary` render, handed to `append`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, the primary and its fragments live text rows with
/// styled parents, and `append` must copy the view synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_rendered_text(
    host: &DocumentHost,
    primary: NodeSlotId,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, FfiRenderedTextView),
) {
    // The rows carry the text each text row renders, unless the row waits for its text to be rendered again, which only
    // the render state does.
    let from_rows = host.read_rows_with_rendered_text(
        ScriptForcedRead::at_script_entry(&SCRIPT_ENTRY),
        |rows, awaiting_render| crate::layout::text_queries::rendered_text_of_rows(rows, awaiting_render, primary),
    );
    let text = match from_rows {
        Some(text) => text,
        None => read_arena(host, |arena| crate::layout::text_queries::rendered_text(arena, primary)),
    };
    let view = FfiRenderedTextView {
        text: text.as_ptr(),
        length_in_code_units: text.len(),
    };
    // SAFETY: The text outlives the call, which copies it.
    unsafe { append(context, view) };
}

/// The rendered text and its rendered-to-DOM UTF-16 offset mapping for a text row.
///
/// # Safety
///
/// `host` must be a live document host on its document's thread, `primary` must be a live text row, and `append` must
/// copy both views synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_rendered_text_with_dom_offsets(
    host: &DocumentHost,
    primary: NodeSlotId,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, FfiRenderedTextWithOffsetsView),
) {
    let (text, offsets) = read_arena(host, |arena| {
        crate::layout::text_queries::rendered_text_with_dom_offsets(arena, primary)
    });
    let offset_mapping: Vec<_> = offsets
        .into_iter()
        .map(|(dom_start_offset, dom_end_offset)| FfiRenderedTextOffset {
            dom_start_offset,
            dom_end_offset,
        })
        .collect();
    let view = FfiRenderedTextWithOffsetsView {
        text: text.as_ptr(),
        length_in_code_units: text.len(),
        offset_mapping: offset_mapping.as_ptr(),
        offset_mapping_length: offset_mapping.len(),
    };
    // SAFETY: Both vectors outlive the call, which copies the views.
    unsafe { append(context, view) };
}

/// The text the pseudo-element of kind `generated_for` on the element `style_node` resolved its generated content to
/// when its box was built, as a raw `AK::Utf16String` representation for which the caller takes one reference.
#[unsafe(no_mangle)]
pub extern "C" fn layout_script_generated_content_accessible_text(
    host: &DocumentHost,
    style_node: u32,
    generated_for: u8,
) -> usize {
    let text = match StyleNodeID::from_raw(style_node) {
        Some(element) => read_arena(host, |arena| {
            let owner = crate::layout::counters::CounterOwner { element, generated_for };
            arena.generated_content().borrow().accessible_text(owner).to_vec()
        }),
        None => Vec::new(),
    };
    ak::Utf16String::from_utf16(&text).into_raw()
}

/// Where `query` occurs in the searchable text below `viewport`, each match handed to `append`. The text of a DOM
/// text node `is_searchable` turns down is not searched.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, the query must stay readable during the call, and
/// the callbacks may read the DOM but must not change it or the layout tree.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_find_matching_text(
    host: &DocumentHost,
    viewport: NodeSlotId,
    query: FfiUtf16View,
    case_sensitive: bool,
    is_searchable: unsafe extern "C" fn(*mut c_void, u32) -> bool,
    context: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, FfiDomTextRange),
) {
    // SAFETY: The host lends the query for this synchronous call.
    let query = query.to_utf16().expect("query carries no storage");
    if query.is_empty() {
        return;
    }
    let candidates = read_arena(host, |arena| {
        crate::layout::text_queries::search_candidates(arena, viewport)
    });
    let mut excluded: Vec<StyleNodeID> = candidates
        .into_iter()
        // SAFETY: The callback only reads whether the DOM text node is searchable.
        .filter(|text| !unsafe { is_searchable(context, text.raw()) })
        .collect();
    excluded.sort_unstable();
    let matches = read_arena(host, |arena| {
        crate::layout::text_queries::find_matching_text(arena, viewport, &query, case_sensitive, &excluded)
    });
    for range in matches {
        // SAFETY: The host resolves each identity's DOM node itself, synchronously.
        unsafe { append(context, range) };
    }
}

/// A min or max sizing property, whose anchor-size() functions the abspos engine resolves.
#[repr(u8)]
#[derive(Clone, Copy)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiAnchorSizedProperty {
    MinWidth,
    MinHeight,
    MaxWidth,
    MaxHeight,
}

/// Writes the size `property` of the absolutely positioned box `slot` resolved to in the committed layout to `out`,
/// when its anchor-size() functions resolved against an anchor box.
#[unsafe(no_mangle)]
pub extern "C" fn layout_script_anchor_resolved_size(
    host: &DocumentHost,
    slot: NodeSlotId,
    property: FfiAnchorSizedProperty,
    out: &mut crate::css::css_pixels::CssPixels,
) -> bool {
    use super::style_values::ResolvedSizeOverride;
    let resolved = read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(slot) {
            return None;
        }
        arena.with_committed_fragment_link(slot, |link| {
            let link = link?;
            let sizes = link.abspos_layout_inputs.as_ref()?.resolved_anchor_sizes;
            // NB: The sizes lie in the frame the box was laid out in, whose axes a vertical writing mode transposes.
            let (physical_width, physical_height) = if link.frame.is_transposed() {
                ((sizes.min_height, sizes.max_height), (sizes.min_width, sizes.max_width))
            } else {
                ((sizes.min_width, sizes.max_width), (sizes.min_height, sizes.max_height))
            };
            match property {
                FfiAnchorSizedProperty::MinWidth => physical_width.0,
                FfiAnchorSizedProperty::MaxWidth => physical_width.1,
                FfiAnchorSizedProperty::MinHeight => physical_height.0,
                FfiAnchorSizedProperty::MaxHeight => physical_height.1,
            }
        })
    });
    // NB: A size resolved to a keyword holds an anchor function with neither an anchor box nor a fallback value, which
    //     makes the declaration invalid at computed-value time however its other functions resolve.
    let Some(ResolvedSizeOverride::Length(px)) = resolved else {
        return false;
    };
    *out = px;
    true
}
