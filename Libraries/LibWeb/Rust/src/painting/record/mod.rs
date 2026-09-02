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
pub(crate) mod scratch;
pub mod traversal;
pub(crate) mod verify;

use crate::css::css_enums;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::builder::{CommandRange, PendingInlineClip, RecordedDisplayList};
use crate::painting::display_list::commands::{ContextRef, DisplayListResourceId, FrameNodeIndex, SpatialNodeIndex};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::hit_test::HitTestList;
use crate::painting::host::{
    FfiHitTestHostCallbacks, FfiHitTestTextNodeFacts, FfiPaintHostCallbacks, FfiPaintRecordingStats,
    FfiRecordingInputs, FfiVisualContextHostCallbacks,
};
use crate::painting::paintable_data::{InlineBoxPieceRecord, PaintableData};
use crate::painting::paintable_rows::PaintableRowsRef;
use crate::painting::record::cache::{OpenCapture, RecordGen};
use crate::painting::visual_context::nested::NestedAssignments;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Default)]
pub struct RecordingOutput {
    pub recorded_structural_epoch: u64,
    // A default-constructed output's 0.0 never matches a real recording scale.
    pub recorded_device_pixels_per_css_pixel: f64,
    pub hit_test_list: HitTestList,
    pub display_list: Rc<RecordedDisplayList>,
    pub has_blocking_wheel_event_listeners: bool,
    pub wheel_event_listener_state_generation: u64,
    pub mask_display_lists: Vec<(FrameNodeIndex, DisplayListResourceId)>,
    pub recording_stats: FfiPaintRecordingStats,
    pub is_identical_to_cache_source: bool,
    pub(crate) capture_log_for_verification: Option<verify::CaptureLog>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
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

pub(crate) struct DeferredWholeTapeSplice {
    pub(crate) source_display_list: Rc<RecordedDisplayList>,
    pub(crate) prologue_byte_count: usize,
    pub(crate) source_range: CommandRange,
}

pub struct PaintRecorder<'a> {
    pub(crate) layout_arena: &'a PaintableRowsRef<'a>,
    pub(crate) paint_state: &'a crate::painting::paint_state::PaintState,
    pub(crate) host: &'a FfiHitTestHostCallbacks,
    pub(crate) paint_host: &'a FfiPaintHostCallbacks,
    pub(crate) inputs: FfiRecordingInputs,
    pub(crate) recorder: DisplayListRecorder,
    pub(crate) converter: DevicePixelConverter,
    pub(crate) draw_svg_geometry_for_clip_path: bool,
    pub(crate) visual_context_host: &'a FfiVisualContextHostCallbacks,
    pub(crate) nested: Option<NestedRecordingState>,
    pub(crate) nested_tree: Option<crate::painting::visual_context::VisualContextTree>,
    pub(crate) recording_into_context_free_nested_list: bool,
    pub(crate) prerecorded: crate::painting::record::masks::PrerecordedNestedDisplayLists,
    pub(crate) viewport: NodeSlotId,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    hit_test_list_generation: u64,
    open_capture_stack: Vec<OpenCapture>,
    deferred_whole_tape_splice: Option<DeferredWholeTapeSplice>,
    pub(crate) blocking_wheel_event_region_count: u32,
    pub(crate) recording_stats: FfiPaintRecordingStats,
    uncacheable_paint_generation: u64,
    pub(crate) capture_log_for_verification: Option<verify::CaptureLog>,
    list: HitTestList,
    pub(crate) memo_tables: &'a RefCell<scratch::PerRecordingMemoTables>,
    pub(crate) completed_record_gen: RecordGen,
    pub(crate) all_paint_caches_dirty: bool,
    pub(crate) all_descendant_subtree_caches_dirty: bool,
    text_node_facts_cache: HashMap<u32, FfiHitTestTextNodeFacts>,
    font_resource_id_cache: HashMap<usize, u64>,
    text_control_selection_cache: HashMap<u32, crate::painting::host::FfiTextControlSelection>,
    selection_style_cache: HashMap<u32, Rc<paint::text::SelectionStyleAnswer>>,
    pub(crate) wheel_hit_test_target_cache: HashMap<NodeSlotId, SpatialNodeIndex>,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct BasePaintFacts {
    pub is_visible: bool,
    pub empty_cells_property_applies: bool,
    pub has_backdrop_filter: bool,
    pub paints_border_image: bool,
}

impl<'a> PaintRecorder<'a> {
    pub(crate) fn mark_open_captures_unsplicable(&mut self) {
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
        if let Some(facts) = self.memo_tables.borrow().hit_test_facts(paintable) {
            return facts;
        }
        let dom_facts = self.host.paintable_facts(self.layout_node_shell(paintable));
        let facts = hit_test_items::hit_test_facts(self.layout_arena, paintable, &self.inputs, dom_facts);
        self.memo_tables.borrow_mut().set_hit_test_facts(paintable, facts);
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
            host: self.host,
            paint_host: self.paint_host,
            inputs: self.inputs,
            recorder,
            converter: self.converter,
            draw_svg_geometry_for_clip_path,
            visual_context_host: self.visual_context_host,
            nested,
            nested_tree,
            recording_into_context_free_nested_list: false,
            prerecorded: crate::painting::record::masks::PrerecordedNestedDisplayLists::default(),
            viewport: self.viewport,
            command_cache_source: None,
            item_cache_source: None,
            hit_test_list_generation: self.hit_test_list_generation,
            open_capture_stack: Vec::new(),
            deferred_whole_tape_splice: None,
            blocking_wheel_event_region_count: 0,
            recording_stats: FfiPaintRecordingStats::default(),
            uncacheable_paint_generation: 0,
            capture_log_for_verification: None,
            list: HitTestList::default(),
            memo_tables: self.memo_tables,
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
        if let Some(facts) = self.memo_tables.borrow().base_paint_facts(paintable) {
            return facts;
        }
        let Some(style) = self.layout_arena.node_style_if_live(paintable) else {
            let facts = BasePaintFacts::default();
            self.memo_tables.borrow_mut().set_base_paint_facts(paintable, facts);
            return facts;
        };
        let effects = style.effects();
        let retains_animated_content =
            self.layout_arena.node_flags_if_live(paintable) & NodeFlag::HasAnimatedOpacityOrTransform as u32 != 0;
        let is_visible = style.visibility() == crate::css::css_enums::visibility::VISIBLE
            && (effects.opacity != 0.0 || retains_animated_content);
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
        self.memo_tables.borrow_mut().set_base_paint_facts(paintable, facts);
        facts
    }

    fn has_stacking_context(&self, paintable: NodeSlotId) -> bool {
        self.data(paintable).establishes_stacking_context
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

    pub(crate) fn with_context<R>(&mut self, context: ContextRef, paint: impl FnOnce(&mut Self) -> R) -> R {
        let previous_context = self.recorder.accumulated_visual_context();
        self.recorder.set_accumulated_visual_context(context);
        let result = paint(self);
        self.recorder.set_accumulated_visual_context(previous_context);
        result
    }

    pub(crate) fn record_with_inline_clips<R>(
        &mut self,
        inline_clips: &[PendingInlineClip],
        paint: impl FnOnce(&mut Self) -> R,
    ) -> R {
        let enclosing_scope_clip_count = self.recorder.ambient_inline_clip_depth();
        self.recorder.push_ambient_inline_clips(inline_clips);
        let result = paint(self);
        self.recorder.truncate_ambient_inline_clips(enclosing_scope_clip_count);
        result
    }

    pub(crate) fn own_context(&self, paintable: NodeSlotId) -> ContextRef {
        if let Some(nested) = &self.nested
            && let Some((own, _)) = nested.assignments.paintable_contexts.get(&paintable.index)
        {
            return *own;
        }
        self.data(paintable).accumulated_visual_context
    }

    pub(crate) fn accumulated_2d_scale_at(&self, spatial: SpatialNodeIndex) -> libgfx_rust::FloatSize {
        let tree = self
            .nested_tree
            .as_ref()
            .or(self.paint_state.visual_context.tree.as_deref())
            .expect("recording runs against a visual context tree");
        tree.accumulated_2d_scale(
            spatial,
            &[],
            crate::painting::visual_context::IncludeVisualViewportTransform::No,
        )
    }

    pub(crate) fn own_accumulated_2d_scale(&self, paintable: NodeSlotId) -> libgfx_rust::FloatSize {
        self.accumulated_2d_scale_at(self.own_context(paintable).spatial)
    }
}
