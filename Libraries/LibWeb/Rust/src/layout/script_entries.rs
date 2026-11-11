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
use super::query::inline_cursor::{self, CaretPosition, InlineCursor};
use super::used_values::{FfiCssPixelPoint, FfiCssPixelSize};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::style::tree::StyleNodeID;
use crate::layout::text_queries::FfiDomTextRange;
use crate::painting::ffi::FfiBoxModelMetrics;
use crate::painting::host::FfiCaretPaintKind;
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintRow, PaintSource};
use crate::painting::viewport_mapping::{FfiViewportMapping, ViewportSpace};
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
    viewport_mapping: FfiViewportMapping<'_>,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    let viewport_mapping = viewport_mapping.resolve();
    read_rows(host, |arena| {
        crate::painting::client_rects::for_each_client_rect(arena, layout_node, &viewport_mapping.mapping(), |rect| {
            // SAFETY: The consumer copies the plain-data rect synchronously.
            unsafe { push_rect(context, rect.into()) };
        });
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_bounding_client_rect(
    host: &DocumentHost,
    layout_node: NodeSlotId,
    viewport_mapping: FfiViewportMapping<'_>,
    out: &mut FfiCssPixelRect,
) {
    let viewport_mapping = viewport_mapping.resolve();
    *out = read_rows(host, |arena| {
        crate::painting::client_rects::bounding_client_rect(arena, layout_node, &viewport_mapping.mapping()).into()
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_transform_subtree_is_clipped_outside(
    host: &DocumentHost,
    target: NodeSlotId,
    root_bounds: FfiCssPixelRect,
    viewport_mapping: FfiViewportMapping<'_>,
) -> bool {
    let viewport_mapping = viewport_mapping.resolve();
    read_rows(host, |arena| {
        crate::painting::intersection_observer::transform_subtree_is_clipped_outside(
            arena,
            target,
            root_bounds.into(),
            &viewport_mapping.mapping(),
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
    viewport_mapping: FfiViewportMapping<'_>,
    context: *mut c_void,
    inflate_scroll_container_clip_rect_by_scroll_margin: unsafe extern "C" fn(
        *mut c_void,
        FfiCssPixelRect,
    ) -> FfiCssPixelRect,
    out: &mut FfiCssPixelRect,
) {
    let viewport_mapping = viewport_mapping.resolve();
    *out = read_rows(host, |arena| {
        crate::painting::intersection_observer::intersection_rect(
            arena,
            target,
            target_rect.into(),
            intersection_root,
            root_bounds.into(),
            &viewport_mapping.mapping(),
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
    viewport_mapping: FfiViewportMapping<'_>,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    let viewport_mapping = viewport_mapping.resolve();
    read_rows(host, |arena| {
        let paintable_rows = arena;
        let viewport_mapping = viewport_mapping.mapping();

        // A collapsed range has the rect of the caret it is in text, with no width.
        if selection_state == crate::painting::paintable_data::SELECTION_STATE_START_AND_END
            && range_start_offset == range_end_offset
        {
            let cursor = InlineCursor::new(arena);
            if let Some(position) = cursor.line_of(primary, range_start_offset, true)
                && let Some(index) = position.fragment
            {
                let root = position.line.root;
                let side = arena.committed_side_data(root);
                let fragment = &side.fragments()[index as usize];
                let mut rect = cursor
                    .caret_rect_in_fragment(root, fragment, range_start_offset, true)
                    .rect;
                if crate::painting::text_fragment::is_horizontal(fragment) {
                    rect.width = CssPixels::default();
                } else {
                    rect.height = CssPixels::default();
                }
                let rect = viewport_mapping.line_content_rect_to_viewport(
                    arena,
                    root,
                    fragment.layout_node,
                    rect,
                    ViewportSpace::LayoutViewport,
                );
                // SAFETY: The consumer copies the plain-data rect synchronously.
                unsafe { push_rect(context, rect.into()) };
            }
            return;
        }

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
                viewport_mapping.line_content_rect_to_viewport(
                    paintable_rows,
                    block,
                    fragment.layout_node,
                    rect,
                    ViewportSpace::LayoutViewport,
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

/// Calls `push_source` with what the character geometry of the text node `primary` is computed from: for each of its
/// layout nodes, a retained reference to its containing block's line content, and the raw document-absolute position
/// of that block. The same references at the same positions mean the same geometry. The consumer owns every reference
/// pushed, and releases each one with layout_release_inline_content(). Returns false when the geometry depends on more
/// than this: when a block is fragmented, or holds no line content.
///
/// # Safety
///
/// The callback must be sound to call with `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_text_geometry_sources(
    host: &DocumentHost,
    primary: NodeSlotId,
    context: *mut c_void,
    push_source: unsafe extern "C" fn(*mut c_void, *const c_void, i32, i32),
) -> bool {
    read_rows(host, |arena| {
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        if node_slots.is_empty() {
            return false;
        }
        for &node in node_slots {
            let Some(block) = crate::painting::text_fragment::containing_block_paintable_of_node(arena, node) else {
                return false;
            };
            let side_data = arena.committed_side_data(block);
            let Some(content) = side_data.inline_content.as_ref() else {
                return false;
            };
            // Lines in different fragments of a block can each move on their own.
            if side_data.box_fragments().len() > 1 {
                return false;
            }
            let position = crate::painting::paintable_geometry::absolute_line_root_position(arena, block, 0);
            let retained = std::sync::Arc::into_raw(content.clone()).cast::<c_void>();
            // SAFETY: The consumer takes ownership of the reference, and copies the position synchronously.
            unsafe { push_source(context, retained, position.x.raw_value(), position.y.raw_value()) };
        }
        true
    })
}

/// Releases a reference to line content that layout_script_text_geometry_sources() handed out.
///
/// # Safety
///
/// `content` must be a reference layout_script_text_geometry_sources() pushed, released only once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_release_inline_content(content: *const c_void) {
    if content.is_null() {
        return;
    }
    // SAFETY: The caller passes a reference from Arc::into_raw() in layout_script_text_geometry_sources(), once.
    drop(unsafe { std::sync::Arc::from_raw(content.cast::<crate::layout::inline_content::InlineContent>()) });
}

/// Calls `push_position` with each DOM offset (in UTF-16 code units) of the text node `primary` that one of its
/// horizontal fragments renders, along with the document-absolute x coordinate where that offset's character starts,
/// read from the fragment's shaped glyph run. `is_fragment_end` marks the offset one past a fragment's last character,
/// whose x is where that fragment's text ends.
///
/// # Safety
///
/// The callback must be sound to call with `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_text_character_x_positions(
    host: &DocumentHost,
    primary: NodeSlotId,
    context: *mut c_void,
    push_position: unsafe extern "C" fn(*mut c_void, usize, f32, bool),
) {
    read_rows(host, |arena| {
        let fragments = arena.text_fragments(primary);
        crate::painting::text_fragment::for_each_fragment_of_nodes(arena, fragments.as_slice(), |_, _, fragment| {
            let end = fragment.dom_end_offset_in_node;
            crate::painting::text_fragment::for_each_dom_offset_x(arena, fragment, |dom_offset, x| {
                // SAFETY: The consumer copies the plain-data position synchronously.
                unsafe { push_position(context, dom_offset, x, dom_offset == end) };
            });
            true
        });
    });
}

/// A caret position as the host names it: `offset` in the DOM text whose layout node is `node`, or, where `node` is the
/// box of an element, the position between the children of the element whose boxes are `child_before` and
/// `child_after` (`NodeSlotId::INVALID` for none), where the DOM text of `child_before` is `child_before_length` long.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCaretPosition {
    pub node: NodeSlotId,
    pub offset: usize,
    pub downstream: bool,
    pub child_before: NodeSlotId,
    pub child_before_length: usize,
    pub child_after: NodeSlotId,
}

impl FfiCaretPosition {
    /// The caret positions the position can take, in the order to try them.
    pub(crate) fn candidates(&self, arena: &impl PaintRead) -> impl Iterator<Item = CaretPosition> {
        let is_text = arena
            .node_kind_if_live(self.node)
            .is_some_and(super::node_facts::kind_is_text);
        let text = is_text.then_some(CaretPosition::Text {
            text: self.node,
            offset: self.offset,
            downstream: self.downstream,
        });
        let between_children = (!is_text)
            .then(|| {
                inline_cursor::caret_positions_between_children(
                    arena,
                    self.node,
                    self.child_before,
                    self.child_before_length,
                    self.child_after,
                )
            })
            .into_iter()
            .flatten();
        text.into_iter().chain(between_children)
    }
}

/// The caret layout gives a position, as the recorder paints it and in the layout viewport.
#[repr(C)]
pub struct FfiCaret {
    pub found: bool,
    pub paint: FfiCaretPaintKind,
    pub block: NodeSlotId,
    pub owner: NodeSlotId,
    pub rect: FfiCssPixelRect,
    pub viewport_rect: FfiCssPixelRect,
    pub style_source: NodeSlotId,
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_script_caret(
    host: &DocumentHost,
    position: FfiCaretPosition,
    viewport_mapping: FfiViewportMapping<'_>,
    out: &mut FfiCaret,
) {
    let viewport_mapping = viewport_mapping.resolve();
    *out = read_rows(host, |arena| {
        let cursor = InlineCursor::new(arena);
        let Some(caret) = position
            .candidates(arena)
            .find_map(|candidate| cursor.caret_rect(candidate))
        else {
            return FfiCaret {
                found: false,
                paint: FfiCaretPaintKind::None,
                block: NodeSlotId::INVALID,
                owner: NodeSlotId::INVALID,
                rect: FfiCssPixelRect::default(),
                viewport_rect: FfiCssPixelRect::default(),
                style_source: NodeSlotId::INVALID,
            };
        };
        // A box with no line content around the caret paints the caret itself.
        let at_start_of_box = caret.owner == caret.node;
        FfiCaret {
            found: true,
            paint: if at_start_of_box && crate::painting::node_painting::is_fragmented_inline(arena, caret.owner) {
                FfiCaretPaintKind::EmptyInline
            } else {
                FfiCaretPaintKind::InBlock
            },
            block: caret.owner,
            owner: crate::painting::fragment_ownership::nearest_self_painting_inline_box(arena, caret.node)
                .filter(|_| !at_start_of_box)
                .unwrap_or(NodeSlotId::INVALID),
            rect: caret.rect.into(),
            viewport_rect: viewport_mapping
                .mapping()
                .line_content_rect_to_viewport(
                    arena,
                    caret.owner,
                    caret.node,
                    caret.rect,
                    ViewportSpace::LayoutViewport,
                )
                .into(),
            style_source: caret.style_source,
        }
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

/// Whether the box or one of its ancestors is sticky positioned, which moves the box by an offset that depends on the
/// scroll position.
#[unsafe(no_mangle)]
pub extern "C" fn layout_script_has_sticky_positioned_inclusive_ancestor(
    host: &DocumentHost,
    slot: NodeSlotId,
) -> bool {
    read_rows(host, |arena| {
        let mut node = slot;
        while let Some(row) = arena.node(node) {
            if let Some(style) = row.style()
                && crate::painting::style_queries::is_sticky_position(style)
            {
                return true;
            }
            node = row.parent();
        }
        false
    })
}

/// The offset by which sticky positioning moves the box, as the sum of the offsets of the sticky positioned box and
/// its sticky positioned ancestors.
#[unsafe(no_mangle)]
pub extern "C" fn layout_script_accumulated_sticky_offset(
    host: &DocumentHost,
    slot: NodeSlotId,
    viewport_mapping: FfiViewportMapping<'_>,
    out: &mut FfiCssPixelPoint,
) {
    let viewport_mapping = viewport_mapping.resolve();
    *out = read_rows(host, |arena| viewport_mapping.mapping().sticky_offset(arena, slot)).into();
}

/// The offset by which scrolling, sticky positioning and anchor scroll shifts move the box, without its transforms.
#[unsafe(no_mangle)]
pub extern "C" fn layout_script_offset_to_viewport_ignoring_transforms(
    host: &DocumentHost,
    slot: NodeSlotId,
    viewport_mapping: FfiViewportMapping<'_>,
    out: &mut FfiCssPixelPoint,
) {
    let viewport_mapping = viewport_mapping.resolve();
    *out = read_rows(host, |arena| {
        viewport_mapping
            .mapping()
            .offset_to_viewport_ignoring_transforms(arena, slot)
    })
    .into();
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

/// Whether the forced break `forced_break` ends an empty line of `block`.
#[unsafe(no_mangle)]
pub extern "C" fn layout_script_forced_break_ends_empty_line(
    host: &DocumentHost,
    block: NodeSlotId,
    forced_break: NodeSlotId,
) -> bool {
    read_rows(host, |arena| {
        arena.paintable_row_is_populated(block)
            && arena.committed_side_data(block).lines().iter().any(|line| {
                line.fragment_count == 0 && line.forced_break.is_some_and(|position| position.node == forced_break)
            })
    })
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
            arena.generated_content().accessible_text(owner).to_vec()
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
            let inputs = link.placement.abspos_layout_inputs.as_ref()?;
            let sizes = inputs.resolved_anchor_sizes;
            // NB: The sizes lie in the frame the box was laid out in, whose axes a vertical writing mode transposes.
            let (physical_width, physical_height) = if inputs.frame.is_transposed() {
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
