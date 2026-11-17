/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The producers of a frame: each records one kind of output for one row under the contexts it
//! needs, starting and ending at a closed group boundary with no ambient state, so the run of
//! producers a chunk holds can be drawn by a later frame as it is. The recorder implements the
//! assembly host with them.

use super::assemble::{AssemblyEvent, AssemblyHost, ItemCut, ProducerOutcome, ScopeAction, ScopePlan};
use super::order_tree::{ProducerKind, RecordedChunk, RowPlacement};
use super::trace::{Action, Observer, Operation};
use super::window::InkBound;
use super::{PaintPhase, PaintRecorder};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paint_order_plan::{PaintOrderItem, PaintProducer, PaintScope, PaintScopePlan};
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
use crate::painting::record::change_ticks::RowChanges;
use crate::painting::record::paint::background_resolution::root_paint_covers_canvas;
use crate::painting::record::svg_resources::MaskLayerSet;
use crate::painting::{node_painting, paintable_geometry};
use libgfx_rust::Color;
use smallvec::SmallVec;
use std::sync::Arc;

/// The height in CSS pixels of a row of the grid the plans of walked scopes are cut along. A child scope taller
/// than a row is walked on its own, and a run ends where the next item starts in another row, so a run records about
/// a row of the document.
const GRID_ROW_HEIGHT_IN_CSS_PIXELS: i64 = 512;

fn draw_phase(kind: ProducerKind) -> Option<PaintPhase> {
    match kind {
        ProducerKind::DrawBackground => Some(PaintPhase::Background),
        ProducerKind::DrawBorder => Some(PaintPhase::Border),
        ProducerKind::DrawTableCollapsedBorder => Some(PaintPhase::TableCollapsedBorder),
        ProducerKind::DrawForeground => Some(PaintPhase::Foreground),
        ProducerKind::DrawOutline => Some(PaintPhase::Outline),
        ProducerKind::DrawOverlay => Some(PaintPhase::Overlay),
        _ => None,
    }
}

fn draw_kind(phase: PaintPhase) -> ProducerKind {
    match phase {
        PaintPhase::Background => ProducerKind::DrawBackground,
        PaintPhase::Border => ProducerKind::DrawBorder,
        PaintPhase::TableCollapsedBorder => ProducerKind::DrawTableCollapsedBorder,
        PaintPhase::Foreground => ProducerKind::DrawForeground,
        PaintPhase::Outline => ProducerKind::DrawOutline,
        PaintPhase::Overlay => ProducerKind::DrawOverlay,
    }
}

impl<O: Observer> PaintRecorder<'_, O> {
    pub(crate) fn record_canvas(&mut self) -> Color {
        let mut root_group_color = Color::TRANSPARENT;
        self.trace_paint(Operation::Named(None, "canvas"), |this| {
            let inputs = this.inputs;
            let canvas_color = this
                .recorder
                .resolve_color(inputs.uncaptured.canvas_color, ForceDarkRole::Background);
            if let Some(rect) = inputs.canvas_fill_rect {
                this.recorder
                    .fill_rect(rect, inputs.uncaptured.canvas_color, ForceDarkRole::Background);
                root_group_color = canvas_color;
            }
            // .. in the case of embedded documents typically rendered over a transparent canvas
            // (such as provided via an HTML iframe element), if the used color scheme of the element
            // and the used color scheme of the embedded document’s root element do not match,
            // then the UA must use an opaque canvas of the Canvas color appropriate to the
            // embedded document’s used color scheme instead of a transparent canvas.
            if inputs.opaque_canvas {
                this.recorder.fill_rect(
                    inputs.bitmap_rect,
                    inputs.uncaptured.canvas_color,
                    ForceDarkRole::Background,
                );
                root_group_color = canvas_color;
            }
            // The root element paints its background over the canvas as well, wherever its own paint covers the canvas.
            // So there, fill with it only when it's opaque, since painting it twice changes nothing: A translucent one
            // would otherwise be applied twice.
            if inputs.uncaptured.background_color.alpha() == 255
                || !root_paint_covers_canvas(this.source, inputs.uncaptured.root_background_source.root_layout_node)
            {
                this.recorder.fill_rect(
                    inputs.bitmap_rect,
                    inputs.uncaptured.background_color,
                    ForceDarkRole::Background,
                );
            }
            root_group_color = root_group_color.blend(
                this.recorder
                    .resolve_color(inputs.uncaptured.background_color, ForceDarkRole::Background),
            );
        });
        root_group_color
    }

    // The transparent fill that triggers a content-generating SVG filter, and the mask
    // contents of a stacking context, recorded before its content.
    fn record_scope_preamble(&mut self, owner: NodeSlotId) {
        let context = self.own_context(owner);
        self.recorder.set_accumulated_visual_context(context);
        if let Some(svg_filter_bounds) = self.source.committed_side_data(owner).svg_filter_bounds {
            let device_rect = self
                .converter
                .enclosing_device_rect(CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }
        self.declare_mask_contents(owner, MaskLayerSet::CssAndSvg);
        self.recorder.set_accumulated_visual_context(ContextRef::default());
    }

    fn record_scroll_metadata(&mut self, owner: NodeSlotId) {
        let context = self.own_context(owner);
        self.recorder.set_accumulated_visual_context(context);
        self.record_async_scrolling_metadata(owner);
        self.recorder.set_accumulated_visual_context(ContextRef::default());
    }

    fn record_box_commands(&mut self, owner: NodeSlotId, phase: PaintPhase) {
        let facts = self.base_paint_facts(owner);
        // Scrolling repaints without stamping any row, so output that depends on the scroll
        // offset records every frame.
        if facts.has_fixed_background
            || (facts.has_scroll_offset_dependent_background && phase == PaintPhase::Background)
        {
            self.mark_live_producer();
        }
        let context = self.context_for_phase(owner, phase);
        self.recorder.set_accumulated_visual_context(context);
        self.paint(owner, phase);
        self.recorder.set_accumulated_visual_context(ContextRef::default());
    }

    fn record_svg_box_foreground(&mut self, owner: NodeSlotId) {
        self.paint_svg_box_impl(owner);
        self.recorder.set_accumulated_visual_context(ContextRef::default());
    }

    fn producer_is_skipped(&mut self, owner: NodeSlotId, kind: ProducerKind) -> bool {
        draw_phase(kind).is_some_and(|phase| self.base_paint_facts(owner).paint_phase_mask & phase.bit() == 0)
    }

    /// The commands of a box phase inside a resource walk, which records into the referencing
    /// producer rather than into an entry of its own.
    pub(crate) fn record_box_phase_inside_resource(&mut self, owner: NodeSlotId, phase: PaintPhase) {
        debug_assert!(self.is_recording_svg_resource_content());
        let kind = draw_kind(phase);
        if self.producer_is_skipped(owner, kind) {
            self.observer
                .observe(|log| log.leaf(Operation::Producer(owner, kind), Action::Skip, true));
            return;
        }
        let context = self.context_for_phase(owner, phase);
        self.recorder.set_accumulated_visual_context(context);
        self.trace_paint(Operation::Producer(owner, kind), |this| this.paint(owner, phase));
    }
}

impl<O: Observer> AssemblyHost for PaintRecorder<'_, O> {
    fn plan_scope(&mut self, scope: PaintScope) -> ScopePlan {
        let plan = PaintScopePlan::build(self.source, scope, self.inputs.should_paint_overlay);
        let active = !plan.establishes_stacking_context
            || crate::painting::hit_test::walk::stacking_context_paints(self.source, scope.owner);
        ScopePlan {
            active,
            items: if active { plan.items } else { SmallVec::new() },
        }
    }

    fn record_producer(&mut self, owner: NodeSlotId, kind: ProducerKind) -> ProducerOutcome {
        debug_assert!(
            self.recorder.is_producer_boundary(),
            "a producer starts at a closed group boundary without ambient state"
        );
        if self.producer_is_skipped(owner, kind) {
            self.observer
                .observe(|log| log.leaf(Operation::Producer(owner, kind), Action::Skip, true));
            return ProducerOutcome::default();
        }
        self.live_producer = false;
        self.producer_reads_window_focus.set(false);
        self.producer_reads_accumulated_scale.set(false);
        self.recorder
            .begin_record_origin(owner, super::trace::producer_name(kind));
        let start = self.recorder.byte_size();
        let blocking_wheel_event_regions_before = self.blocking_wheel_event_region_count;
        self.observer
            .observe(|log| log.begin(Operation::Producer(owner, kind), Action::Record));
        match kind {
            ProducerKind::ScopePreamble => self.record_scope_preamble(owner),
            ProducerKind::ScrollMetadata => self.record_scroll_metadata(owner),
            ProducerKind::Svg => self.record_svg_box_foreground(owner),
            ProducerKind::InlinePiece(index) | ProducerKind::TextFragment(index) => {
                let root = if crate::painting::node_painting::has_lines(self.source, owner) {
                    owner
                } else {
                    self.data(owner).containing_block
                };
                if matches!(kind, ProducerKind::InlinePiece(_)) {
                    super::paint::inline_box::paint_piece(self, root, index);
                } else {
                    let context = self.context_for_phase(owner, PaintPhase::Foreground);
                    self.recorder.set_accumulated_visual_context(context);
                    super::paint::text::paint_fragments(self, root, &[index]);
                    self.recorder.set_accumulated_visual_context(ContextRef::default());
                }
            }
            _ => {
                if let Some(phase) = draw_phase(kind) {
                    self.record_box_commands(owner, phase);
                }
            }
        }
        let end = self.recorder.byte_size();
        self.recorder.end_record_origin();
        if self.ink_bounds_audit.enabled {
            self.verify_producer_ink(owner, kind, start..end);
            self.verify_producer_against_open_scopes(owner, kind, start..end);
        }
        self.observer.observe(|log| log.end(end == start));
        debug_assert!(
            self.recorder.is_producer_boundary(),
            "a producer ends at a closed group boundary without ambient state"
        );
        ProducerOutcome {
            live: self.live_producer,
            reads_window_focus: self.producer_reads_window_focus.get(),
            reads_accumulated_scale: self.producer_reads_accumulated_scale.get(),
            blocking_wheel_event_regions: self.blocking_wheel_event_region_count - blocking_wheel_event_regions_before,
        }
    }

    fn take_run_chunk(&mut self) -> Option<Arc<RecordedChunk>> {
        let chunk = self.recorder.take_chunk();
        let record_notes = self.recorder.take_record_notes();
        let draws_vector_image_placeholders = std::mem::take(&mut self.run_draws_vector_image_placeholders);
        if chunk.records.is_empty() {
            return None;
        }
        let chunk = RecordedChunk::new(chunk, draws_vector_image_placeholders);
        if let Some(record_notes) = record_notes {
            self.record_notes_of_chunks.push((Arc::clone(&chunk), record_notes));
        }
        Some(chunk)
    }

    fn changes_of_row(&mut self, row: NodeSlotId) -> RowChanges {
        let Some(published) = &self.source_recording else {
            return RowChanges::NONE;
        };
        self.source
            .paint_change_stamps(row)
            .changes_since(published.recorded_as_of)
    }

    fn placement_of_row(&mut self, row: NodeSlotId) -> RowPlacement {
        use crate::painting::paintable_data::PaintableFlag;
        use std::hash::Hasher;
        // A fragmented inline is placed by the pieces its line root holds, and the offset of its own fragment is
        // the one layout left there last.
        let placed_with = if node_painting::is_fragmented_inline(self.source, row) {
            self.source.inline_pieces_root(row).unwrap_or(row)
        } else {
            row
        };
        let mut later_fragments = 0;
        if self
            .source
            .paintable_data(placed_with)
            .has_flag(PaintableFlag::HasLaterBoxFragments)
        {
            let mut digest = crate::fast_hash::fast_hasher();
            for fragment in paintable_geometry::absolute_border_box_rects_of_fragments(self.source, placed_with)
                .iter()
                .skip(1)
            {
                for coordinate in [fragment.x, fragment.y, fragment.width, fragment.height] {
                    digest.write_i32(coordinate.raw_value());
                }
            }
            later_fragments = digest.finish();
        }
        RowPlacement {
            origin: paintable_geometry::absolute_rect(self.source, placed_with).location(),
            later_fragments,
        }
    }

    fn cut_at(&mut self, scope: PaintScope, item: PaintOrderItem) -> ItemCut {
        let rows = self.source;
        let grid_row_height = CssPixels::from_integer(if self.inputs.records_in_finest_runs {
            1
        } else {
            GRID_ROW_HEIGHT_IN_CSS_PIXELS
        });
        let grid_row_at = |block_start: CssPixels| {
            Some(i64::from(block_start.raw_value()).div_euclid(i64::from(grid_row_height.raw_value())) as i32)
        };
        let line_root = || {
            if node_painting::has_lines(rows, scope.owner) {
                scope.owner
            } else {
                rows.paintable_data(scope.owner).containing_block
            }
        };
        match item {
            PaintOrderItem::Scope(child) => {
                // The pieces of an inline box are spread over its line root's lines: it stays with what is next
                // to it in the plan.
                if node_painting::is_fragmented_inline(rows, child.owner) {
                    return ItemCut::InRun { grid_row: None };
                }
                if crate::painting::style_queries::is_scroll_container(rows, child.owner) {
                    return ItemCut::Walked;
                }
                match rows.absolute_ink_overflow_rect(child.owner) {
                    Some(ink) if ink.height <= grid_row_height => ItemCut::InRun {
                        grid_row: grid_row_at(ink.y),
                    },
                    _ => ItemCut::Walked,
                }
            }
            PaintOrderItem::Producer(PaintProducer::TextFragment(index)) => {
                let side = rows.committed_side_data(line_root());
                let fragment = &side.fragments()[index as usize];
                ItemCut::InRun {
                    grid_row: grid_row_at(crate::painting::text_fragment::absolute_rect(rows, fragment).y),
                }
            }
            PaintOrderItem::Producer(PaintProducer::InlinePiece(index)) => {
                let root = line_root();
                let positions = self.line_root_positions(root);
                let side = rows.committed_side_data(root);
                let piece = &side.inline_box_pieces()[index as usize];
                let line = positions.of_line(side.lines(), piece.line_index as usize);
                ItemCut::InRun {
                    grid_row: grid_row_at(piece.border_box_rect.y + line.y),
                }
            }
            PaintOrderItem::Producer(_) => ItemCut::InRun { grid_row: None },
        }
    }

    // A child scope is bounded by its owner's ink overflow, in the space its own box is painted in, unless a transform
    // below that box moves some of it, and the text fragments and inline box pieces of a line root by what each draws.
    // The other producers of the scope's owner are not bounded unless they paint nothing: the owner of a walked scope
    // is in the window when the scope is, and its own producers mostly cover it.
    fn bound_of_item(&mut self, scope: PaintScope, item: PaintOrderItem) -> Option<InkBound> {
        use crate::painting::ink_overflow::{ScopeInkBound, inline_piece_ink, scope_ink_bound, text_fragment_ink_rect};
        let line_root = |rows: &PaintSource<'_>| {
            if node_painting::has_lines(rows, scope.owner) {
                scope.owner
            } else {
                rows.paintable_data(scope.owner).containing_block
            }
        };
        let (rect, space) = match item {
            PaintOrderItem::Scope(child) => {
                let ScopeInkBound::Rect(bound) = scope_ink_bound(self.source, child) else {
                    return None;
                };
                // The bound says where a transform below the child's own box has put what it moves, and nothing records
                // again when the transform puts it elsewhere.
                if self
                    .source
                    .stored_ink_overflow(child.owner)
                    .holds_what_a_transform_moves
                {
                    return None;
                }
                (bound, self.own_context(child.owner).spatial)
            }
            PaintOrderItem::Producer(PaintProducer::TextFragment(index)) => {
                let rect = text_fragment_ink_rect(self.source, self.paint_state, line_root(self.source), index);
                if rect.is_empty() {
                    return None;
                }
                (
                    rect,
                    self.context_for_phase(scope.owner, PaintPhase::Foreground).spatial,
                )
            }
            PaintOrderItem::Producer(PaintProducer::InlinePiece(index)) => {
                let root = line_root(self.source);
                let positions = self.line_root_positions(root);
                let ink = inline_piece_ink(self.source, root, &positions, index);
                if ink.unbounded {
                    return None;
                }
                let piece_node = self.source.committed_side_data(root).inline_box_pieces()[index as usize].node;
                (
                    ink.rect,
                    self.context_for_phase(piece_node, PaintPhase::Background).spatial,
                )
            }
            PaintOrderItem::Producer(producer) => {
                let paints_nothing = crate::painting::record::assemble::producer_kinds(producer)
                    .into_iter()
                    .all(|kind| self.producer_is_skipped(scope.owner, kind));
                return paints_nothing.then(InkBound::nothing);
            }
        };
        let slack = super::ink_bounds::COMPARISON_SLACK_IN_DEVICE_PIXELS;
        Some(InkBound::in_space(
            space,
            self.converter
                .enclosing_device_rect(rect)
                .inflated_edges(slack, slack, slack, slack)
                .to_float(),
        ))
    }

    fn is_outside_window(&mut self, bound: &InkBound) -> bool {
        self.window_spaces
            .as_ref()
            .is_some_and(|spaces| spaces.is_outside(bound))
    }

    fn is_wholly_inside_window(&mut self, bound: &InkBound) -> bool {
        self.window_spaces
            .as_ref()
            .is_some_and(|spaces| spaces.is_wholly_inside(bound))
    }

    // Any box whose overflow is not visible, though not every one of them clips: saying so of a box that does not
    // only has its scopes ask what bounds their segments.
    fn cuts_off_what_it_holds(&mut self, owner: NodeSlotId) -> bool {
        use crate::css::css_enums::overflow;
        self.source
            .node_style_if_live(owner)
            .is_some_and(|style| style.overflow_x() != overflow::VISIBLE || style.overflow_y() != overflow::VISIBLE)
    }

    // SVG content is recorded by its root.
    fn producer_reads_descendants(&mut self, owner: NodeSlotId, kind: ProducerKind) -> bool {
        match kind {
            ProducerKind::Svg => true,
            ProducerKind::ScopePreamble => self.paints_svg_mask_or_clip_resource_subtree(owner),
            ProducerKind::InlinePiece(_) | ProducerKind::TextFragment(_) => true,
            _ => false,
        }
    }

    fn observe(&mut self, event: AssemblyEvent) {
        if self.ink_bounds_audit.enabled {
            self.audit_scope_event(event);
        }
        if !O::ENABLED {
            return;
        }
        self.observer.observe(|log| match event {
            AssemblyEvent::ScopeBegin(scope, action) => log.begin(
                Operation::Scope(scope),
                match action {
                    ScopeAction::Assembled => Action::Assemble,
                    ScopeAction::Replanned => Action::Replan,
                    ScopeAction::Recorded => Action::Record,
                    ScopeAction::Inactive => Action::Skip,
                },
            ),
            AssemblyEvent::ScopeEnd(_) => log.end(false),
            AssemblyEvent::ScopeCopied(scope) => log.leaf(Operation::Scope(scope), Action::Copy, false),
            AssemblyEvent::ProducerCopied(owner, kind) => {
                log.leaf(Operation::Producer(owner, kind), Action::Copy, false);
            }
            AssemblyEvent::ScopeLeftOut(scope) => log.leaf(Operation::Scope(scope), Action::LeaveOut, false),
            AssemblyEvent::ProducerLeftOut(owner, kind) => {
                log.leaf(Operation::Producer(owner, kind), Action::LeaveOut, false);
            }
        });
    }
}
