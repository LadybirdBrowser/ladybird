/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::{Observer, Operation};

pub mod async_scroll_metadata;
pub mod cache;
pub mod hit_test_items;
pub mod paint;
pub(crate) mod publish;
pub(crate) mod scratch;
pub mod svg_resources;
pub mod trace;
pub mod traversal;
pub(crate) mod verify;

use crate::css::css_enums;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::builder::{CommandRange, PendingInlineClip, RecordedDisplayList};
use crate::painting::display_list::commands::{ContextRef, SpatialNodeIndex};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::hit_test::HitTestList;
use crate::painting::host::{
    FfiHitTestHostCallbacks, FfiPaintHostCallbacks, FfiRecordingInputs, FfiRootBackgroundSource,
    FfiVisualContextHostCallbacks, FfiVisualContextTreeInputs,
};
use crate::painting::paintable_data::{InlineBoxPieceRecord, PaintableData};
use crate::painting::paintable_rows::PaintableRowsRef;
use crate::painting::record::cache::{OpenCapture, RecordGen};
use crate::painting::record::svg_resources::SvgResourceWalk;
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

#[derive(Clone, Copy)]
pub(crate) struct RecordingInputs {
    pub(crate) host: FfiRecordingInputs,
    pub(crate) device_pixels_per_css_pixel: f64,
    pub(crate) viewport_wheel_overflow_x: u8,
    pub(crate) viewport_wheel_overflow_y: u8,
    pub(crate) root_background_source: FfiRootBackgroundSource,
}

impl RecordingInputs {
    pub(crate) fn from_host_and_last_visual_context_update(
        host: FfiRecordingInputs,
        tree_inputs: FfiVisualContextTreeInputs,
        root_background_source: FfiRootBackgroundSource,
    ) -> Self {
        Self {
            host,
            device_pixels_per_css_pixel: tree_inputs.device_pixels_per_css_pixel,
            viewport_wheel_overflow_x: tree_inputs.viewport_wheel_overflow_x,
            viewport_wheel_overflow_y: tree_inputs.viewport_wheel_overflow_y,
            root_background_source,
        }
    }
}

impl std::ops::Deref for RecordingInputs {
    type Target = FfiRecordingInputs;

    fn deref(&self) -> &FfiRecordingInputs {
        &self.host
    }
}

impl std::ops::DerefMut for RecordingInputs {
    fn deref_mut(&mut self) -> &mut FfiRecordingInputs {
        &mut self.host
    }
}

#[derive(Default)]
pub struct RecordingOutput {
    pub recorded_structural_epoch: u64,
    // A default-constructed output's 0.0 never matches a real recording scale.
    pub recorded_device_pixels_per_css_pixel: f64,
    pub hit_test_list: HitTestList,
    pub display_list: Rc<RecordedDisplayList>,
    pub has_blocking_wheel_event_listeners: bool,
    pub wheel_event_listener_state_generation: u64,
    pub is_identical_to_cache_source: bool,
    pub(crate) capture_log_for_verification: Option<verify::CaptureLog>,
    pub(crate) newly_referenced_fonts: Vec<libgfx_rust::font::FontHandle>,
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

    const fn bit(self) -> u8 {
        1 << self as u8
    }
}
pub(crate) struct DeferredWholeTapeSplice {
    pub(crate) source_display_list: Rc<RecordedDisplayList>,
    pub(crate) prologue_byte_count: usize,
    pub(crate) source_range: CommandRange,
}

pub struct PaintRecorder<'a, O: Observer> {
    pub(crate) layout_arena: &'a PaintableRowsRef<'a>,
    pub(crate) paint_state: &'a crate::painting::paint_state::PaintState,
    pub(crate) host: &'a FfiHitTestHostCallbacks,
    pub(crate) paint_host: &'a FfiPaintHostCallbacks,
    pub(crate) inputs: RecordingInputs,
    pub(crate) recorder: DisplayListRecorder,
    pub(crate) converter: DevicePixelConverter,
    pub(crate) visual_context_host: &'a FfiVisualContextHostCallbacks,
    pub(crate) svg_resource_walk: Option<SvgResourceWalk>,
    pattern_tile_records: HashMap<PatternTileKey, Rc<Vec<u8>>>,
    pub(crate) viewport: NodeSlotId,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    open_capture_stack: Vec<OpenCapture>,
    deferred_whole_tape_splice: Option<DeferredWholeTapeSplice>,
    pub(crate) blocking_wheel_event_region_count: u32,
    uncacheable_paint_generation: u64,
    pub(crate) observer: O,
    list: HitTestList,
    pub(crate) memo_tables: &'a RefCell<scratch::PerRecordingMemoTables>,
    pub(crate) completed_record_gen: RecordGen,
    pub(crate) all_paint_caches_dirty: bool,
    pub(crate) all_descendant_subtree_caches_dirty: bool,
    registered_font_ids: HashSet<libgfx_rust::font::FontId>,
    newly_referenced_fonts: Vec<libgfx_rust::font::FontHandle>,
    selection_style_cache: HashMap<u32, Rc<paint::text::SelectionStyleAnswer>>,
    pub(crate) wheel_hit_test_target_cache: HashMap<NodeSlotId, SpatialNodeIndex>,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct BasePaintFacts {
    pub is_visible: bool,
    pub empty_cells_property_applies: bool,
    pub has_backdrop_filter: bool,
    pub has_box_shadow: bool,
    pub paints_border_image: bool,
    pub paint_phase_mask: u8,
}

impl<O: Observer> PaintRecorder<'_, O> {
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
        let facts = hit_test_items::hit_test_facts(self.layout_arena, paintable, &self.inputs);
        self.memo_tables.borrow_mut().set_hit_test_facts(paintable, facts);
        facts
    }

    pub(crate) fn register_font(&mut self, font: &libgfx_rust::font::FontHandle) -> u64 {
        if self.registered_font_ids.insert(font.id()) {
            self.newly_referenced_fonts.push(font.clone());
        }
        font.id().0
    }

    pub(crate) fn own_scroll_container_offset(&self, paintable: NodeSlotId) -> crate::css::css_pixels::CssPixelPoint {
        use crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX;
        let own_scroll_node = self.data(paintable).own_scroll_node_index;
        if own_scroll_node == VISUAL_VIEWPORT_NODE_INDEX {
            return crate::css::css_pixels::CssPixelPoint::default();
        }
        let visual_context = &self.paint_state.visual_context;
        let Some(tree) = visual_context.tree.as_ref() else {
            return crate::css::css_pixels::CssPixelPoint::default();
        };
        let slot = tree.scroll_state_slot_for_node(own_scroll_node);
        let own_offset = visual_context.scroll_state.state_at_slot(slot).own_offset;
        crate::css::css_pixels::CssPixelPoint::new(-own_offset.x, -own_offset.y)
    }

    /// The focused text control's selection as `(start, end)` when `node` is one of its text
    /// node's committed rows.
    pub(crate) fn text_control_selection(&self, node: NodeSlotId) -> Option<(usize, usize)> {
        let control = &self.inputs.focused_text_control;
        if control.start == control.end {
            return None;
        }
        self.layout_arena
            .text_fragments(control.text_node)
            .as_slice()
            .contains(&node)
            .then_some((control.start, control.end))
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
        let mut facts = BasePaintFacts {
            is_visible,
            empty_cells_property_applies,
            has_backdrop_filter,
            has_box_shadow: effects.box_shadows.length != 0,
            paints_border_image,
            paint_phase_mask: 0,
        };
        facts.paint_phase_mask = paint::paint_phase_mask(self, paintable, style, &facts);
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
        if let Some(walk) = &self.svg_resource_walk {
            return walk.enclosing_context;
        }
        self.data(paintable).accumulated_visual_context
    }

    pub(crate) fn is_recording_svg_resource_content(&self) -> bool {
        self.svg_resource_walk.is_some()
    }

    pub(crate) fn draws_clip_path_geometry(&self) -> bool {
        self.svg_resource_walk.is_some_and(|walk| walk.draws_clip_path_geometry)
    }

    pub(crate) fn accumulated_2d_scale_at(&self, spatial: SpatialNodeIndex) -> libgfx_rust::FloatSize {
        if self.svg_resource_walk.is_some() {
            let transform = self
                .recorder
                .ambient_inline_transform()
                .unwrap_or_else(libgfx_rust::AffineTransform::identity);
            return libgfx_rust::FloatSize {
                width: transform.x_scale(),
                height: transform.y_scale(),
            };
        }
        let tree = self
            .paint_state
            .visual_context
            .tree
            .as_deref()
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

    pub(crate) fn pattern_tile_records(
        &mut self,
        pattern: NodeSlotId,
        tile_content_transform: libgfx_rust::FloatMatrix4x4,
    ) -> Rc<Vec<u8>> {
        let root_transform = tile_content_transform.extract_2d_affine();
        let key = PatternTileKey {
            pattern: pattern.index,
            root_transform_bits: root_transform.values.map(f32::to_bits),
        };
        if let Some(records) = self.pattern_tile_records.get(&key) {
            return records.clone();
        }
        let detached = self.recorder.begin_detached_records();
        // Pattern tiles exclude the root's own transform: patternTransform reaches the replay-side
        // tile shader instead, so the tiling grid repeats under it rather than the content scaling
        // twice.
        self.trace_paint(Operation::Producer(Some(pattern), "svg-pattern"), |this| {
            this.walk_svg_resource(pattern, root_transform, false, false);
        });
        let records = Rc::new(self.recorder.finish_detached_records(detached));
        self.pattern_tile_records.insert(key, records.clone());
        records
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct PatternTileKey {
    pattern: u32,
    root_transform_bits: [u32; 6],
}
