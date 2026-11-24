/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Hit testing by walking the document's paint order over its rows.
//!
//! The walk follows the plans the display list recorder records by, so what it meets is in paint order, and a point
//! is answered by walking that order backwards: the first item under the point is the topmost one. A scope whose
//! owner's ink overflow bounds it is skipped when the point lies outside the bound.

use super::items::{HitTestFacts, HitTestItemSink, hit_test_facts, hit_test_items_of_phase};
use super::query::{DepthSortingState, item_contains_point};
use super::*;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::painting::ink_overflow::HitTestBoundOfLine;
use crate::painting::paint_order_plan::{
    PaintOrderInputs, PaintOrderItem, PaintProducer, PaintScope, PaintScopeKind, PaintScopePlan, PlanLeavesOut,
};
use crate::painting::paint_read::PaintRead;
use crate::painting::paintable_data::PaintableFlag;
use crate::painting::record::PaintPhase;
use crate::painting::viewport_mapping::{ViewportMapping, ViewportSpace};
use crate::painting::visual_context::{
    ClipBehavior, IncludeVisualViewportTransform, SpatialNodeIndex, VisualContextTree, WorldTransforms,
};
use crate::painting::{node_painting, paintable_geometry};
use libgfx_rust::FloatPoint;
use smallvec::SmallVec;

// https://drafts.csswg.org/css-transforms-1/#transform-function-lists
// If a transform function causes the current transformation matrix of an object to be
// non-invertible, the object and its content do not get displayed. Content whose transform
// is animated is retained so the compositor can reveal it without a main-thread repaint.
pub(crate) fn stacking_context_paints(rows: &impl PaintRead, owner: NodeSlotId) -> bool {
    !(rows
        .paintable_data(owner)
        .has_flag(PaintableFlag::HasNonInvertibleCssTransform)
        && rows.node_flags_if_live(owner) & NodeFlag::HasAnimatedOpacityOrTransform as u32 == 0)
}

/// Whether the SVG box paints nothing, and so can be hit nowhere, because a mask or clip path it references covers
/// no area.
fn svg_box_is_hidden_by_an_empty_mask_or_clip(rows: &impl PaintRead, svg_box: NodeSlotId) -> bool {
    rows.paintable_data(svg_box)
        .has_flag(PaintableFlag::HiddenByEmptySvgMaskOrClip)
}

/// The boxes below an SVG box that its producer walks: those a stacking context of their own does not paint.
fn svg_children_painted_with_their_parent(rows: &impl PaintRead, svg_box: NodeSlotId) -> SmallVec<[NodeSlotId; 8]> {
    let mut children = SmallVec::new();
    let mut next_child = crate::painting::paint_order::first_paint_child(rows, svg_box);
    while let Some(child) = next_child {
        next_child = crate::painting::paint_order::next_paint_sibling(rows, child);
        if !rows.paintable_data(child).establishes_stacking_context {
            children.push(child);
        }
    }
    children
}

/// What a producer of the paint order can be hit by: the items of one phase of its box, or an SVG box with the boxes
/// painted with it.
#[derive(Clone, Copy)]
enum HitTestedPart {
    Phase(PaintPhase),
    SvgBox,
}

/// What `producer` can be hit by, in paint order.
fn hit_tested_parts(producer: PaintProducer) -> SmallVec<[HitTestedPart; 2]> {
    use HitTestedPart::*;
    match producer {
        PaintProducer::BoxPhase(phase @ (PaintPhase::Background | PaintPhase::Foreground | PaintPhase::Overlay)) => {
            smallvec::smallvec![Phase(phase)]
        }
        PaintProducer::SvgRoot => smallvec::smallvec![Phase(PaintPhase::Background), SvgBox],
        PaintProducer::SvgBoxForeground => smallvec::smallvec![SvgBox],
        PaintProducer::ForegroundHitTest => smallvec::smallvec![Phase(PaintPhase::Foreground)],
        PaintProducer::BoxPhase(_)
        | PaintProducer::ForegroundCommands
        | PaintProducer::InlinePiece(_)
        | PaintProducer::TextFragment(_)
        | PaintProducer::ScopePreamble => SmallVec::new(),
    }
}

// The scale of a space does not depend on where anything is scrolled to, so any table of the tree's spaces gives it.
fn own_accumulated_2d_scale(
    rows: &impl PaintRead,
    spaces: &WorldTransforms,
    paintable: NodeSlotId,
) -> libgfx_rust::FloatSize {
    spaces.accumulated_2d_scale(
        rows.paintable_data(paintable).accumulated_visual_context.spatial,
        IncludeVisualViewportTransform::No,
    )
}

struct WhatThePointMisses<'walk, 'a, R: PaintRead> {
    walk: &'walk mut PointWalk<'a, R>,
    owner: PlacedOwner,
}

impl<R: PaintRead> PlanLeavesOut for WhatThePointMisses<'_, '_, R> {
    const LEAVES_OUT_THE_SAME_CHILD_SCOPES_IN_EVERY_PHASE: bool = true;

    fn leaves_out_child_scope(&mut self, child: PaintScope, child_inputs: PaintOrderInputs) -> bool {
        self.walk.point_misses_child_scope(child, child_inputs, self.owner)
    }

    fn leaves_out_line(&mut self, line_root: NodeSlotId, line_index: u32) -> bool {
        line_root == self.owner.slot
            && self
                .walk
                .point_misses_line(line_root, line_index, self.owner.absolute_content_origin)
    }

    fn paint_children_kept_by_an_earlier_phase(&mut self, owner: NodeSlotId) -> Option<&[NodeSlotId]> {
        self.walk
            .paint_children_the_point_may_be_over
            .get(&owner)
            .map(|children| children.as_slice())
    }

    fn keep_paint_children_for_later_phases(&mut self, owner: NodeSlotId, children_in_tree_order: &[NodeSlotId]) {
        self.walk
            .paint_children_the_point_may_be_over
            .insert(owner, SmallVec::from_slice(children_in_tree_order));
    }
}

#[derive(Clone, Copy)]
struct PlacedOwner {
    slot: NodeSlotId,
    absolute_content_origin: CssPixelPoint,
    places_children_at_their_committed_offsets: bool,
}

/// An item under a point, with the point in the space of the item.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PointHit {
    pub item: HitTestItem,
    pub local_point: CssPixelPoint,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Wanted {
    Topmost,
    Everything,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Flow {
    Continue,
    Stop,
}

struct PointWalk<'a, R: PaintRead> {
    rows: &'a R,
    viewport: NodeSlotId,
    callbacks: &'a FfiHitTestQueryCallbacks,
    viewport_mapping: ViewportMapping<'a>,
    point: CssPixelPoint,
    paint_overlay: bool,
    wanted: Wanted,
    skips_scopes_the_point_misses: bool,
    depth_sorting: DepthSortingState<'a>,
    local_points_inside_clips: FastMap<ContextRef, Option<FloatPoint>>,
    local_points_ignoring_clips: FastMap<SpatialNodeIndex, Option<FloatPoint>>,
    last_local_point_ignoring_clips: Option<(SpatialNodeIndex, Option<FloatPoint>)>,
    hits_of_producer_in_paint_order: Vec<PointHit>,
    paint_children_the_point_may_be_over: FastMap<NodeSlotId, SmallVec<[NodeSlotId; 8]>>,
    /// What lies under the point, topmost first as the walk met it.
    hits: Vec<PointHit>,
    /// The 3D rendering context the topmost hit is in, whose planes paint sorted by depth: the walk goes on until
    /// it leaves the context, and the plane nearest the viewer wins.
    sorting_group_of_topmost_hit: Option<u32>,
    walk_left_the_sorting_group: bool,
}

impl<R: PaintRead> HitTestItemSink for PointWalk<'_, R> {
    fn hit_test_facts(&mut self, paintable: NodeSlotId) -> HitTestFacts {
        hit_test_facts(self.rows, paintable)
    }

    fn own_accumulated_2d_scale(&self, paintable: NodeSlotId) -> libgfx_rust::FloatSize {
        own_accumulated_2d_scale(self.rows, self.callbacks.world_transforms(), paintable)
    }

    fn wants(&mut self, kind: HitTestItemKind, rect: CssPixelRect, context: ContextRef) -> bool {
        if let Some(group) = self.sorting_group_of_topmost_hit
            && self.depth_sorting.group_for(context.spatial) != Some(group)
        {
            self.walk_left_the_sorting_group = true;
            return false;
        }
        let Some(local) = self.local_point_inside_clips(context) else {
            return false;
        };
        // A chrome widget is placed when it is tested, and a path is tested in float units. Everything else lies in
        // its rect, which one unit of slack keeps from being rejected by rounding the point.
        let one_unit = CssPixels::from_raw(1);
        kind == HitTestItemKind::ChromeWidget
            || kind == HitTestItemKind::SvgPath
            || rect
                .inflated(one_unit, one_unit, one_unit, one_unit)
                .contains_point(to_css_point(local))
    }

    fn rejects_text_fragments_placed_at_their_line_root_before_building_them(&self) -> bool {
        self.skips_scopes_the_point_misses
    }

    fn misses_line(
        &mut self,
        line_root: NodeSlotId,
        line_index: u32,
        line_root_absolute_content_origin: CssPixelPoint,
    ) -> bool {
        self.point_misses_line(line_root, line_index, line_root_absolute_content_origin)
    }

    fn append(&mut self, item: HitTestItem) {
        let Some(local) = self.local_point_inside_clips(item.context) else {
            return;
        };
        if item_contains_point(self.rows, self.callbacks, &item, local) {
            self.hits_of_producer_in_paint_order.push(PointHit {
                item,
                local_point: to_css_point(local),
            });
        }
    }
}

impl<R: PaintRead> PointWalk<'_, R> {
    fn local_point(&self, context: ContextRef, clip_behavior: ClipBehavior) -> Option<FloatPoint> {
        self.viewport_mapping
            .point_to_local(context, self.point, ViewportSpace::VisualViewport, clip_behavior)
    }

    fn local_point_inside_clips(&mut self, context: ContextRef) -> Option<FloatPoint> {
        if let Some(local) = self.local_points_inside_clips.get(&context) {
            return *local;
        }
        let local = self.local_point(context, ClipBehavior::Respect);
        self.local_points_inside_clips.insert(context, local);
        local
    }

    fn local_point_ignoring_clips(&mut self, context: ContextRef) -> Option<FloatPoint> {
        if let Some((spatial, local)) = self.last_local_point_ignoring_clips
            && spatial == context.spatial
        {
            return local;
        }
        let local = match self.local_points_ignoring_clips.get(&context.spatial) {
            Some(local) => *local,
            None => {
                let local = self.local_point(context, ClipBehavior::Ignore);
                self.local_points_ignoring_clips.insert(context.spatial, local);
                local
            }
        };
        self.last_local_point_ignoring_clips = Some((context.spatial, local));
        local
    }

    fn placed_owner(&self, owner: NodeSlotId, absolute_content_origin: CssPixelPoint) -> PlacedOwner {
        PlacedOwner {
            slot: owner,
            absolute_content_origin,
            places_children_at_their_committed_offsets: self.rows.node_kind_if_live(owner).is_some_and(|kind| {
                kind != NodeKind::SVGSVGBox && kind != NodeKind::SVGForeignObjectBox && !node_painting::is_svg(kind)
            }),
        }
    }

    fn is_placed_at_its_committed_offset_in(&self, child: NodeSlotId, owner: PlacedOwner) -> bool {
        let data = self.rows.paintable_data(child);
        owner.places_children_at_their_committed_offsets
            && data.containing_block == owner.slot
            && !data.has_flag(PaintableFlag::InLaterContainingBlockFragment)
    }

    fn absolute_content_origin_of_child(&self, child: NodeSlotId, owner: PlacedOwner) -> CssPixelPoint {
        if self.is_placed_at_its_committed_offset_in(child, owner) {
            let offset: CssPixelPoint = paintable_geometry::committed_offset(self.rows, child).into();
            return owner.absolute_content_origin.translated(offset.x, offset.y);
        }
        paintable_geometry::absolute_rect(self.rows, child).location()
    }

    fn point_misses_line(
        &mut self,
        line_root: NodeSlotId,
        line_index: u32,
        line_root_absolute_content_origin: CssPixelPoint,
    ) -> bool {
        let rows = self.rows;
        let side = rows.committed_side_data(line_root);
        let Some(bounds) = side.hit_test_bounds_of_lines.as_deref() else {
            return false;
        };
        if bounds.len() != side.lines().len() {
            return false;
        }
        let Some(HitTestBoundOfLine::RectInContentBoxOfLineRoot(bound)) = bounds.get(line_index as usize) else {
            return false;
        };
        let bound = bound.translated_by(line_root_absolute_content_origin);
        let context = rows
            .paintable_data(line_root)
            .accumulated_visual_context_for_descendants;
        let Some(local) = self.local_point_ignoring_clips(context) else {
            return false;
        };
        let one_pixel = CssPixels::from_integer(1);
        !bound
            .inflated(one_pixel, one_pixel, one_pixel, one_pixel)
            .contains_point(to_css_point(local))
    }

    /// Whether nothing the child scope paints can be under the point. A point that cannot be taken into the space of
    /// the child's box misses nothing.
    fn point_misses_child_scope(
        &mut self,
        child: PaintScope,
        child_inputs: PaintOrderInputs,
        owner: PlacedOwner,
    ) -> bool {
        let rows = self.rows;
        if child.owner == self.viewport
            || !rows.paintable_row_is_populated(child.owner)
            || !child_inputs.forms_containing_block_for_children()
        {
            return false;
        }
        let ink = rows.stored_ink_overflow(child.owner);
        if ink.unbounded || ink.contribution_unbounded {
            return false;
        }
        let (offset, padding_left, padding_top): (CssPixelPoint, CssPixels, CssPixels) = rows
            .with_committed_fragment_link(child.owner, |link| {
                link.map_or_else(Default::default, |link| {
                    (
                        link.offset.into(),
                        link.fragment.sides.padding.left,
                        link.fragment.sides.padding.top,
                    )
                })
            });
        let content_origin = if self.is_placed_at_its_committed_offset_in(child.owner, owner) {
            owner.absolute_content_origin.translated(offset.x, offset.y)
        } else {
            paintable_geometry::absolute_rect(rows, child.owner).location()
        };
        let padding_box_origin = CssPixelPoint::new(content_origin.x - padding_left, content_origin.y - padding_top);
        let bound = ink.rect.translated_by(padding_box_origin);
        let data = rows.paintable_data(child.owner);
        let Some(local) = self.local_point_ignoring_clips(data.accumulated_visual_context) else {
            return false;
        };
        let one_pixel = CssPixels::from_integer(1);
        if bound
            .inflated(one_pixel, one_pixel, one_pixel, one_pixel)
            .contains_point(to_css_point(local))
        {
            return false;
        }
        let paints_boxes_hoisted_to_it =
            child.kind == PaintScopeKind::PaintedAsStackingContext && data.establishes_stacking_context;
        !paints_boxes_hoisted_to_it || child.owner_contains_the_boxes_hoisted_to_its_stacking_context(rows)
    }

    fn scope(&mut self, scope: PaintScope, owner_absolute_content_origin: CssPixelPoint) -> Flow {
        let rows = self.rows;
        let owner = self.placed_owner(scope.owner, owner_absolute_content_origin);
        let plan = if self.skips_scopes_the_point_misses {
            let paint_overlay = self.paint_overlay;
            PaintScopePlan::build_leaving_out(
                rows,
                scope,
                paint_overlay,
                &mut WhatThePointMisses { walk: self, owner },
            )
        } else {
            PaintScopePlan::build(rows, scope, self.paint_overlay)
        };
        if plan.establishes_stacking_context && !stacking_context_paints(rows, scope.owner) {
            return Flow::Continue;
        }
        for item in plan.items.iter().rev() {
            let flow = match *item {
                PaintOrderItem::Producer(producer) => hit_tested_parts(producer)
                    .into_iter()
                    .rev()
                    .map(|part| self.part(scope.owner, part))
                    .find(|flow| *flow == Flow::Stop)
                    .unwrap_or(Flow::Continue),
                PaintOrderItem::Scope(child) => {
                    let child_origin = self.absolute_content_origin_of_child(child.owner, owner);
                    self.scope(child, child_origin)
                }
            };
            if flow == Flow::Stop {
                return Flow::Stop;
            }
        }
        Flow::Continue
    }

    fn part(&mut self, owner: NodeSlotId, part: HitTestedPart) -> Flow {
        match part {
            HitTestedPart::Phase(phase) => self.items_of_phase(owner, phase),
            HitTestedPart::SvgBox => self.svg_box(owner),
        }
    }

    fn svg_box(&mut self, svg_box: NodeSlotId) -> Flow {
        if svg_box_is_hidden_by_an_empty_mask_or_clip(self.rows, svg_box) {
            return Flow::Continue;
        }
        for child in svg_children_painted_with_their_parent(self.rows, svg_box)
            .into_iter()
            .rev()
        {
            if self.svg_box(child) == Flow::Stop {
                return Flow::Stop;
            }
        }
        self.items_of_phase(svg_box, PaintPhase::Foreground)
    }

    fn items_of_phase(&mut self, owner: NodeSlotId, phase: PaintPhase) -> Flow {
        let rows = self.rows;
        debug_assert!(self.hits_of_producer_in_paint_order.is_empty());
        hit_test_items_of_phase(rows, self, owner, phase);
        let mut hits = std::mem::take(&mut self.hits_of_producer_in_paint_order);
        let mut flow = if self.walk_left_the_sorting_group {
            Flow::Stop
        } else {
            Flow::Continue
        };
        for hit in hits.drain(..).rev() {
            if flow == Flow::Continue {
                flow = self.take_hit(hit);
            }
        }
        self.hits_of_producer_in_paint_order = hits;
        flow
    }

    fn take_hit(&mut self, hit: PointHit) -> Flow {
        if self.wanted == Wanted::Everything {
            self.hits.push(hit);
            return Flow::Continue;
        }
        let group = self.depth_sorting.group_for(hit.item.context.spatial);
        if self.hits.is_empty() {
            self.hits.push(hit);
            self.sorting_group_of_topmost_hit = group;
            return if group.is_some() { Flow::Continue } else { Flow::Stop };
        }
        // A context's items are consecutive in paint order, so the first hit outside it ends it.
        if group != self.sorting_group_of_topmost_hit {
            return Flow::Stop;
        }
        self.hits.push(hit);
        Flow::Continue
    }

    /// The key hits on the planes of one 3D rendering context sort by, greatest in front: the depth of the plane at
    /// the point, and for planes at one depth their paint order, which is the reverse of the order the walk met
    /// them in.
    fn front_to_back_key(&mut self, position_among_hits: usize) -> (Option<i64>, std::cmp::Reverse<usize>) {
        let spatial = self.hits[position_among_hits].item.context.spatial;
        (
            self.depth_sorting.depth_key_for(spatial),
            std::cmp::Reverse(position_among_hits),
        )
    }

    fn topmost(mut self) -> Option<PointHit> {
        let front = (0..self.hits.len()).max_by_key(|position| self.front_to_back_key(*position))?;
        Some(self.hits.swap_remove(front))
    }

    // Runs of hits on the planes of one 3D rendering context are reordered front to back. A context's items are
    // consecutive in paint order, so its hits sit next to each other.
    fn everything_topmost_first(mut self) -> Vec<PointHit> {
        let no_hit_is_on_a_plane_ranked_by_depth = (0..self.hits.len()).all(|position| {
            let spatial = self.hits[position].item.context.spatial;
            self.depth_sorting.group_for(spatial).is_none()
        });
        if no_hit_is_on_a_plane_ranked_by_depth {
            return self.hits;
        }
        let mut order: Vec<usize> = (0..self.hits.len()).collect();
        let mut run_begin = 0;
        while run_begin < order.len() {
            let group = self.depth_sorting.group_for(self.hits[run_begin].item.context.spatial);
            let mut run_end = run_begin + 1;
            if group.is_some() {
                while run_end < order.len()
                    && self.depth_sorting.group_for(self.hits[run_end].item.context.spatial) == group
                {
                    run_end += 1;
                }
                order[run_begin..run_end].sort_by_key(|position| std::cmp::Reverse(self.front_to_back_key(*position)));
            }
            run_begin = run_end;
        }
        let mut hits: Vec<Option<PointHit>> = self.hits.into_iter().map(Some).collect();
        order
            .into_iter()
            .map(|position| hits[position].take().expect("every hit is taken once"))
            .collect()
    }
}

/// What a point query reads besides the rows.
#[derive(Clone, Copy)]
pub(crate) struct PointQuery<'a> {
    pub tree: &'a VisualContextTree,
    pub callbacks: &'a FfiHitTestQueryCallbacks,
    pub viewport: NodeSlotId,
    pub paint_overlay: bool,
    pub point: CssPixelPoint,
}

fn walk_for<'a, R: PaintRead>(
    rows: &'a R,
    query: PointQuery<'a>,
    wanted: Wanted,
    skips_scopes_the_point_misses: bool,
) -> PointWalk<'a, R> {
    let mut walk = PointWalk {
        rows,
        viewport: query.viewport,
        callbacks: query.callbacks,
        viewport_mapping: query.callbacks.viewport_mapping(query.tree),
        point: query.point,
        paint_overlay: query.paint_overlay,
        wanted,
        skips_scopes_the_point_misses,
        depth_sorting: DepthSortingState::new(query.callbacks, query.tree, query.point),
        local_points_inside_clips: FastMap::default(),
        local_points_ignoring_clips: FastMap::default(),
        last_local_point_ignoring_clips: None,
        hits_of_producer_in_paint_order: Vec::new(),
        paint_children_the_point_may_be_over: FastMap::default(),
        hits: Vec::new(),
        sorting_group_of_topmost_hit: None,
        walk_left_the_sorting_group: false,
    };
    if rows.paintable_row_is_populated(query.viewport) {
        let viewport_origin = paintable_geometry::absolute_rect(rows, query.viewport).location();
        walk.scope(PaintScope::stacking_context(query.viewport), viewport_origin);
    }
    walk
}

fn skipped_scopes_are_verified() -> bool {
    static VERIFIED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *VERIFIED.get_or_init(|| cfg!(debug_assertions) || std::env::var_os("LIBWEB_VERIFY_HIT_TEST_PRUNING").is_some())
}

/// The topmost item under the point.
pub(crate) fn topmost_hit(rows: &impl PaintRead, query: PointQuery<'_>) -> Option<PointHit> {
    let hit = walk_for(rows, query, Wanted::Topmost, true).topmost();
    if skipped_scopes_are_verified() {
        let hit_without_skipping = walk_for(rows, query, Wanted::Topmost, false).topmost();
        assert!(
            hit == hit_without_skipping,
            "Skipping paint scopes by their ink bounds changed the topmost hit at {:?}: {hit:?} instead of {hit_without_skipping:?}",
            query.point
        );
    }
    hit
}

/// Every item under the point, topmost first.
pub(crate) fn all_hits(rows: &impl PaintRead, query: PointQuery<'_>) -> Vec<PointHit> {
    let hits = walk_for(rows, query, Wanted::Everything, true).everything_topmost_first();
    if skipped_scopes_are_verified() {
        let hits_without_skipping = walk_for(rows, query, Wanted::Everything, false).everything_topmost_first();
        assert!(
            hits == hits_without_skipping,
            "Skipping paint scopes by their ink bounds changed the hits at {:?}: {hits:?} instead of {hits_without_skipping:?}",
            query.point
        );
        let topmost_without_skipping = walk_for(rows, query, Wanted::Topmost, false).topmost();
        assert!(
            topmost_without_skipping.as_ref() == hits_without_skipping.first(),
            "The walk that stops at the topmost hit at {:?} found {topmost_without_skipping:?} instead of {:?}",
            query.point,
            hits_without_skipping.first()
        );
    }
    hits
}
