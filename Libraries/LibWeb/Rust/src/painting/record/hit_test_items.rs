/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{PaintPhase, PaintRecorder};
use crate::css::css_enums;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::fragment_ownership;
use crate::painting::hit_test::*;
use crate::painting::host::FfiHitTestTextNodeFacts;
use crate::painting::node_painting;
use crate::painting::paintable_data::*;
use crate::painting::paintable_geometry;
use crate::painting::text_fragment;
use libgfx_rust::WindingRule;
use std::rc::Rc;

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct HitTestFacts {
    pub(crate) visible_for_hit_testing: bool,
    pub(crate) dom_node_has_parent: bool,
    pub(crate) is_editable_or_editing_host: bool,
    pub(crate) has_resizer: bool,
    pub(crate) could_be_scrolled_horizontally: bool,
    pub(crate) could_be_scrolled_vertically: bool,
    pub(crate) svg_path_has_fill: bool,
    pub(crate) svg_path_winding_rule: WindingRule,
    pub(crate) svg_mask_content_units_object_bbox: bool,
    pub(crate) svg_clip_path_units_object_bbox: bool,
    pub(crate) inside_blocking_wheel_event_handler: bool,
}

pub(crate) fn hit_test_facts(
    arena: &impl crate::painting::paintable_rows::PaintableRowsRead,
    paintable: NodeSlotId,
    inputs: &crate::painting::host::FfiRecordingInputs,
    dom: crate::painting::host::FfiHitTestPaintableFacts,
) -> HitTestFacts {
    let Some(style) = arena.node_style_if_live(paintable) else {
        return HitTestFacts {
            dom_node_has_parent: dom.dom_node_has_parent,
            is_editable_or_editing_host: dom.is_editable_or_editing_host,
            svg_mask_content_units_object_bbox: dom.svg_mask_content_units_object_bbox,
            svg_clip_path_units_object_bbox: dom.svg_clip_path_units_object_bbox,
            inside_blocking_wheel_event_handler: dom.inside_blocking_wheel_event_handler,
            ..HitTestFacts::default()
        };
    };
    let wheel_axes = crate::painting::chrome_geometry::wheel_scrollable_axes(
        arena,
        paintable,
        inputs.viewport_wheel_overflow_x,
        inputs.viewport_wheel_overflow_y,
    );
    let svg_path = arena
        .node_kind_if_live(paintable)
        .is_some_and(node_painting::is_svg_path);
    let svg = style.inherited_svg();
    HitTestFacts {
        visible_for_hit_testing: arena.paintable_row_is_populated(paintable)
            && !dom.is_inert
            && style.inherited_ui().pointer_events != css_enums::pointer_events::NONE,
        dom_node_has_parent: dom.dom_node_has_parent,
        is_editable_or_editing_host: dom.is_editable_or_editing_host,
        has_resizer: crate::painting::chrome_geometry::has_resizer(arena, paintable),
        could_be_scrolled_horizontally: wheel_axes.horizontal,
        could_be_scrolled_vertically: wheel_axes.vertical,
        svg_path_has_fill: svg_path && crate::painting::record::paint::svg::svg_paint_color(&svg.fill).is_some(),
        svg_path_winding_rule: if svg_path && svg.fill_rule == css_enums::fill_rule::EVENODD {
            WindingRule::EvenOdd
        } else {
            WindingRule::Nonzero
        },
        svg_mask_content_units_object_bbox: dom.svg_mask_content_units_object_bbox,
        svg_clip_path_units_object_bbox: dom.svg_clip_path_units_object_bbox,
        inside_blocking_wheel_event_handler: dom.inside_blocking_wheel_event_handler,
    }
}

impl<'a> PaintRecorder<'a> {
    fn text_node_facts(&mut self, text_node: NodeSlotId) -> FfiHitTestTextNodeFacts {
        let key = text_node.index;
        if let Some(facts) = self.text_node_facts_cache.get(&key) {
            return *facts;
        }
        let facts = self.host.text_node_facts(self.layout_arena.shell_if_live(text_node));
        self.text_node_facts_cache.insert(key, facts);
        facts
    }

    fn is_anonymous(&self, paintable: NodeSlotId) -> bool {
        crate::painting::style_queries::is_anonymous(self.layout_arena, paintable)
    }

    fn is_generated_for_pseudo_element(&self, paintable: NodeSlotId) -> bool {
        self.layout_arena.node_is_generated_for_pseudo_element(paintable)
    }

    fn is_atomic_inline(&self, paintable: NodeSlotId) -> bool {
        crate::painting::style_queries::is_atomic_inline(self.layout_arena, paintable)
    }

    pub(crate) fn record_foreign_object_descendant_hit_test_items(&mut self, paintable: NodeSlotId) {
        let mut next_child = crate::painting::paint_order::first_paint_child(self.layout_arena, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, child);
            self.record_hit_test_items(child, PaintPhase::Background);
            self.record_foreign_object_descendant_hit_test_items(child);
            self.record_hit_test_items(child, PaintPhase::Foreground);
            self.record_hit_test_items(child, PaintPhase::Overlay);
        }
    }

    pub(crate) fn record_hit_test_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if self.nested.is_some() {
            return;
        }
        if node_painting::is_inline(self.layout_arena, paintable) {
            self.record_inline_hit_test_items(paintable, phase);
        } else if node_painting::has_lines(self.layout_arena, paintable) {
            self.record_base_hit_test_items(paintable, phase);
            self.record_lines_hit_test_items(paintable, phase);
        } else if self
            .layout_arena
            .node_kind_if_live(paintable)
            .is_some_and(node_painting::is_svg_path)
        {
            self.record_svg_path_hit_test_items(paintable, phase);
        } else {
            self.record_base_hit_test_items(paintable, phase);
        }
    }

    fn record_base_hit_test_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Background && phase != PaintPhase::Overlay {
            return;
        }
        if !self.visibility_is_visible(paintable) || !self.visible_for_hit_testing(paintable) {
            return;
        }
        if phase == PaintPhase::Background {
            if self.is_anonymous(paintable) && !self.is_generated_for_pseudo_element(paintable) {
                return;
            }
            let rect = paintable_geometry::absolute_border_box_rect(self.layout_arena, paintable);
            let radii = self.border_radii(paintable);
            let context = self.data(paintable).accumulated_visual_context;
            self.append_box(paintable, paintable, rect, context, radii);
            return;
        }
        let facts = self.paintable_facts(paintable);
        let context = self.data(paintable).accumulated_visual_context;
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

    fn record_lines_hit_test_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        let fragment_count = self.layout_arena.paintable_side_data(paintable).fragments.len();
        if fragment_count == 0
            && crate::painting::paint_order::first_paint_child(self.layout_arena, paintable).is_none()
        {
            if self.is_visible(paintable) && self.visible_for_hit_testing(paintable) {
                self.record_empty_editable_hit_test_item(paintable);
            }
            return;
        }
        let context = self.data(paintable).accumulated_visual_context_for_descendants;
        // Fragments inside self-painting inline boxes are recorded during that box's paint.
        let filter = fragment_ownership::effective_filter(self.layout_arena, paintable);
        filter.for_each_owned_fragment_index(fragment_count, |index| {
            if self.fragment_is_block_level_box(paintable, index) {
                return;
            }
            self.append_text_fragment(paintable, index as u32, context);
        });
        self.record_empty_line_caret_items(paintable, context);
    }

    fn record_empty_line_caret_items(&mut self, paintable: NodeSlotId, context: ContextRef) {
        let targets = crate::painting::visual_lines::empty_line_caret_targets(self.layout_arena, paintable);
        for target in targets {
            self.append_empty_line_for_fragment(paintable, 0, target.offset, target.line_index, target.rect, context);
        }
        if !self.layout_arena.paintable_side_data(paintable).fragments.is_empty() {
            for target in self.host.line_break_caret_targets(self.layout_node_shell(paintable)) {
                let rect = CssPixelRect::from(target.rect);
                let caret_node = paintable;
                self.append_empty_line_for_node(paintable, caret_node, target.caret_offset, rect, context);
            }
        }
    }

    fn record_inline_hit_test_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if self.is_anonymous(paintable) && !self.is_generated_for_pseudo_element(paintable) {
            return;
        }
        // Only this box's own items are gated on its visibility; the fragments it owns may belong
        // to descendants that override visibility or pointer-events, so they are filtered per
        // fragment instead.
        let box_itself_is_hit_testable = self.is_visible(paintable) && self.visible_for_hit_testing(paintable);

        if phase == PaintPhase::Background {
            if box_itself_is_hit_testable {
                self.append_piece_boxes(paintable);
            }
            return;
        }
        if phase != PaintPhase::Foreground {
            return;
        }

        if fragment_ownership::is_self_painting_inline(self.layout_arena, paintable) {
            let Some(root) = self.inline_root(paintable) else {
                return;
            };
            let context = self.data(paintable).accumulated_visual_context_for_descendants;
            let fragment_count = self.layout_arena.paintable_side_data(root).fragments.len();
            let filter = fragment_ownership::effective_filter(self.layout_arena, paintable);
            // Hit-test precedence follows paint order: this box's own text loses to the box itself
            // (re-recorded so its z-order matches this box's paint order), while nested content
            // (e.g. a link inside this box) wins over it.
            let own_layout_node = paintable;
            let mut nested_content_indices = Vec::new();
            filter.for_each_owned_fragment_index(fragment_count, |index| {
                if self.fragment_is_block_level_box(root, index) {
                    return;
                }
                let fragment_node = self.layout_arena.paintable_side_data(root).fragments[index].layout_node;
                if fragment_ownership::nearest_fragmented_inline_ancestor(self.layout_arena, fragment_node)
                    == Some(own_layout_node)
                {
                    self.append_text_fragment(root, index as u32, context);
                } else {
                    nested_content_indices.push(index);
                }
            });
            if box_itself_is_hit_testable && self.has_stacking_context(paintable) {
                self.append_piece_boxes(paintable);
            }
            for index in nested_content_indices {
                self.append_text_fragment(root, index as u32, context);
            }
        }

        // Clicks must be able to place the caret in an editable element with no content, so its
        // target is recorded in foreground order, winning over earlier-painted overlapping content.
        if box_itself_is_hit_testable && !self.inline_has_content(paintable) {
            self.record_empty_editable_hit_test_item(paintable);
        }
    }

    fn inline_root(&self, paintable: NodeSlotId) -> Option<NodeSlotId> {
        let block = self.data(paintable).containing_block;
        if block.is_invalid() || !self.layout_arena.paintable_row_is_populated(block) {
            return None;
        }
        node_painting::has_lines(self.layout_arena, block).then_some(block)
    }

    fn piece_of(&self, root: NodeSlotId, piece_index: u32) -> InlineBoxPieceRecord {
        self.layout_arena.paintable_side_data(root).inline_box_pieces[piece_index as usize]
    }

    fn inline_has_content(&self, paintable: NodeSlotId) -> bool {
        let has_content_pieces = self.inline_root(paintable).is_some_and(|root| {
            self.layout_arena
                .paintable_side_data(paintable)
                .piece_indices
                .iter()
                .any(|piece_index| !self.piece_of(root, *piece_index).is_geometry_only_placeholder)
        });
        has_content_pieces || crate::painting::paint_order::first_paint_child(self.layout_arena, paintable).is_some()
    }

    fn append_piece_boxes(&mut self, paintable: NodeSlotId) {
        let Some(root) = self.inline_root(paintable) else {
            return;
        };
        let root_position = paintable_geometry::absolute_position(self.layout_arena, root);
        let layout_arena = self.layout_arena;
        let context = self.data(paintable).accumulated_visual_context;
        for piece_index in &layout_arena.paintable_side_data(paintable).piece_indices {
            let piece = &layout_arena.paintable_side_data(root).inline_box_pieces[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_position);
            let radii = self.piece_border_radii(paintable, piece);
            self.append_box(paintable, paintable, rect, context, radii);
        }
    }

    fn record_svg_path_hit_test_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        let Some(path) = paintable_geometry::committed_svg_path(self.layout_arena, paintable) else {
            return;
        };
        if !self.visibility_is_visible(paintable) || !self.visible_for_hit_testing(paintable) {
            return;
        }
        let facts = self.paintable_facts(paintable);
        // FIXME: Hit test the stroked region of paths without a fill.
        if !facts.svg_path_has_fill {
            return;
        }
        // The item rect rounds outward so fixed-point quantization can never reject a valid hit.
        let [x, y, width, height] = path.bounding_box();
        let x1 = x.floor() as i32;
        let y1 = y.floor() as i32;
        let x2 = (x + width).ceil() as i32;
        let y2 = (y + height).ceil() as i32;
        let bounding_box = CssPixelRect::new(
            CssPixels::from_integer(i64::from(x1)),
            CssPixels::from_integer(i64::from(y1)),
            CssPixels::from_integer(i64::from(x2 - x1)),
            CssPixels::from_integer(i64::from(y2 - y1)),
        );
        if bounding_box.is_empty() {
            return;
        }
        let context = self.data(paintable).accumulated_visual_context;
        self.append_svg_path(paintable, path, facts.svg_path_winding_rule, bounding_box, context);
    }

    fn record_empty_editable_hit_test_item(&mut self, paintable: NodeSlotId) {
        if self.is_anonymous(paintable) || !self.paintable_facts(paintable).is_editable_or_editing_host {
            return;
        }
        let rect = paintable_geometry::absolute_border_box_rect(self.layout_arena, paintable);
        let context = self.data(paintable).accumulated_visual_context;
        self.append_empty_editable(paintable, rect, context);
    }

    fn fragment(&self, owner: NodeSlotId, index: usize) -> std::cell::Ref<'a, FragmentRecord> {
        std::cell::Ref::map(self.layout_arena.paintable_side_data(owner), |side_data| {
            &side_data.fragments[index]
        })
    }

    fn fragment_is_block_level_box(&self, owner: NodeSlotId, index: usize) -> bool {
        text_fragment::is_block_level_box(self.layout_arena, &self.fragment(owner, index))
    }

    fn text_fragment_is_hit_testable(&mut self, fragment: &FragmentRecord) -> bool {
        let node = fragment.layout_node;
        let Some(kind) = self.layout_arena.node_kind_if_live(node) else {
            return false;
        };
        if !node_facts::kind_is_text(kind) {
            return false;
        }
        let Some(parent) = self.layout_arena.node_parent_if_live(node) else {
            return false;
        };
        let parent_visible = self
            .layout_arena
            .node_style_if_live(parent)
            .is_none_or(|style| style.visibility() == css_enums::visibility::VISIBLE);
        if !parent_visible {
            return false;
        }
        let Some(parent_style) = self.layout_arena.node_style_if_live(parent) else {
            return false;
        };
        if parent_style.effects().opacity == 0.0
            || parent_style.inherited_ui().pointer_events == css_enums::pointer_events::NONE
        {
            return false;
        }
        let facts = self.text_node_facts(node);
        if facts.is_inert {
            return false;
        }
        true
    }

    fn containing_block_margin_rect(&self, containing_block: NodeSlotId) -> Option<CssPixelRect> {
        if containing_block.is_invalid() || !self.layout_arena.paintable_row_is_populated(containing_block) {
            return None;
        }
        let absolute = paintable_geometry::absolute_rect(self.layout_arena, containing_block);
        let margin = paintable_geometry::committed_margin(self.layout_arena, containing_block);
        let border = paintable_geometry::committed_border(self.layout_arena, containing_block);
        let padding = paintable_geometry::committed_padding(self.layout_arena, containing_block);
        let content_size = paintable_geometry::committed_content_size(self.layout_arena, containing_block);
        let top = margin.top + border.top + padding.top;
        let right = margin.right + border.right + padding.right;
        let bottom = margin.bottom + border.bottom + padding.bottom;
        let left = margin.left + border.left + padding.left;
        Some(CssPixelRect::new(
            absolute.x - left,
            absolute.y - top,
            content_size.width + left + right,
            content_size.height + top + bottom,
        ))
    }

    fn margin_rect_for_paintable(&self, paintable: NodeSlotId) -> Option<CssPixelRect> {
        self.containing_block_margin_rect(self.data(paintable).containing_block)
    }

    fn margin_rect_for_fragment(&self, fragment: &FragmentRecord) -> Option<CssPixelRect> {
        let block = text_fragment::containing_block_paintable(self.layout_arena, fragment)?;
        self.containing_block_margin_rect(block)
    }

    fn writing_mode_of(&self, paintable: NodeSlotId) -> u8 {
        self.layout_arena
            .node_style_if_live(paintable)
            .map_or(css_enums::writing_mode::HORIZONTAL_TB, |style| style.writing_mode())
    }

    fn inline_axis_is_reverse_of(&self, paintable: NodeSlotId) -> bool {
        self.layout_arena
            .node_style_if_live(paintable)
            .is_some_and(|style| style.inline_axis_is_reverse())
    }

    fn block_axis_is_reverse_of(&self, paintable: NodeSlotId) -> bool {
        self.layout_arena
            .node_style_if_live(paintable)
            .is_some_and(|style| style.block_axis_is_reverse())
    }

    fn layout_containing_block_of(&self, paintable: NodeSlotId) -> NodeSlotId {
        self.layout_arena
            .node_containing_block_if_live(paintable)
            .unwrap_or(NodeSlotId::INVALID)
    }

    fn node_has_dom_node(&self, node: NodeSlotId) -> bool {
        !self.layout_arena.shell_if_live(node).is_null()
            && self.layout_arena.node_flags_if_live(node) & NodeFlag::Anonymous as u32 == 0
    }

    fn absolute_containing_line_box_rect(&self, paintable: NodeSlotId) -> Option<CssPixelRect> {
        let containing_line_box_index =
            paintable_geometry::committed_containing_line_box_index(self.layout_arena, paintable)?;
        let block = self.data(paintable).containing_block;
        if block.is_invalid()
            || !self.layout_arena.paintable_row_is_populated(block)
            || !node_painting::has_lines(self.layout_arena, block)
        {
            return None;
        }
        let side_data = self.layout_arena.paintable_side_data(block);
        let line = side_data.lines.get(containing_line_box_index)?;
        Some(
            CssPixelRect::from(line.rect)
                .translated_by(paintable_geometry::absolute_position(self.layout_arena, block)),
        )
    }

    fn append_box(
        &mut self,
        paintable_box: NodeSlotId,
        target: NodeSlotId,
        rect: CssPixelRect,
        context: ContextRef,
        border_radii: BorderRadii,
    ) {
        let (caret_line_index, caret_line_rect) =
            match paintable_geometry::committed_containing_line_box_index(self.layout_arena, paintable_box) {
                Some(containing_line_box_index) => (
                    Some(containing_line_box_index),
                    self.absolute_containing_line_box_rect(paintable_box),
                ),
                None => (None, None),
            };
        let can_produce_caret_position = (self.is_atomic_inline(target) || self.is_replaced_box(target)) && {
            let negative_z =
                crate::painting::style_queries::effective_z_index(self.layout_arena, target).unwrap_or(0) < 0;
            let target_node = target;
            !negative_z && self.node_has_dom_node(target_node) && self.paintable_facts(target).dom_node_has_parent
        };
        let item = HitTestItem {
            rect,
            caret_rect: rect,
            caret_line_index,
            caret_line_rect,
            block_container_margin_rect: self.margin_rect_for_paintable(paintable_box),
            border_radii,
            can_produce_caret_position,
            ..self.base_hit_test_item(HitTestItemKind::Box, target, context)
        };
        self.list.append(item);
    }

    fn base_hit_test_item(&self, kind: HitTestItemKind, target: NodeSlotId, context: ContextRef) -> HitTestItem {
        HitTestItem {
            kind,
            paintable: target,
            chrome_widget_kind: CHROME_WIDGET_NONE,
            text_fragment_index: None,
            caret_node: NodeSlotId::INVALID,
            caret_offset: 0,
            rect: CssPixelRect::default(),
            caret_rect: CssPixelRect::default(),
            caret_line_index: None,
            caret_line_rect: None,
            block_container_margin_rect: None,
            context,
            border_radii: BorderRadii::default(),
            path: None,
            winding_rule: 0,
            writing_mode: self.writing_mode_of(target),
            inline_axis_is_reverse: self.inline_axis_is_reverse_of(target),
            block_axis_is_reverse: self.block_axis_is_reverse_of(target),
            containing_block: self.layout_containing_block_of(target),
            can_produce_caret_position: false,
        }
    }

    fn append_svg_path(
        &mut self,
        target: NodeSlotId,
        path: Rc<libgfx_rust::path::OwnedPath>,
        winding_rule: libgfx_rust::WindingRule,
        bounding_box: CssPixelRect,
        context: ContextRef,
    ) {
        let item = HitTestItem {
            rect: bounding_box,
            path: Some(path),
            winding_rule: winding_rule as i32,
            ..self.base_hit_test_item(HitTestItemKind::SvgPath, target, context)
        };
        self.list.append(item);
    }

    fn append_text_fragment(&mut self, owner: NodeSlotId, fragment_index: u32, context: ContextRef) {
        let fragment = self.fragment(owner, fragment_index as usize);
        if !self.text_fragment_is_hit_testable(&fragment) {
            return;
        }
        let layout_arena = self.layout_arena;
        let first_available_font = || text_fragment::first_available_font(layout_arena, &fragment);
        let item = HitTestItem {
            text_fragment_index: Some(fragment_index),
            rect: text_fragment::absolute_rect(layout_arena, &fragment),
            caret_rect: text_fragment::whole_range_rect(layout_arena, &fragment, first_available_font),
            caret_line_index: Some(fragment.line_index as usize),
            caret_line_rect: Some(text_fragment::absolute_line_box_rect(layout_arena, owner, &fragment)),
            block_container_margin_rect: self.margin_rect_for_fragment(&fragment),
            containing_block: layout_arena
                .node_containing_block_if_live(fragment.layout_node)
                .unwrap_or(NodeSlotId::INVALID),
            can_produce_caret_position: self.node_has_dom_node(fragment.layout_node),
            ..self.base_hit_test_item(HitTestItemKind::TextFragment, owner, context)
        };
        self.list.append(item);
    }

    fn append_empty_line_for_fragment(
        &mut self,
        owner: NodeSlotId,
        sibling_fragment_index: u32,
        caret_offset: usize,
        line_box_index: usize,
        line_rect: CssPixelRect,
        context: ContextRef,
    ) {
        let fragment = self.fragment(owner, sibling_fragment_index as usize);
        if !self.text_fragment_is_hit_testable(&fragment) {
            return;
        }
        // Empty lines are only reachable through caret lines, never through regular hit testing,
        // so they are recorded with the base's empty rect and stay out of the spatial index.
        let item = HitTestItem {
            text_fragment_index: Some(sibling_fragment_index),
            caret_node: fragment.layout_node,
            caret_offset,
            caret_rect: line_rect,
            caret_line_index: Some(line_box_index),
            caret_line_rect: Some(line_rect),
            block_container_margin_rect: self.margin_rect_for_fragment(&fragment),
            containing_block: self
                .layout_arena
                .node_containing_block_if_live(fragment.layout_node)
                .unwrap_or(NodeSlotId::INVALID),
            can_produce_caret_position: self.node_has_dom_node(fragment.layout_node),
            ..self.base_hit_test_item(HitTestItemKind::EmptyLine, owner, context)
        };
        self.list.append(item);
    }

    fn append_empty_line_for_node(
        &mut self,
        owner: NodeSlotId,
        caret_node: NodeSlotId,
        caret_offset: usize,
        line_rect: CssPixelRect,
        context: ContextRef,
    ) {
        let item = HitTestItem {
            caret_node,
            caret_offset,
            caret_rect: line_rect,
            caret_line_rect: Some(line_rect),
            block_container_margin_rect: self.margin_rect_for_paintable(owner),
            can_produce_caret_position: self.node_has_dom_node(caret_node),
            ..self.base_hit_test_item(HitTestItemKind::EmptyLine, owner, context)
        };
        self.list.append(item);
    }

    fn append_empty_editable(&mut self, paintable: NodeSlotId, rect: CssPixelRect, context: ContextRef) {
        let item = HitTestItem {
            rect,
            caret_rect: rect,
            block_container_margin_rect: self.margin_rect_for_paintable(paintable),
            can_produce_caret_position: self.node_has_dom_node(paintable),
            ..self.base_hit_test_item(HitTestItemKind::EmptyEditable, paintable, context)
        };
        self.list.append(item);
    }

    fn append_chrome_widget(&mut self, paintable: NodeSlotId, chrome_widget_kind: u8, context: ContextRef) {
        let item = HitTestItem {
            chrome_widget_kind,
            ..self.base_hit_test_item(HitTestItemKind::ChromeWidget, paintable, context)
        };
        self.list.append(item);
    }
}
