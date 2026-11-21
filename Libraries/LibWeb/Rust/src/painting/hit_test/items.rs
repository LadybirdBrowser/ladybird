/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The hit-test items of one box in one paint phase, in the order they stack, read from rows.

use super::*;
use crate::css::computed_value_types::SVG_PAINT_NONE;
use crate::css::css_enums;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{DomPaintFact, NodeKind, NodeSlotId};
use crate::layout::node_facts::{self, NodeShape};
use crate::painting::display_list::commands::ContextRef;
use crate::painting::fragment_ownership;
use crate::painting::node_painting;
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::paintable_data::CommittedSideData;
use crate::painting::paintable_data::*;
use crate::painting::paintable_geometry;
use crate::painting::record::PaintPhase;
use crate::painting::style_queries;
use crate::painting::text_fragment;
use libgfx_rust::{FloatPoint, WindingRule};

/// A fragment of a row's inline content, read through whatever holds the row's committed side
/// data.
struct CommittedFragment<SideData> {
    side_data: SideData,
    index: usize,
}

impl<SideData: std::ops::Deref<Target = CommittedSideData>> std::ops::Deref for CommittedFragment<SideData> {
    type Target = FragmentRecord;

    fn deref(&self) -> &FragmentRecord {
        &self.side_data.fragments()[self.index]
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct HitTestFacts {
    pub(crate) visible_for_hit_testing: bool,
    pub(crate) has_resizer: bool,
    pub(crate) could_be_scrolled_horizontally: bool,
    pub(crate) could_be_scrolled_vertically: bool,
    pub(crate) svg_hit_areas: u8,
    pub(crate) svg_path_winding_rule: WindingRule,
}

// https://svgwg.org/svg2-draft/interact.html#PointerEventsProperty
fn svg_hit_areas(pointer_events: u8, visibility_is_visible: bool, fill_is_none: bool, stroke_is_none: bool) -> u8 {
    use css_enums::pointer_events;
    let fill_and_stroke = svg_hit_area::FILL | svg_hit_area::STROKE;
    // The pointer is over a painted area if it is over the interior (i.e., fill) of the element and the fill property
    // has an actual value other than none or it is over the perimeter (i.e., stroke) of the element and the stroke
    // property has an actual value other than none.
    let painted_areas =
        (if fill_is_none { 0 } else { svg_hit_area::FILL }) | (if stroke_is_none { 0 } else { svg_hit_area::STROKE });
    let if_visible = |areas: u8| if visibility_is_visible { areas } else { 0 };
    match pointer_events {
        // auto: The default behavior, which is the same as for visiblePainted.
        // visiblePainted: The given element can be the target element for pointer events when the visibility property
        //                 is set to visible and when the pointer is over a "painted" area.
        pointer_events::AUTO | pointer_events::VISIBLEPAINTED => if_visible(painted_areas),
        // bounding-box: The given element can be a target element for pointer events when the pointer is over the
        //               bounding box of the element.
        pointer_events::BOUNDING_BOX => svg_hit_area::BOUNDING_BOX,
        // visibleFill: The given element can be the target element for pointer events when the visibility property is
        //              set to visible and when the pointer is over the interior (i.e., fill) of the element.
        pointer_events::VISIBLEFILL => if_visible(svg_hit_area::FILL),
        // visibleStroke: The given element can be the target element for pointer events when the visibility property
        //                is set to visible and when the pointer is over the perimeter (i.e., stroke) of the element.
        pointer_events::VISIBLESTROKE => if_visible(svg_hit_area::STROKE),
        // visible: The given element can be the target element for pointer events when the visibility property is set
        //          to visible and the pointer is over either the interior (i.e., fill) or the perimeter (i.e.,
        //          stroke) of the element.
        pointer_events::VISIBLE => if_visible(fill_and_stroke),
        // painted: The given element can be the target element for pointer events when the pointer is over a "painted"
        //          area.
        pointer_events::PAINTED => painted_areas,
        // fill: The given element can be the target element for pointer events when the pointer is over the interior
        //       (i.e., fill) of the element.
        pointer_events::FILL => svg_hit_area::FILL,
        // stroke: The given element can be the target element for pointer events when the pointer is over the
        //         perimeter (i.e., stroke) of the element.
        pointer_events::STROKE => svg_hit_area::STROKE,
        // all: The given element can be the target element for pointer events whenever the pointer is over either the
        //      interior (i.e., fill) or the perimeter (i.e., stroke) of the element.
        pointer_events::ALL => fill_and_stroke,
        // none: The given element does not receive pointer events.
        _ => 0,
    }
}

pub(crate) fn hit_test_facts(arena: &impl PaintRead, paintable: NodeSlotId) -> HitTestFacts {
    let Some(row) = arena.node(paintable) else {
        return HitTestFacts::default();
    };
    let Some(style) = row.style() else {
        return HitTestFacts::default();
    };
    let wheel_axes = crate::painting::chrome_geometry::wheel_scrollable_axes(arena, paintable);
    let svg_path = node_painting::is_svg_path(row.kind());
    let svg = style.inherited_svg();
    HitTestFacts {
        visible_for_hit_testing: arena.paintable_row_is_populated(paintable)
            && row.dom_paint_facts() & DomPaintFact::Inert as u8 == 0
            && style.pointer_events() != css_enums::pointer_events::NONE,
        has_resizer: crate::painting::chrome_geometry::has_resizer(arena, paintable),
        could_be_scrolled_horizontally: wheel_axes.horizontal,
        could_be_scrolled_vertically: wheel_axes.vertical,
        svg_hit_areas: if svg_path {
            let areas = svg_hit_areas(
                style.pointer_events(),
                style.visibility() == css_enums::visibility::VISIBLE,
                svg.fill.kind == SVG_PAINT_NONE,
                svg.stroke.kind == SVG_PAINT_NONE,
            );
            // For text elements, hit-testing is performed on a character cell basis, so the text receives events
            // anywhere within a character cell whenever its fill or its stroke would.
            if matches!(row.kind(), NodeKind::SVGTextBox | NodeKind::SVGTextPathBox)
                && areas & (svg_hit_area::FILL | svg_hit_area::STROKE) != 0
            {
                svg_hit_area::FILL
            } else {
                areas
            }
        } else if row.kind() == NodeKind::SVGImageBox {
            // For raster images, hit-testing is performed on a whole-image basis (i.e., the rectangular area for the
            // image is the determinant for whether the image receives the event).
            // NB: The rectangular area for the image is also its bounding box.
            if svg_hit_areas(
                style.pointer_events(),
                style.visibility() == css_enums::visibility::VISIBLE,
                false,
                false,
            ) != 0
            {
                svg_hit_area::BOUNDING_BOX
            } else {
                0
            }
        } else if row.kind() == NodeKind::SVGGraphicsBox
            && style.pointer_events() == css_enums::pointer_events::BOUNDING_BOX
        {
            svg_hit_area::BOUNDING_BOX
        } else {
            0
        },
        svg_path_winding_rule: if svg_path && svg.fill_rule == css_enums::fill_rule::EVENODD {
            WindingRule::EvenOdd
        } else {
            WindingRule::Nonzero
        },
    }
}

/// Where the items of a box go, with what generating them asks of whoever wants them.
pub(crate) trait HitTestItemSink {
    /// The facts of a row, which a sink that visits a row in several phases may keep.
    fn hit_test_facts(&mut self, paintable: NodeSlotId) -> HitTestFacts;

    /// The scale from the space of the paintable's own box to the screen, which a stroke that does not scale is
    /// measured against.
    fn own_accumulated_2d_scale(&self, paintable: NodeSlotId) -> libgfx_rust::FloatSize;

    /// Whether an item of this kind over this rect is worth building. The item of a text fragment resolves a font
    /// and measures the fragment, which a sink that only wants what lies under a point can save.
    fn wants(&mut self, _kind: HitTestItemKind, _rect: CssPixelRect, _context: ContextRef) -> bool {
        true
    }

    fn rejects_text_fragments_placed_at_their_line_root_before_building_them(&self) -> bool {
        false
    }

    fn misses_line(
        &mut self,
        _line_root: NodeSlotId,
        _line_index: u32,
        _line_root_absolute_content_origin: CssPixelPoint,
    ) -> bool {
        false
    }

    fn append(&mut self, item: HitTestItem);
}

pub(crate) struct HitTestItems<'a, R: PaintRead, S: HitTestItemSink> {
    rows: &'a R,
    sink: &'a mut S,
}

/// Hands `sink` the items `paintable` has in `phase`.
pub(crate) fn hit_test_items_of_phase<R: PaintRead, S: HitTestItemSink>(
    rows: &R,
    sink: &mut S,
    paintable: NodeSlotId,
    phase: PaintPhase,
) {
    HitTestItems { rows, sink }.of_phase(paintable, phase);
}

impl<'a, R: PaintRead, S: HitTestItemSink> HitTestItems<'a, R, S> {
    fn visibility_is_visible(&self, paintable: NodeSlotId) -> bool {
        self.rows
            .node_style_if_live(paintable)
            .is_none_or(|style| style.visibility() == css_enums::visibility::VISIBLE)
    }

    fn is_anonymous(&self, paintable: NodeSlotId) -> bool {
        style_queries::is_anonymous(self.rows, paintable)
    }

    fn is_generated_for_pseudo_element(&self, paintable: NodeSlotId) -> bool {
        self.rows.node_is_generated_for_pseudo_element(paintable)
    }

    fn of_phase(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if node_painting::is_fragmented_inline(self.rows, paintable) {
            self.inline_items(paintable, phase);
        } else if node_painting::has_lines(self.rows, paintable) {
            self.base_items(paintable, phase);
            self.lines_items(paintable, phase);
        } else if self
            .rows
            .node_kind_if_live(paintable)
            .is_some_and(node_painting::is_svg_path)
        {
            self.svg_path_items(paintable, phase);
        } else if self
            .rows
            .node_kind_if_live(paintable)
            .is_some_and(|kind| matches!(kind, NodeKind::SVGImageBox | NodeKind::SVGGraphicsBox))
        {
            self.svg_box_items(paintable, phase);
        } else {
            self.base_items(paintable, phase);
        }
    }

    fn base_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Background && phase != PaintPhase::Overlay {
            return;
        }
        if !self.visibility_is_visible(paintable) || !self.sink.hit_test_facts(paintable).visible_for_hit_testing {
            return;
        }
        if phase == PaintPhase::Background {
            if self.is_anonymous(paintable) && !self.is_generated_for_pseudo_element(paintable) {
                return;
            }
            // Column and column group boxes only exist to paint their backgrounds under the cells of their columns;
            // they are not hit-tested, as in other engines.
            let display = style_queries::display(self.rows, paintable);
            if display.is_table_column() || display.is_table_column_group() {
                return;
            }
            let context = self.rows.paintable_data(paintable).accumulated_visual_context;
            for (rect, radii) in
                crate::painting::border_radii::border_boxes_and_radii_of_fragments(self.rows, paintable)
            {
                self.append_box(paintable, rect, context, radii);
            }
            return;
        }
        let facts = self.sink.hit_test_facts(paintable);
        let context = self.rows.paintable_data(paintable).accumulated_visual_context;
        if facts.has_resizer {
            self.append_chrome_widget(paintable, CHROME_WIDGET_RESIZE_HANDLE, context);
        }
        if facts.could_be_scrolled_horizontally {
            self.append_chrome_widget(paintable, CHROME_WIDGET_HORIZONTAL_SCROLLBAR, context);
        }
        if facts.could_be_scrolled_vertically {
            self.append_chrome_widget(paintable, CHROME_WIDGET_VERTICAL_SCROLLBAR, context);
        }
    }

    fn lines_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        let fragment_count = self.rows.committed_side_data(paintable).fragments().len();
        if fragment_count == 0 && crate::painting::paint_order::first_paint_child(self.rows, paintable).is_none() {
            if style_queries::is_visible(self.rows, paintable)
                && self.sink.hit_test_facts(paintable).visible_for_hit_testing
            {
                self.empty_editable_item(paintable);
            }
            return;
        }
        let context = self
            .rows
            .paintable_data(paintable)
            .accumulated_visual_context_for_descendants;
        // Fragments inside self-painting inline boxes are recorded during that box's paint.
        let filter = fragment_ownership::effective_filter(self.rows, paintable);
        let line_root_positions = self
            .sink
            .rejects_text_fragments_placed_at_their_line_root_before_building_them()
            .then(|| paintable_geometry::LineRootPositions::new(self.rows, paintable));
        let side = self.rows.committed_side_data(paintable);
        let mut last_line_missed: Option<(u32, bool)> = None;
        filter.for_each_owned_fragment_index(fragment_count, |index| {
            let fragment = &side.fragments()[index];
            if fragment.is_atomic_inline {
                return;
            }
            if let Some(positions) = &line_root_positions {
                let line_root_origin = positions.of_line(side.lines(), fragment.line_index as usize);
                let line_missed = match last_line_missed {
                    Some((line_index, missed)) if line_index == fragment.line_index => missed,
                    _ => {
                        let missed = self.sink.misses_line(paintable, fragment.line_index, line_root_origin);
                        last_line_missed = Some((fragment.line_index, missed));
                        missed
                    }
                };
                if line_missed {
                    return;
                }
                let rect_placed_at_its_line_root =
                    text_fragment::rect_in_containing_block(fragment).translated_by(line_root_origin);
                if !self
                    .sink
                    .wants(HitTestItemKind::TextFragment, rect_placed_at_its_line_root, context)
                {
                    return;
                }
            }
            if text_fragment::is_block_level_box(self.rows, fragment) {
                return;
            }
            self.append_text_fragment(paintable, index as u32, context);
        });
    }

    fn inline_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if self.is_anonymous(paintable) && !self.is_generated_for_pseudo_element(paintable) {
            return;
        }
        // Only this box's own items are gated on its visibility; the fragments it owns may belong
        // to descendants that override visibility or pointer-events, so they are filtered per
        // fragment instead.
        let box_itself_is_hit_testable = style_queries::is_visible(self.rows, paintable)
            && self.sink.hit_test_facts(paintable).visible_for_hit_testing;

        if phase == PaintPhase::Background {
            if box_itself_is_hit_testable {
                self.append_piece_boxes(paintable);
            }
            return;
        }
        if phase != PaintPhase::Foreground {
            return;
        }

        if fragment_ownership::is_self_painting_inline(self.rows, paintable) {
            let Some(root) = self.inline_root(paintable) else {
                return;
            };
            let context = self
                .rows
                .paintable_data(paintable)
                .accumulated_visual_context_for_descendants;
            let fragment_count = self.rows.committed_side_data(root).fragments().len();
            let filter = fragment_ownership::effective_filter(self.rows, paintable);
            // Hit-test precedence follows paint order: this box's own text loses to the box itself
            // (re-recorded so its z-order matches this box's paint order), while nested content
            // (e.g. a link inside this box) wins over it.
            let own_layout_node = paintable;
            let mut nested_content_indices = Vec::new();
            filter.for_each_owned_fragment_index(fragment_count, |index| {
                if self.fragment_is_block_level_box(root, index) {
                    return;
                }
                let fragment_node = self.rows.committed_side_data(root).fragments()[index].layout_node;
                if fragment_ownership::nearest_fragmented_inline_ancestor(self.rows, fragment_node)
                    == Some(own_layout_node)
                {
                    self.append_text_fragment(root, index as u32, context);
                } else {
                    nested_content_indices.push(index);
                }
            });
            if box_itself_is_hit_testable && self.rows.paintable_data(paintable).establishes_stacking_context {
                self.append_piece_boxes(paintable);
            }
            for index in nested_content_indices {
                self.append_text_fragment(root, index as u32, context);
            }
        }

        // Clicks must be able to place the caret in an editable element with no content, so its
        // target is recorded in foreground order, winning over earlier-painted overlapping content.
        if box_itself_is_hit_testable && !self.inline_has_content(paintable) {
            self.empty_editable_item(paintable);
        }
    }

    fn inline_root(&self, paintable: NodeSlotId) -> Option<NodeSlotId> {
        let block = self.rows.paintable_data(paintable).containing_block;
        if block.is_invalid() || !self.rows.paintable_row_is_populated(block) {
            return None;
        }
        node_painting::has_lines(self.rows, block).then_some(block)
    }

    fn piece_of(&self, root: NodeSlotId, piece_index: u32) -> InlineBoxPieceRecord {
        self.rows.committed_side_data(root).inline_box_pieces()[piece_index as usize]
    }

    fn inline_has_content(&self, paintable: NodeSlotId) -> bool {
        let has_content_pieces = self.inline_root(paintable).is_some_and(|root| {
            self.rows
                .committed_side_data(root)
                .inline_box_piece_indices(paintable)
                .iter()
                .any(|piece_index| {
                    let piece = self.piece_of(root, *piece_index);
                    !piece.is_geometry_only_placeholder && piece.fragment_count != 0
                })
        });
        has_content_pieces || crate::painting::paint_order::first_paint_child(self.rows, paintable).is_some()
    }

    fn append_piece_boxes(&mut self, paintable: NodeSlotId) {
        let Some(root) = self.inline_root(paintable) else {
            return;
        };
        let root_positions = paintable_geometry::LineRootPositions::new(self.rows, root);
        let layout_arena = self.rows;
        let context = self.rows.paintable_data(paintable).accumulated_visual_context;
        let side = layout_arena.committed_side_data(root);
        for piece_index in side.inline_box_piece_indices(paintable) {
            let piece = &side.inline_box_pieces()[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let root_position = root_positions.of_line(side.lines(), piece.line_index as usize);
            let rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_position);
            let radii = crate::painting::border_radii::piece_border_radii(self.rows, paintable, piece);
            self.append_box(paintable, rect, context, radii);
        }
    }

    fn svg_path_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        let Some(mut path) = paintable_geometry::committed_svg_path(self.rows, paintable) else {
            return;
        };
        // OPTIMIZATION: The facts are computed afresh from the box's style each time they're asked for, so they're asked
        //               for once.
        let facts = self.sink.hit_test_facts(paintable);
        if !facts.visible_for_hit_testing {
            return;
        }
        // https://svgwg.org/svg2-draft/interact.html#PointerEventsProperty
        // For text elements, hit-testing is performed on a character cell basis
        let mut winding_rule = facts.svg_path_winding_rule;
        if let Some(cells_path) = self.svg_text_character_cells_path(paintable) {
            path = std::sync::Arc::new(crate::layout::fragment_tree::CommittedSvgPath::new(cells_path));
            // NB: Every point of a cell is in it, even where cells overlap, whatever the text's fill-rule.
            winding_rule = WindingRule::Nonzero;
        }
        if facts.svg_hit_areas == 0 {
            return;
        }
        if facts.svg_hit_areas & svg_hit_area::BOUNDING_BOX != 0 {
            self.append_svg_bounding_box(paintable);
            return;
        }
        let stroke = if facts.svg_hit_areas & svg_hit_area::STROKE != 0 {
            self.svg_stroke_hit_region(paintable)
        } else {
            None
        };
        if facts.svg_hit_areas & !svg_hit_area::STROKE == 0 && stroke.is_none() {
            return;
        }
        let [mut x, mut y, mut width, mut height] = path.bounding_box();
        if let Some(stroke) = &stroke {
            let outset = stroke.outset();
            x -= outset;
            y -= outset;
            width += outset * 2.0;
            height += outset * 2.0;
        }
        // The item rect rounds outward, past the far edges, so fixed-point quantization can never reject a valid hit
        // and points on those edges stay inside it.
        let x1 = x.floor() as i32;
        let y1 = y.floor() as i32;
        let x2 = (x + width).floor() as i32 + 1;
        let y2 = (y + height).floor() as i32 + 1;
        let bounding_box = CssPixelRect::new(
            CssPixels::from_integer(i64::from(x1)),
            CssPixels::from_integer(i64::from(y1)),
            CssPixels::from_integer(i64::from(x2 - x1)),
            CssPixels::from_integer(i64::from(y2 - y1)),
        );
        if bounding_box.is_empty() {
            return;
        }
        let context = self.rows.paintable_data(paintable).accumulated_visual_context;
        if !self.sink.wants(HitTestItemKind::SvgPath, bounding_box, context) {
            return;
        }
        let item = HitTestItem {
            rect: bounding_box,
            path: Some(path),
            winding_rule,
            svg_hit_areas: facts.svg_hit_areas,
            svg_stroke: stroke,
            ..self.base_hit_test_item(HitTestItemKind::SvgPath, paintable, context)
        };
        self.sink.append(item);
    }

    /// The character cells of a text element, as one path, which it's hit-tested by rather than by its glyphs.
    /// FIXME: The cells of text on a path aren't laid along the path, so text on a path, a <text> with a <textPath> in
    ///        it, and a <tspan> in a <textPath> are still hit-tested by their glyphs.
    fn svg_text_character_cells_path(&self, paintable: NodeSlotId) -> Option<libgfx_rust::path::Path> {
        if self.rows.node_kind_if_live(paintable) != Some(NodeKind::SVGTextBox)
            || self
                .rows
                .children(paintable)
                .any(|child| self.rows.node_kind_if_live(child) == Some(NodeKind::SVGTextPathBox))
        {
            return None;
        }
        let mut ancestor = self.rows.node_parent_if_live(paintable);
        while let Some(element) = ancestor {
            match self.rows.node_kind_if_live(element) {
                Some(NodeKind::SVGTextPathBox) => return None,
                Some(NodeKind::SVGTextBox) => ancestor = self.rows.node_parent_if_live(element),
                _ => break,
            }
        }
        // NB: Each element is hit-tested by its own characters, so that the elements inside it keep their own visibility and
        //     pointer-events.
        let cells = paintable_geometry::committed_svg_text_own_character_cells(self.rows, paintable)?;
        let mut path = libgfx_rust::path::Path::new();
        for cell in cells.iter().filter(|cell| cell.starts_typographic_character) {
            if cell.width <= 0.0 || cell.height <= 0.0 {
                continue;
            }
            let (left, top) = (cell.x, cell.y);
            let (right, bottom) = (cell.x + cell.width, cell.y + cell.height);
            path.move_to(FloatPoint { x: left, y: top });
            path.line_to(FloatPoint { x: right, y: top });
            path.line_to(FloatPoint { x: right, y: bottom });
            path.line_to(FloatPoint { x: left, y: bottom });
            path.close();
        }
        (!path.is_empty()).then_some(path)
    }

    fn svg_box_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground || !self.sink.hit_test_facts(paintable).visible_for_hit_testing {
            return;
        }
        if self.sink.hit_test_facts(paintable).svg_hit_areas & svg_hit_area::BOUNDING_BOX != 0 {
            self.append_svg_bounding_box(paintable);
        }
    }

    // NB: The rect of an SVG box is its bounding box, which takes in the boxes of its descendants, like the tspans of a
    //     text element. A point on the far edges of the bounding box is over it too, so the item rect reaches one
    //     fixed-point unit past them.
    fn append_svg_bounding_box(&mut self, paintable: NodeSlotId) {
        let one_unit = CssPixels::from_raw(1);
        let rect = paintable_geometry::absolute_rect(self.rows, paintable).inflated(
            CssPixels::default(),
            one_unit,
            one_unit,
            CssPixels::default(),
        );
        let context = self.rows.paintable_data(paintable).accumulated_visual_context;
        if !self.sink.wants(HitTestItemKind::Box, rect, context) {
            return;
        }
        let item = HitTestItem {
            rect,
            ..self.base_hit_test_item(HitTestItemKind::Box, paintable, context)
        };
        self.sink.append(item);
    }

    fn svg_stroke_hit_region(&self, paintable: NodeSlotId) -> Option<SvgStrokeHitRegion> {
        let style = self.rows.node_style_if_live(paintable)?;
        let stroke = crate::painting::record::paint::svg::svg_stroke_style(self.rows, paintable, &style);
        let mut thickness = stroke.width;
        let mut dash_array = stroke.dash_array;
        let mut dash_offset = stroke.dash_offset;
        // https://svgwg.org/svg2-draft/painting.html#PaintingVectorEffects
        // With the non-scaling-stroke vector effect, stroke outline shall be calculated in the "host" coordinate
        // space instead of user coordinate system.
        if stroke.non_scaling_stroke {
            let accumulated_scale = self.sink.own_accumulated_2d_scale(paintable);
            let accumulated_scale_area = accumulated_scale.width * accumulated_scale.height;
            if accumulated_scale_area > 0.0 {
                let scale = accumulated_scale_area.sqrt();
                thickness /= scale;
                dash_array.iter_mut().for_each(|value| *value /= scale);
                dash_offset /= scale;
            }
        }
        if thickness.is_nan() || thickness <= 0.0 {
            return None;
        }
        Some(SvgStrokeHitRegion::new(libgfx_rust::path::StrokeStyle {
            thickness,
            cap_style: stroke.cap_style,
            join_style: stroke.join_style,
            miter_limit: stroke.miter_limit,
            dash_array,
            dash_offset,
        }))
    }

    fn empty_editable_item(&mut self, paintable: NodeSlotId) {
        if self.is_anonymous(paintable)
            || !self
                .rows
                .node_has_dom_paint_fact(paintable, DomPaintFact::EditableOrEditingHost)
        {
            return;
        }
        let context = self.rows.paintable_data(paintable).accumulated_visual_context;
        for rect in paintable_geometry::absolute_border_box_rects_of_fragments(self.rows, paintable) {
            self.append_empty_editable(paintable, rect, context);
        }
    }

    fn fragment(
        &self,
        owner: NodeSlotId,
        index: usize,
    ) -> CommittedFragment<impl std::ops::Deref<Target = CommittedSideData> + 'a> {
        CommittedFragment {
            side_data: self.rows.committed_side_data(owner),
            index,
        }
    }

    fn fragment_is_block_level_box(&self, owner: NodeSlotId, index: usize) -> bool {
        text_fragment::is_block_level_box(self.rows, &self.fragment(owner, index))
    }

    // A text node has no style of its own, so its parent both decides whether the fragment can be hit and is what an
    // event dispatched from it resolves against. Returns that node, or None when the fragment is not hit-testable.
    fn hit_node_for_text_fragment(&mut self, fragment: &FragmentRecord) -> Option<NodeSlotId> {
        let node = fragment.layout_node;
        let row = self.rows.node(node)?;
        // https://drafts.csswg.org/css-overflow-4/#ellipsing-details
        // "The UA should dispatch any pointer event on the ellipsis to the elided element"
        if fragment.is_block_ellipsis || !node_facts::kind_is_text(row.kind()) {
            return None;
        }
        let parent = row.parent();
        let parent_style = self.rows.node_style_if_live(parent)?;
        if parent_style.visibility() != css_enums::visibility::VISIBLE
            || parent_style.effects().opacity == 0.0
            || parent_style.pointer_events() == css_enums::pointer_events::NONE
            || row.dom_paint_facts() & DomPaintFact::Inert as u8 != 0
        {
            return None;
        }
        // Resolving the hit needs a committed paintable row; without one there is nothing to resolve against.
        if !self.rows.paintable_row_is_populated(parent) {
            return None;
        }
        Some(parent)
    }

    fn append_box(
        &mut self,
        paintable_box: NodeSlotId,
        rect: CssPixelRect,
        context: ContextRef,
        border_radii: BorderRadii,
    ) {
        if !self.sink.wants(HitTestItemKind::Box, rect, context) {
            return;
        }
        let item = HitTestItem {
            rect,
            border_radii,
            ..self.base_hit_test_item(HitTestItemKind::Box, paintable_box, context)
        };
        self.sink.append(item);
    }

    fn base_hit_test_item(&self, kind: HitTestItemKind, target: NodeSlotId, context: ContextRef) -> HitTestItem {
        HitTestItem {
            kind,
            paintable: target,
            hit_node: target,
            chrome_widget_kind: CHROME_WIDGET_NONE,
            text_fragment_index: None,
            rect: CssPixelRect::default(),
            context,
            border_radii: BorderRadii::default(),
            path: None,
            winding_rule: WindingRule::Nonzero,
            svg_hit_areas: 0,
            svg_stroke: None,
        }
    }

    fn append_text_fragment(&mut self, owner: NodeSlotId, fragment_index: u32, context: ContextRef) {
        let fragment = self.fragment(owner, fragment_index as usize);
        let layout_arena = self.rows;
        let rect = text_fragment::absolute_rect(layout_arena, &fragment);
        if !self.sink.wants(HitTestItemKind::TextFragment, rect, context) {
            return;
        }
        let Some(hit_node) = self.hit_node_for_text_fragment(&fragment) else {
            return;
        };
        let item = HitTestItem {
            text_fragment_index: Some(fragment_index),
            rect,
            hit_node,
            ..self.base_hit_test_item(HitTestItemKind::TextFragment, owner, context)
        };
        self.sink.append(item);
    }

    fn append_empty_editable(&mut self, paintable: NodeSlotId, rect: CssPixelRect, context: ContextRef) {
        if !self.sink.wants(HitTestItemKind::EmptyEditable, rect, context) {
            return;
        }
        let item = HitTestItem {
            rect,
            ..self.base_hit_test_item(HitTestItemKind::EmptyEditable, paintable, context)
        };
        self.sink.append(item);
    }

    fn append_chrome_widget(&mut self, paintable: NodeSlotId, chrome_widget_kind: u8, context: ContextRef) {
        if !self
            .sink
            .wants(HitTestItemKind::ChromeWidget, CssPixelRect::default(), context)
        {
            return;
        }
        let item = HitTestItem {
            chrome_widget_kind,
            ..self.base_hit_test_item(HitTestItemKind::ChromeWidget, paintable, context)
        };
        self.sink.append(item);
    }
}
