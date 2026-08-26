/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod async_scroll_metadata;
pub mod cache;
pub mod hit_test_items;
pub mod masks;
pub mod paint;
pub mod traversal;

use crate::css::css_enums;
use crate::layout::node_data::NodeKind;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::hit_test::HitTestList;
use crate::painting::host::{
    FfiHitTestHostCallbacks, FfiHitTestTextNodeFacts, FfiPaintHostCallbacks, FfiRecordingInputs,
    FfiVisualContextHostCallbacks,
};
use crate::painting::paintable_data::{InlineBoxPieceRecord, PaintableData};
use crate::painting::paintable_rows::PaintableRowsRef;
use crate::painting::stacking_context::{NO_STACKING_CONTEXT, StackingContextTree};
use crate::painting::visual_context::nested::NestedAssignments;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Default)]
pub struct RecordingOutput {
    pub id: u64,
    pub compatible_visual_context_tree_version: u64,
    // A default-constructed output's 0.0 never matches a real recording scale.
    pub recorded_device_pixels_per_css_pixel: f64,
    pub hit_test_list: HitTestList,
    pub display_list_bytes: Vec<u8>,
    pub has_blocking_wheel_event_listeners: bool,
    pub mask_display_lists: Vec<(usize, crate::painting::display_list::commands::DisplayListResourceId)>,
    pub spliced_capture_count: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PaintPhase {
    Background,
    Border,
    TableCollapsedBorder,
    Foreground,
    Outline,
    Overlay,
}

impl PaintPhase {
    pub const COUNT: usize = 6;
}
pub(crate) struct NestedRecordingState {
    pub(crate) assignments: NestedAssignments,
}

pub struct PaintRecorder<'a> {
    pub(crate) layout_arena: &'a PaintableRowsRef<'a>,
    pub(crate) paint_state: &'a crate::painting::paint_state::PaintState,
    stacking_contexts: &'a StackingContextTree,
    pub(crate) host: &'a FfiHitTestHostCallbacks,
    pub(crate) paint_host: &'a FfiPaintHostCallbacks,
    pub(crate) inputs: FfiRecordingInputs,
    pub(crate) recorder: DisplayListRecorder,
    pub(crate) converter: DevicePixelConverter,
    pub(crate) draw_svg_geometry_for_clip_path: bool,
    pub(crate) visual_context_host: &'a FfiVisualContextHostCallbacks,
    pub(crate) nested: Option<NestedRecordingState>,
    pub(crate) nested_tree: Option<crate::painting::visual_context::VisualContextTree>,
    pub(crate) prerecorded: crate::painting::record::masks::PrerecordedNestedDisplayLists,
    pub(crate) viewport: NodeSlotId,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    display_list_id: u64,
    hit_test_list_generation: u64,
    pub(crate) has_blocking_wheel_event_listeners: bool,
    spliced_capture_count: usize,
    uncacheable_paint_generation: u64,
    list: HitTestList,
    base_paint_facts_cache: Vec<Option<(NodeSlotId, BasePaintFacts)>>,
    paintable_facts_cache: Vec<Option<(NodeSlotId, hit_test_items::HitTestFacts)>>,
    pub(crate) absolute_position_cache: Vec<std::cell::Cell<Option<(NodeSlotId, used_values::FfiCssPixelPoint)>>>,
    // Validity checks must compare against this recording-start snapshot, not the live per-row
    // cell: the first phase that re-records a moved row updates the cell, and later phases
    // would then wrongly accept their stale captures.
    pub(crate) previously_captured_position_cache:
        Vec<std::cell::Cell<Option<(NodeSlotId, used_values::FfiCssPixelPoint)>>>,
    pub(crate) completed_record_gen: u64,
    pub(crate) all_paint_caches_dirty: bool,
    pub(crate) all_descendant_subtree_caches_dirty: bool,
    text_node_facts_cache: HashMap<u32, FfiHitTestTextNodeFacts>,
    font_resource_id_cache: HashMap<usize, u64>,
    text_control_selection_cache: HashMap<u32, crate::painting::host::FfiTextControlSelection>,
    selection_style_cache: HashMap<u32, Rc<paint::text::SelectionStyleAnswer>>,
    pub(crate) wheel_hit_test_target_cache: HashMap<NodeSlotId, usize>,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct BasePaintFacts {
    pub is_visible: bool,
    pub empty_cells_property_applies: bool,
    pub has_backdrop_filter: bool,
    pub paints_border_image: bool,
}

impl<'a> PaintRecorder<'a> {
    pub(crate) fn prevent_descendant_subtree_caching(&mut self) {
        self.uncacheable_paint_generation = self
            .uncacheable_paint_generation
            .checked_add(1)
            .expect("uncacheable paint generation overflowed");
    }

    pub(crate) fn data(&self, paintable: NodeSlotId) -> &PaintableData {
        self.layout_arena.paintable_data(paintable)
    }

    pub(crate) fn layout_node_shell(&self, paintable: NodeSlotId) -> *mut std::ffi::c_void {
        self.layout_arena.shell_if_live(paintable)
    }

    pub(crate) fn hit_test_facts(&mut self, paintable: NodeSlotId) -> hit_test_items::HitTestFacts {
        self.paintable_facts(paintable)
    }

    fn paintable_facts(&mut self, paintable: NodeSlotId) -> hit_test_items::HitTestFacts {
        let index = paintable.slot_index() as usize;
        if let Some((memoized_id, facts)) = self.paintable_facts_cache[index]
            && memoized_id == paintable
        {
            return facts;
        }
        let dom_facts = self.host.paintable_facts(self.layout_node_shell(paintable));
        let facts = hit_test_items::hit_test_facts(self.layout_arena, paintable, &self.inputs, dom_facts);
        self.paintable_facts_cache[index] = Some((paintable, facts));
        facts
    }

    pub(crate) fn register_font(&mut self, font: *const std::ffi::c_void) -> u64 {
        let key = font as usize;
        if let Some(font_id) = self.font_resource_id_cache.get(&key) {
            return *font_id;
        }
        let font_id = self.paint_host.register_font(font);
        self.font_resource_id_cache.insert(key, font_id);
        font_id
    }

    pub(crate) fn text_control_selection(
        &mut self,
        node: crate::layout::node_data::NodeSlotId,
    ) -> crate::painting::host::FfiTextControlSelection {
        let key = node.index;
        if let Some(facts) = self.text_control_selection_cache.get(&key) {
            return *facts;
        }
        let facts = self
            .paint_host
            .text_control_selection(self.layout_arena.shell_if_live(node));
        self.text_control_selection_cache.insert(key, facts);
        facts
    }

    pub(crate) fn selection_style(
        &mut self,
        node: crate::layout::node_data::NodeSlotId,
    ) -> Rc<paint::text::SelectionStyleAnswer> {
        let key = node.index;
        if let Some(answer) = self.selection_style_cache.get(&key) {
            return answer.clone();
        }
        let (facts, shadows) = self
            .paint_host
            .selection_style_facts(self.layout_arena.shell_if_live(node));
        let answer = Rc::new(paint::text::SelectionStyleAnswer { facts, shadows });
        self.selection_style_cache.insert(key, answer.clone());
        answer
    }

    pub(crate) fn nested_recording_session(
        &self,
        recorder: DisplayListRecorder,
        nested: Option<NestedRecordingState>,
        nested_tree: Option<crate::painting::visual_context::VisualContextTree>,
        draw_svg_geometry_for_clip_path: bool,
    ) -> PaintRecorder<'a> {
        PaintRecorder {
            layout_arena: self.layout_arena,
            paint_state: self.paint_state,
            stacking_contexts: self.stacking_contexts,
            host: self.host,
            paint_host: self.paint_host,
            inputs: self.inputs,
            recorder,
            converter: self.converter,
            draw_svg_geometry_for_clip_path,
            visual_context_host: self.visual_context_host,
            nested,
            nested_tree,
            prerecorded: crate::painting::record::masks::PrerecordedNestedDisplayLists::default(),
            viewport: self.viewport,
            command_cache_source: None,
            item_cache_source: None,
            display_list_id: self.display_list_id,
            hit_test_list_generation: self.hit_test_list_generation,
            has_blocking_wheel_event_listeners: false,
            spliced_capture_count: 0,
            uncacheable_paint_generation: 0,
            list: HitTestList::default(),
            base_paint_facts_cache: vec![None; self.layout_arena.paintable_row_count()],
            paintable_facts_cache: vec![None; self.layout_arena.paintable_row_count()],
            absolute_position_cache: (0..self.layout_arena.paintable_row_count())
                .map(|_| std::cell::Cell::new(None))
                .collect(),
            previously_captured_position_cache: (0..self.layout_arena.paintable_row_count())
                .map(|_| std::cell::Cell::new(None))
                .collect(),
            completed_record_gen: self.completed_record_gen,
            all_paint_caches_dirty: self.all_paint_caches_dirty,
            all_descendant_subtree_caches_dirty: self.all_descendant_subtree_caches_dirty,
            text_node_facts_cache: HashMap::new(),
            font_resource_id_cache: HashMap::new(),
            text_control_selection_cache: HashMap::new(),
            selection_style_cache: HashMap::new(),
            wheel_hit_test_target_cache: HashMap::new(),
        }
    }

    pub(crate) fn border_radii(&mut self, paintable: NodeSlotId) -> BorderRadii {
        let Some(style) = self.layout_arena.node_style_if_live(paintable) else {
            return BorderRadii::default();
        };
        crate::painting::visual_context::node_values::border_radii_data(style, self.layout_arena, paintable)
    }

    pub(crate) fn piece_border_radii(&mut self, paintable: NodeSlotId, piece: &InlineBoxPieceRecord) -> BorderRadii {
        let Some(style) = self.layout_arena.node_style_if_live(paintable) else {
            return BorderRadii::default();
        };
        crate::painting::visual_context::node_values::piece_border_radii_data(
            style,
            piece.border_box_rect.width,
            piece.border_box_rect.height,
            piece.present_edges,
        )
    }

    pub(crate) fn base_paint_facts(&mut self, paintable: NodeSlotId) -> BasePaintFacts {
        let index = paintable.slot_index() as usize;
        if let Some((memoized_id, facts)) = self.base_paint_facts_cache[index]
            && memoized_id == paintable
        {
            return facts;
        }
        let Some(style) = self.layout_arena.node_style_if_live(paintable) else {
            let facts = BasePaintFacts::default();
            self.base_paint_facts_cache[index] = Some((paintable, facts));
            return facts;
        };
        let effects = style.effects();
        let is_visible = style.visibility() == crate::css::css_enums::visibility::VISIBLE && effects.opacity != 0.0;
        let empty_cells_property_applies = self.display(paintable).is_internal_table()
            && style.empty_cells() == crate::css::css_enums::empty_cells::HIDE
            && crate::painting::paint_order::first_paint_child(self.layout_arena, paintable).is_none();
        let has_backdrop_filter = effects.backdrop_filter.operations.length != 0;
        let paints_border_image = crate::painting::style_queries::handle_value(&style.border().border_image_source)
            .is_some_and(|source| matches!(source, crate::css::style_value::StyleValueData::Image { .. }));
        let facts = BasePaintFacts {
            is_visible,
            empty_cells_property_applies,
            has_backdrop_filter,
            paints_border_image,
        };
        self.base_paint_facts_cache[index] = Some((paintable, facts));
        facts
    }

    fn has_stacking_context(&self, paintable: NodeSlotId) -> bool {
        self.data(paintable).stacking_context != NO_STACKING_CONTEXT
    }

    fn layout_kind(&self, paintable: NodeSlotId) -> Option<NodeKind> {
        self.layout_arena.node_kind_if_live(paintable)
    }

    fn display(&self, paintable: NodeSlotId) -> crate::css::display::FfiDisplay {
        crate::painting::style_queries::display(self.layout_arena, paintable)
    }

    fn visibility_is_visible(&self, paintable: NodeSlotId) -> bool {
        self.layout_arena
            .node_style_if_live(paintable)
            .is_none_or(|style| style.visibility() == css_enums::visibility::VISIBLE)
    }

    pub(crate) fn is_visible(&mut self, paintable: NodeSlotId) -> bool {
        self.base_paint_facts(paintable).is_visible
    }

    pub(crate) fn visible_for_hit_testing(&mut self, paintable: NodeSlotId) -> bool {
        self.paintable_facts(paintable).visible_for_hit_testing
    }

    fn is_replaced_box(&self, paintable: NodeSlotId) -> bool {
        crate::painting::style_queries::is_replaced_box(self.layout_arena, paintable)
    }

    pub(crate) fn own_context_index(&self, paintable: NodeSlotId) -> usize {
        if let Some(nested) = &self.nested
            && let Some((own, _)) = nested.assignments.paintable_indices.get(&paintable.index)
        {
            return *own;
        }
        self.data(paintable).accumulated_visual_context_index
    }

    pub(crate) fn accumulated_2d_scale_at(&self, index: usize) -> libgfx_rust::FloatSize {
        self.paint_host.accumulated_2d_scale(self.nested_tree.as_ref(), index)
    }

    pub(crate) fn own_accumulated_2d_scale(&self, paintable: NodeSlotId) -> libgfx_rust::FloatSize {
        self.accumulated_2d_scale_at(self.own_context_index(paintable))
    }
}
