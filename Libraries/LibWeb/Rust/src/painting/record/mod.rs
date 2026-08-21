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
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeKind;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::hit_test::HitTestList;
use crate::painting::host::{
    FfiHitTestHostCallbacks, FfiHitTestPaintableFacts, FfiHitTestTextNodeFacts, FfiPaintHostCallbacks,
    FfiRecordingInputs, FfiVisualContextHostCallbacks,
};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::{InlineBoxPieceRecord, PaintableData, PaintableSlotId};
use crate::painting::stacking_context::{NO_STACKING_CONTEXT, StackingContextTree};
use crate::painting::visual_context::nested::NestedAssignments;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Default)]
pub struct RecordingOutput {
    pub id: u64,
    pub compatible_visual_context_tree_version: u64,
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
    pub(crate) layout_arena: &'a LayoutNodeArena,
    pub(crate) paintables: &'a PaintableArena,
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
    pub(crate) viewport: PaintableSlotId,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    display_list_id: u64,
    hit_test_list_generation: u64,
    pub(crate) has_blocking_wheel_event_listeners: bool,
    spliced_capture_count: usize,
    uncacheable_paint_generation: u64,
    list: HitTestList,
    base_paint_facts_cache: Vec<Option<(PaintableSlotId, BasePaintFacts)>>,
    paintable_facts_cache: Vec<Option<(PaintableSlotId, FfiHitTestPaintableFacts)>>,
    text_node_facts_cache: HashMap<u32, FfiHitTestTextNodeFacts>,
    font_resource_id_cache: HashMap<usize, u64>,
    text_control_selection_cache: HashMap<u32, crate::painting::host::FfiTextControlSelection>,
    selection_style_cache: HashMap<u32, Rc<paint::text::SelectionStyleAnswer>>,
    pub(crate) wheel_hit_test_target_cache: HashMap<PaintableSlotId, usize>,
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

    pub(crate) fn data(&self, paintable: PaintableSlotId) -> PaintableData {
        self.paintables.data_ref(paintable)
    }

    pub(crate) fn layout_node_shell(&self, paintable: PaintableSlotId) -> *mut std::ffi::c_void {
        self.layout_arena.shell_if_live(self.data(paintable).layout_node)
    }

    pub(crate) fn hit_test_facts(&mut self, paintable: PaintableSlotId) -> FfiHitTestPaintableFacts {
        self.paintable_facts(paintable)
    }

    fn paintable_facts(&mut self, paintable: PaintableSlotId) -> FfiHitTestPaintableFacts {
        let index = paintable.slot_index() as usize;
        if let Some((memoized_id, facts)) = self.paintable_facts_cache[index]
            && memoized_id == paintable
        {
            return facts;
        }
        let facts = self.host.paintable_facts(self.layout_node_shell(paintable));
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
            paintables: self.paintables,
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
            base_paint_facts_cache: vec![None; self.paintables.slot_count()],
            paintable_facts_cache: vec![None; self.paintables.slot_count()],
            text_node_facts_cache: HashMap::new(),
            font_resource_id_cache: HashMap::new(),
            text_control_selection_cache: HashMap::new(),
            selection_style_cache: HashMap::new(),
            wheel_hit_test_target_cache: HashMap::new(),
        }
    }

    pub(crate) fn border_radii(&mut self, paintable: PaintableSlotId) -> BorderRadii {
        let Some(style) = self.layout_arena.node_style_if_live(self.data(paintable).layout_node) else {
            return BorderRadii::default();
        };
        crate::painting::visual_context::node_values::border_radii_data(style, self.paintables, paintable)
    }

    pub(crate) fn piece_border_radii(
        &mut self,
        paintable: PaintableSlotId,
        piece: &InlineBoxPieceRecord,
    ) -> BorderRadii {
        let Some(style) = self.layout_arena.node_style_if_live(self.data(paintable).layout_node) else {
            return BorderRadii::default();
        };
        crate::painting::visual_context::node_values::piece_border_radii_data(
            style,
            piece.border_box_rect.width,
            piece.border_box_rect.height,
            piece.present_edges,
        )
    }

    pub(crate) fn base_paint_facts(&mut self, paintable: PaintableSlotId) -> BasePaintFacts {
        let index = paintable.slot_index() as usize;
        if let Some((memoized_id, facts)) = self.base_paint_facts_cache[index]
            && memoized_id == paintable
        {
            return facts;
        }
        let data = self.data(paintable);
        let Some(style) = self.layout_arena.node_style_if_live(data.layout_node) else {
            let facts = BasePaintFacts::default();
            self.base_paint_facts_cache[index] = Some((paintable, facts));
            return facts;
        };
        let effects = style.effects();
        let is_visible = style.visibility() == crate::css::css_enums::visibility::VISIBLE && effects.opacity != 0.0;
        let empty_cells_property_applies = self.display(paintable).is_internal_table()
            && style.empty_cells() == crate::css::css_enums::empty_cells::HIDE
            && crate::painting::paint_order::first_paint_child(self.layout_arena, self.paintables, paintable).is_none();
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

    fn has_stacking_context(&self, paintable: PaintableSlotId) -> bool {
        self.data(paintable).stacking_context != NO_STACKING_CONTEXT
    }

    fn layout_kind(&self, paintable: PaintableSlotId) -> Option<NodeKind> {
        self.layout_arena.node_kind_if_live(self.data(paintable).layout_node)
    }

    fn display(&self, paintable: PaintableSlotId) -> crate::css::display::FfiDisplay {
        crate::css::display::FfiDisplay::from_raw(self.data(paintable).display)
    }

    fn visibility_is_visible(&self, paintable: PaintableSlotId) -> bool {
        self.layout_arena
            .node_style_if_live(self.data(paintable).layout_node)
            .is_none_or(|style| style.visibility() == css_enums::visibility::VISIBLE)
    }

    pub(crate) fn is_visible(&mut self, paintable: PaintableSlotId) -> bool {
        self.visibility_is_visible(paintable) && !self.paintable_facts(paintable).opacity_is_zero
    }

    pub(crate) fn visible_for_hit_testing(&mut self, paintable: PaintableSlotId) -> bool {
        self.paintable_facts(paintable).visible_for_hit_testing
    }

    fn is_replaced_box(&self, paintable: PaintableSlotId) -> bool {
        self.data(paintable)
            .has_flag(crate::painting::paintable_data::PaintableFlag::ReplacedBox)
    }

    pub(crate) fn own_context_index(&self, paintable: PaintableSlotId) -> usize {
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

    pub(crate) fn own_accumulated_2d_scale(&self, paintable: PaintableSlotId) -> libgfx_rust::FloatSize {
        self.accumulated_2d_scale_at(self.own_context_index(paintable))
    }

    pub(crate) fn is_chrome_mirrored(&self, paintable: PaintableSlotId) -> bool {
        self.layout_arena
            .node_style_if_live(self.data(paintable).layout_node)
            .is_some_and(|style| {
                let writing_mode = style.writing_mode();
                (writing_mode == css_enums::writing_mode::HORIZONTAL_TB
                    && style.direction() == css_enums::direction::RTL)
                    || writing_mode == css_enums::writing_mode::VERTICAL_RL
                    || writing_mode == css_enums::writing_mode::SIDEWAYS_RL
            })
    }
}
