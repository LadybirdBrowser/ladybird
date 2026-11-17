/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Assembles a frame from the scopes the published recording walked and what changed since. A walked scope whose
//! owner and paint subtree were stamped with no generation newer than the published recording, and which has not
//! moved, is reused whole. Otherwise its plan is walked: each run of items reuses the chunk the published recording
//! recorded it into, or is recorded again into a chunk of its own, from its first item to its last, when something it
//! reads changed or an ancestor moved it.
//!
//! A recording of a window of the document leaves out the runs and the walked child scopes that lie outside the
//! window, and remembers that it did, so that the recording after it records them once they lie inside.

use super::PaintPhase;
use super::order_tree::{
    OutputFacts, ProducerKind, RecordedChunk, RecordedRun, RecordedScope, RecordedSegment, RowPlacement, Segment,
    SegmentedPlan,
};
use super::window::InkBound;
use crate::css::style::fast_hash::FastMap;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_order_plan::{PaintOrderItem, PaintProducer, PaintScope};
use crate::painting::record::change_ticks::RowChanges;
use smallvec::SmallVec;
use std::sync::Arc;

/// The planner's items for one scope in paint order. Every producer belongs to the scope's
/// owner. An inactive plan is a stacking context that does not paint; its scope records nothing.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct ScopePlan {
    pub active: bool,
    pub items: SmallVec<[PaintOrderItem; 16]>,
}

/// A box phase in paint order is up to two producers: the scroll metadata recorded with the background, and its
/// commands. An SVG root paints its background, border and then all of its content as one producer. What a box can
/// be hit by is not recorded: hit testing walks the paint order over the rows.
pub(crate) fn producer_kinds(producer: PaintProducer) -> SmallVec<[ProducerKind; 4]> {
    use ProducerKind::*;
    match producer {
        PaintProducer::BoxPhase(PaintPhase::Background) => smallvec::smallvec![ScrollMetadata, DrawBackground],
        PaintProducer::BoxPhase(PaintPhase::Border) => smallvec::smallvec![DrawBorder],
        PaintProducer::BoxPhase(PaintPhase::TableCollapsedBorder) => smallvec::smallvec![DrawTableCollapsedBorder],
        PaintProducer::BoxPhase(PaintPhase::Foreground) => smallvec::smallvec![DrawForeground],
        PaintProducer::BoxPhase(PaintPhase::Outline) => smallvec::smallvec![DrawOutline],
        PaintProducer::BoxPhase(PaintPhase::Overlay) => smallvec::smallvec![DrawOverlay],
        PaintProducer::SvgRoot => smallvec::smallvec![ScrollMetadata, DrawBackground, DrawBorder, Svg],
        PaintProducer::SvgBoxForeground => smallvec::smallvec![Svg],
        PaintProducer::ScopePreamble => smallvec::smallvec![ScopePreamble],
        PaintProducer::ForegroundHitTest => SmallVec::new(),
        PaintProducer::ForegroundCommands => smallvec::smallvec![DrawForeground],
        PaintProducer::InlinePiece(index) => smallvec::smallvec![InlinePiece(index)],
        PaintProducer::TextFragment(index) => smallvec::smallvec![TextFragment(index)],
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ProducerOutcome {
    // Recorded again every frame; the entry and every scope above it stay live.
    pub live: bool,
    // Recorded again when the window gains or loses focus.
    pub reads_window_focus: bool,
    // Recorded again when a transform changes a scale that the spaces of the tree accumulate.
    pub reads_accumulated_scale: bool,
    pub blocking_wheel_event_regions: u32,
}

impl ProducerOutcome {
    fn facts(self) -> OutputFacts {
        OutputFacts {
            blocking_wheel_event_regions: self.blocking_wheel_event_regions,
            live: self.live,
            reads_window_focus: self.reads_window_focus,
            reads_accumulated_scale: self.reads_accumulated_scale,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ScopeAction {
    // Walked with the published child list.
    Assembled,
    // The plan was rebuilt and matched against the published children.
    Replanned,
    // Recorded without a published counterpart.
    Recorded,
    // The stacking context does not paint.
    Inactive,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AssemblyEvent {
    ScopeBegin(PaintScope, ScopeAction),
    ScopeEnd(PaintScope),
    ScopeCopied(PaintScope),
    ProducerCopied(NodeSlotId, ProducerKind),
    ScopeLeftOut(PaintScope),
    ProducerLeftOut(NodeSlotId, ProducerKind),
}

/// How much of the document a recording holds, against what the published recording holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Coverage {
    WholeDocument,
    /// What shows in the window the published recording was made for, with every space placing what it holds where it
    /// did then, so that what lay outside it then still does unless something changed since.
    PublishedWindow,
    /// What shows in a window the published recording was not made for.
    AnotherWindow,
}

/// Where the plan of a walked scope is cut at one of its items.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ItemCut {
    /// A child scope that is walked on its own.
    Walked,
    /// An item recorded in a run. Runs are cut along a grid, so an item that knows which row of it it starts in
    /// says so; one that does not stays with the items before it.
    InRun { grid_row: Option<i32> },
}

/// A run never holds more items than this, however little room they take.
const MAX_ITEMS_IN_A_RUN: usize = 512;

/// What assembly needs from the recorder, which owns the output and the published recording.
pub(crate) trait AssemblyHost {
    fn plan_scope(&mut self, scope: PaintScope) -> ScopePlan;

    /// Where the plan of a walked scope is cut at `item`.
    fn cut_at(&mut self, scope: PaintScope, item: PaintOrderItem) -> ItemCut;

    /// Records one producer of `owner` under the contexts it needs.
    fn record_producer(&mut self, owner: NodeSlotId, kind: ProducerKind) -> ProducerOutcome;

    /// Takes what was recorded since the last run ended out as the chunk of the run that recorded it, or none where
    /// nothing was.
    fn take_run_chunk(&mut self) -> Option<Arc<RecordedChunk>>;

    /// What changed of a row since the published recording: what its own producers read, its place in paint order,
    /// or something in its paint subtree.
    fn changes_of_row(&mut self, row: NodeSlotId) -> RowChanges;

    /// Where a row's boxes are. A row an ancestor moved records again though nothing stamped it.
    fn placement_of_row(&mut self, row: NodeSlotId) -> RowPlacement;

    fn producer_reads_descendants(&mut self, owner: NodeSlotId, kind: ProducerKind) -> bool;

    /// What bounds everything `item` of the plan of `scope` paints, or none where nothing in the spaces of the tree
    /// does. Only a recording of a window asks.
    fn bound_of_item(&mut self, _scope: PaintScope, _item: PaintOrderItem) -> Option<InkBound> {
        None
    }

    fn is_outside_window(&mut self, _bound: &InkBound) -> bool {
        false
    }

    fn is_wholly_inside_window(&mut self, _bound: &InkBound) -> bool {
        false
    }

    /// Whether a box cuts off some of what it holds, as a scroller does. What bounds its scopes then bounds what
    /// shows of them, and what they paint may lie outside it.
    fn cuts_off_what_it_holds(&mut self, _owner: NodeSlotId) -> bool {
        false
    }

    fn observe(&mut self, _event: AssemblyEvent) {}
}

#[derive(PartialEq, Eq, Hash)]
enum SegmentKey {
    RunStartingAt(PaintOrderItem),
    Walked(PaintScope),
}

/// Whether the published recording can be returned as it is: nothing changed since, and nothing in it is recorded
/// every frame or read a window focus or an accumulated scale that changed.
pub(crate) fn frame_is_unchanged(
    published_root: &RecordedScope,
    anything_changed: bool,
    window_focus_changed: bool,
    accumulated_scales_changed: bool,
    coverage: Coverage,
) -> bool {
    let facts = published_root.facts;
    let covers_the_same = match coverage {
        Coverage::PublishedWindow => true,
        Coverage::WholeDocument => !published_root.leaves_out_what_lies_outside_the_window,
        Coverage::AnotherWindow => false,
    };
    !anything_changed
        && !facts.live
        && !(window_focus_changed && facts.reads_window_focus)
        && !(accumulated_scales_changed && facts.reads_accumulated_scale)
        && covers_the_same
}

pub(crate) struct Assembler<'a, H: AssemblyHost> {
    host: &'a mut H,
    // The published recording was made in the other window focus state.
    window_focus_changed: bool,
    // A transform changed a scale that the spaces of the tree accumulate since the published recording.
    accumulated_scales_changed: bool,
    coverage: Coverage,
}

impl<'a, H: AssemblyHost> Assembler<'a, H> {
    pub(crate) fn new(
        host: &'a mut H,
        window_focus_changed: bool,
        accumulated_scales_changed: bool,
        coverage: Coverage,
    ) -> Self {
        Self {
            host,
            window_focus_changed,
            accumulated_scales_changed,
            coverage,
        }
    }

    // Reusing a part would keep output that is recorded every frame, or that read the window focus or an
    // accumulated scale before it changed.
    fn records_again(&self, facts: OutputFacts) -> bool {
        facts.live
            || (self.window_focus_changed && facts.reads_window_focus)
            || (self.accumulated_scales_changed && facts.reads_accumulated_scale)
    }

    /// Records the frame's root scope. `published_root` is the root the published recording walked, when that
    /// recording can be reused from.
    pub(crate) fn assemble_root(
        mut self,
        root_scope: PaintScope,
        published_root: Option<&Arc<RecordedScope>>,
    ) -> Arc<RecordedScope> {
        let published = published_root.filter(|root| root.scope == root_scope);
        let root_bound = match self.coverage {
            Coverage::WholeDocument => None,
            Coverage::PublishedWindow | Coverage::AnotherWindow => {
                self.host.bound_of_item(root_scope, PaintOrderItem::Scope(root_scope))
            }
        };
        let root_cuts_off_what_it_holds = self.host.cuts_off_what_it_holds(root_scope.owner);
        let is_wholly_inside_window =
            self.is_wholly_inside_window(root_bound.as_ref(), false, root_cuts_off_what_it_holds);
        self.walk_scope(root_scope, published, is_wholly_inside_window)
    }

    // Nothing of what a scope paints directly is left out: the recording holds everything, or the scope is in a
    // scope that is wholly inside the window or is bounded by something that is. The scope of a box that cuts off
    // what it holds paints outside what bounds it, so being inside the window says nothing about what it holds.
    fn is_wholly_inside_window(
        &mut self,
        bound: Option<&InkBound>,
        is_in_a_scope_wholly_inside_window: bool,
        owner_cuts_off_what_it_holds: bool,
    ) -> bool {
        if self.coverage == Coverage::WholeDocument {
            return true;
        }
        if owner_cuts_off_what_it_holds {
            return false;
        }
        is_in_a_scope_wholly_inside_window || bound.is_some_and(|bound| self.host.is_wholly_inside_window(bound))
    }

    // Whether a clean walked scope of the published recording holds what this recording holds of it: neither left
    // out what this one records, nor recorded what this one leaves out. In another window that is so where the scope
    // is wholly inside both, down to the scopes below it that paint outside what bounds it.
    fn published_scope_covers_the_same(&self, published: &RecordedScope, is_wholly_inside_window: bool) -> bool {
        match self.coverage {
            Coverage::PublishedWindow => true,
            Coverage::WholeDocument => !published.leaves_out_what_lies_outside_the_window,
            Coverage::AnotherWindow => {
                !published.leaves_out_what_lies_outside_the_window
                    && !published.walks_a_scope_of_a_box_that_cuts_off_what_it_holds
                    && is_wholly_inside_window
            }
        }
    }

    fn bound_of_segment(&mut self, scope: PaintScope, plan: &SegmentedPlan, segment: &Segment) -> Option<InkBound> {
        match segment {
            Segment::Walked(item) => self.host.bound_of_item(scope, plan.items[*item as usize]),
            Segment::Run(run) => {
                let mut bound_of_run: Option<InkBound> = None;
                for item in plan.items_of(run) {
                    let bound = self.host.bound_of_item(scope, *item)?;
                    bound_of_run = Some(match bound_of_run {
                        Some(bound_so_far) => bound_so_far.united(&bound)?,
                        None => bound,
                    });
                }
                bound_of_run
            }
        }
    }

    fn observe_left_out(&mut self, scope: PaintScope, plan: &SegmentedPlan, segment: &Segment) {
        match segment {
            Segment::Walked(item) => self.host.observe(AssemblyEvent::ScopeLeftOut(plan.walked_scope(*item))),
            Segment::Run(run) => {
                for item in plan.items_of(run) {
                    match *item {
                        PaintOrderItem::Producer(producer) => {
                            for kind in producer_kinds(producer) {
                                self.host.observe(AssemblyEvent::ProducerLeftOut(scope.owner, kind));
                            }
                        }
                        PaintOrderItem::Scope(child) => self.host.observe(AssemblyEvent::ScopeLeftOut(child)),
                    }
                }
            }
        }
    }

    fn scope_is_clean(&mut self, scope: PaintScope, recorded_placement: RowPlacement) -> bool {
        !self.host.changes_of_row(scope.owner).any() && self.host.placement_of_row(scope.owner) == recorded_placement
    }

    fn producer_records_again(&mut self, owner: NodeSlotId, kind: ProducerKind, owner_changes: RowChanges) -> bool {
        owner_changes.inputs
            || (owner_changes.scroll_metadata && kind == ProducerKind::ScrollMetadata)
            || (owner_changes.below && self.host.producer_reads_descendants(owner, kind))
    }

    fn plan_and_cut(&mut self, scope: PaintScope) -> SegmentedPlan {
        let plan = self.host.plan_scope(scope);
        self.cut(scope, plan.active, plan.items.into_vec().into())
    }

    fn cut(&mut self, scope: PaintScope, active: bool, items: Arc<[PaintOrderItem]>) -> SegmentedPlan {
        let mut segments = Vec::new();
        let mut run_start = 0;
        let mut grid_row_of_run = None;
        for (index, item) in items.iter().enumerate() {
            let cut = self.host.cut_at(scope, *item);
            let starts_another_run = match cut {
                ItemCut::Walked => true,
                ItemCut::InRun { grid_row } => {
                    let leaves_grid_row =
                        grid_row.is_some() && grid_row_of_run.is_some() && grid_row != grid_row_of_run;
                    leaves_grid_row || index - run_start == MAX_ITEMS_IN_A_RUN
                }
            };
            if starts_another_run {
                if run_start < index {
                    segments.push(Segment::Run(run_start as u32..index as u32));
                }
                run_start = index;
                grid_row_of_run = None;
            }
            match cut {
                ItemCut::Walked => {
                    segments.push(Segment::Walked(index as u32));
                    run_start = index + 1;
                }
                ItemCut::InRun { grid_row } => grid_row_of_run = grid_row_of_run.or(grid_row),
            }
        }
        if run_start < items.len() {
            segments.push(Segment::Run(run_start as u32..items.len() as u32));
        }
        SegmentedPlan {
            active,
            items,
            segments,
        }
    }

    /// Walks the plan of `scope`, which changed or the published recording did not walk, or which holds something the
    /// window of the published recording left out. A scope that is wholly inside the window of this recording leaves
    /// nothing out, so nothing asks what bounds its segments.
    fn walk_scope(
        &mut self,
        scope: PaintScope,
        published: Option<&Arc<RecordedScope>>,
        is_wholly_inside_window: bool,
    ) -> Arc<RecordedScope> {
        let owner_changes = self.host.changes_of_row(scope.owner);
        let owner_placement = self.host.placement_of_row(scope.owner);
        let owner_moved = published.is_some_and(|published| published.owner_placement != owner_placement);
        let published_plan = published
            .filter(|_| !owner_changes.order)
            .map(|published| &published.plan);
        let items_were_kept = published_plan.is_some();
        // Nothing changed of the owner or anything below it, and nothing moved it: every item is where it was.
        let cuts_were_kept = items_were_kept && !owner_changes.any() && !owner_moved;
        let plan = match published_plan {
            Some(published_plan) if cuts_were_kept => Arc::clone(published_plan),
            Some(published_plan) => Arc::new(self.cut(scope, published_plan.active, Arc::clone(&published_plan.items))),
            None => Arc::new(self.plan_and_cut(scope)),
        };
        let action = match (published.is_some(), items_were_kept, plan.active) {
            (_, _, false) => ScopeAction::Inactive,
            (true, true, true) => ScopeAction::Assembled,
            (true, false, true) => ScopeAction::Replanned,
            (false, _, true) => ScopeAction::Recorded,
        };
        self.host.observe(AssemblyEvent::ScopeBegin(scope, action));
        let mut recorded = RecordedScope {
            scope,
            plan: Arc::clone(&plan),
            segments: Vec::with_capacity(plan.segments.len()),
            facts: OutputFacts::default(),
            owner_placement,
            segment_bounds: None,
            leaves_out_what_lies_outside_the_window: false,
            walks_a_scope_of_a_box_that_cuts_off_what_it_holds: false,
        };
        if plan.active {
            if !is_wholly_inside_window {
                // A scope whose cuts were kept holds every item where it was, so what bounded its segments does.
                let kept_bounds = published
                    .filter(|_| cuts_were_kept)
                    .and_then(|published| published.segment_bounds.clone());
                recorded.segment_bounds = Some(kept_bounds.unwrap_or_else(|| {
                    plan.segments
                        .iter()
                        .map(|segment| self.bound_of_segment(scope, &plan, segment))
                        .collect()
                }));
            }
            let segment_bounds = recorded.segment_bounds.clone();
            let published_segments = published.map(|published| PublishedSegments::new(published, cuts_were_kept));
            for (index, segment) in plan.segments.iter().enumerate() {
                let bound = segment_bounds.as_ref().and_then(|bounds| bounds[index].as_ref());
                if bound.is_some_and(|bound| self.host.is_outside_window(bound)) {
                    self.observe_left_out(scope, &plan, segment);
                    recorded.leaves_out_what_lies_outside_the_window = true;
                    recorded.segments.push(RecordedSegment::Outside);
                    continue;
                }
                let published_segment = published_segments
                    .as_ref()
                    .and_then(|segments| segments.matching(index, segment, &plan));
                let assembled = match segment {
                    Segment::Run(run) => RecordedSegment::Run(self.assemble_run(
                        scope,
                        plan.items_of(run),
                        published_segment,
                        owner_changes,
                        owner_moved,
                    )),
                    Segment::Walked(item) => {
                        let child = plan.walked_scope(*item);
                        let child_cuts_off_what_it_holds = self.host.cuts_off_what_it_holds(child.owner);
                        let child_is_wholly_inside_window =
                            self.is_wholly_inside_window(bound, is_wholly_inside_window, child_cuts_off_what_it_holds);
                        let assembled_child =
                            self.assemble_walked_child(child, published_segment, child_is_wholly_inside_window);
                        recorded.walks_a_scope_of_a_box_that_cuts_off_what_it_holds |= child_cuts_off_what_it_holds
                            || assembled_child.walks_a_scope_of_a_box_that_cuts_off_what_it_holds;
                        RecordedSegment::Walked(assembled_child)
                    }
                };
                recorded.facts.add(assembled.facts());
                recorded.leaves_out_what_lies_outside_the_window |= assembled.leaves_out_what_lies_outside_the_window();
                recorded.segments.push(assembled);
            }
        }
        self.host.observe(AssemblyEvent::ScopeEnd(scope));
        Arc::new(recorded)
    }

    fn assemble_walked_child(
        &mut self,
        child: PaintScope,
        published: Option<&RecordedSegment>,
        is_wholly_inside_window: bool,
    ) -> Arc<RecordedScope> {
        let published = published.and_then(|segment| match segment {
            RecordedSegment::Walked(scope) => Some(scope),
            RecordedSegment::Run(_) | RecordedSegment::Outside => None,
        });
        if let Some(published) = published
            && !self.records_again(published.facts)
            && self.published_scope_covers_the_same(published, is_wholly_inside_window)
            && self.scope_is_clean(child, published.owner_placement)
        {
            self.host.observe(AssemblyEvent::ScopeCopied(child));
            return Arc::clone(published);
        }
        self.walk_scope(child, published, is_wholly_inside_window)
    }

    fn assemble_run(
        &mut self,
        scope: PaintScope,
        items: &[PaintOrderItem],
        published: Option<&RecordedSegment>,
        owner_changes: RowChanges,
        owner_moved: bool,
    ) -> RecordedRun {
        let published = published.and_then(|segment| match segment {
            RecordedSegment::Run(run) => Some(run),
            RecordedSegment::Walked(_) | RecordedSegment::Outside => None,
        });
        // Only a chunk without placeholders for SVG images can be drawn by another recording than the one that made
        // it, which the publication of a recording finishes every chunk of.
        if let Some(run) = published
            && !owner_moved
            && !self.records_again(run.facts)
            && run.chunk.as_ref().is_none_or(|chunk| chunk.finished().is_some())
            && self.run_is_clean(scope, items, run, owner_changes)
        {
            for item in items {
                match *item {
                    PaintOrderItem::Producer(producer) => {
                        for kind in producer_kinds(producer) {
                            self.host.observe(AssemblyEvent::ProducerCopied(scope.owner, kind));
                        }
                    }
                    PaintOrderItem::Scope(child) => self.host.observe(AssemblyEvent::ScopeCopied(child)),
                }
            }
            return RecordedRun {
                chunk: run.chunk.clone(),
                facts: run.facts,
                child_scope_placements: run.child_scope_placements.clone(),
            };
        }
        let mut facts = OutputFacts::default();
        let mut child_scope_placements = Vec::new();
        for item in items {
            match *item {
                PaintOrderItem::Producer(producer) => {
                    for kind in producer_kinds(producer) {
                        facts.add(self.host.record_producer(scope.owner, kind).facts());
                    }
                }
                PaintOrderItem::Scope(child) => {
                    child_scope_placements.push(self.host.placement_of_row(child.owner));
                    facts.add(self.record_whole_scope(child));
                }
            }
        }
        RecordedRun {
            chunk: self.host.take_run_chunk(),
            facts,
            child_scope_placements: child_scope_placements.into_boxed_slice(),
        }
    }

    fn run_is_clean(
        &mut self,
        scope: PaintScope,
        items: &[PaintOrderItem],
        published: &RecordedRun,
        owner_changes: RowChanges,
    ) -> bool {
        let mut child_scope_placements = published.child_scope_placements.iter();
        items.iter().all(|item| match *item {
            PaintOrderItem::Producer(producer) => !producer_kinds(producer)
                .into_iter()
                .any(|kind| self.producer_records_again(scope.owner, kind, owner_changes)),
            PaintOrderItem::Scope(child) => {
                let recorded_placement = child_scope_placements
                    .next()
                    .expect("a run keeps the origin of each of its child scopes");
                self.scope_is_clean(child, *recorded_placement)
            }
        })
    }

    /// Records a scope with everything below it, as part of a run.
    fn record_whole_scope(&mut self, scope: PaintScope) -> OutputFacts {
        let plan = self.host.plan_scope(scope);
        let mut total = OutputFacts::default();
        if !plan.active {
            self.host
                .observe(AssemblyEvent::ScopeBegin(scope, ScopeAction::Inactive));
            self.host.observe(AssemblyEvent::ScopeEnd(scope));
            return total;
        }
        self.host
            .observe(AssemblyEvent::ScopeBegin(scope, ScopeAction::Recorded));
        for item in plan.items {
            match item {
                PaintOrderItem::Producer(producer) => {
                    for kind in producer_kinds(producer) {
                        total.add(self.host.record_producer(scope.owner, kind).facts());
                    }
                }
                PaintOrderItem::Scope(child_scope) => total.add(self.record_whole_scope(child_scope)),
            }
        }
        self.host.observe(AssemblyEvent::ScopeEnd(scope));
        total
    }
}

/// The segments a walked scope had in the published recording. A plan whose cuts were kept has the same segments as
/// then. One that was cut again is matched by what its segments begin with: a run is the published one when it lists
/// the same items from the same first item.
struct PublishedSegments<'a> {
    published: &'a RecordedScope,
    index_by_key: Option<FastMap<SegmentKey, usize>>,
}

impl<'a> PublishedSegments<'a> {
    fn new(published: &'a RecordedScope, cuts_were_kept: bool) -> Self {
        let index_by_key = (!cuts_were_kept).then(|| {
            published
                .plan
                .segments
                .iter()
                .enumerate()
                .map(|(index, segment)| (Self::key(&published.plan, segment), index))
                .collect()
        });
        Self {
            published,
            index_by_key,
        }
    }

    fn key(plan: &SegmentedPlan, segment: &Segment) -> SegmentKey {
        match segment {
            Segment::Run(run) => SegmentKey::RunStartingAt(plan.items[run.start as usize]),
            Segment::Walked(item) => SegmentKey::Walked(plan.walked_scope(*item)),
        }
    }

    fn matching(&self, index: usize, segment: &Segment, plan: &SegmentedPlan) -> Option<&'a RecordedSegment> {
        let published = self.published;
        let published_index = match &self.index_by_key {
            None => index,
            Some(index_by_key) => {
                let published_index = *index_by_key.get(&Self::key(plan, segment))?;
                if let (Segment::Run(run), Segment::Run(published_run)) =
                    (segment, &published.plan.segments[published_index])
                    && plan.items_of(run) != published.plan.items_of(published_run)
                {
                    return None;
                }
                published_index
            }
        };
        published.segments.get(published_index)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::{CssPixelPoint, CssPixels};
    use crate::painting::display_list::builder::RecordedDisplayList;
    use crate::painting::display_list::recorder::DisplayListRecorder;
    use crate::painting::force_dark::ForceDarkRole;
    use crate::painting::paint_order_plan::{PaintScopeKind, StackingContextPaintPhase};
    use libgfx_rust::{Color, IntRect};
    use std::collections::{HashMap, HashSet};

    fn row(index: u32) -> NodeSlotId {
        NodeSlotId::new(index, 1)
    }

    // Tells the output of one producer kind from another's.
    fn code_of(kind: ProducerKind) -> u8 {
        match kind {
            ProducerKind::DrawBackground => 0,
            ProducerKind::DrawForeground => 1,
            _ => 2,
        }
    }

    fn context(owner: NodeSlotId) -> PaintScope {
        PaintScope::stacking_context(owner)
    }

    fn backgrounds(owner: NodeSlotId) -> PaintScope {
        PaintScope {
            owner,
            kind: PaintScopeKind::Descendants(StackingContextPaintPhase::BackgroundAndBorders),
        }
    }

    fn foreground(owner: NodeSlotId) -> PaintScope {
        PaintScope {
            owner,
            kind: PaintScopeKind::Descendants(StackingContextPaintPhase::Foreground),
        }
    }

    fn phase(phase: PaintPhase) -> PaintOrderItem {
        PaintOrderItem::Producer(PaintProducer::BoxPhase(phase))
    }

    fn child(scope: PaintScope) -> PaintOrderItem {
        PaintOrderItem::Scope(scope)
    }

    #[derive(Clone, Copy)]
    struct Content {
        rects: u32,
        version: u8,
        live: bool,
        reads_window_focus: bool,
        reads_accumulated_scale: bool,
        blocking: u32,
    }

    fn content(rects: u32) -> Content {
        Content {
            rects,
            version: 1,
            live: false,
            reads_window_focus: false,
            reads_accumulated_scale: false,
            blocking: 0,
        }
    }

    struct Frame {
        display_list: RecordedDisplayList,
        root: Arc<RecordedScope>,
        window: Option<(i32, i32)>,
    }

    fn new_output() -> DisplayListRecorder {
        DisplayListRecorder::new(None)
    }

    struct TestHost {
        plans: HashMap<PaintScope, ScopePlan>,
        contents: HashMap<(NodeSlotId, ProducerKind), Content>,
        changes: HashMap<NodeSlotId, RowChanges>,
        origins: HashMap<NodeSlotId, i64>,
        walked: HashSet<PaintScope>,
        grid_rows: HashMap<NodeSlotId, i32>,
        descendant_readers: HashSet<(NodeSlotId, ProducerKind)>,
        recorded: Vec<(NodeSlotId, ProducerKind)>,
        events: Vec<AssemblyEvent>,
        output: DisplayListRecorder,
        source: Option<Frame>,
        // The rows a recording holds what shows in, from the first of them to the last, or none for a recording
        // that holds everything. A row is as tall as a row of the grid.
        window: Option<(i32, i32)>,
        // The grid rows a child scope's owner paints in, from the first to the last.
        rows_painted_in: HashMap<NodeSlotId, (i32, i32)>,
        cutting_off_what_they_hold: HashSet<NodeSlotId>,
        bounds_asked: usize,
    }

    impl Default for TestHost {
        fn default() -> Self {
            Self {
                plans: HashMap::new(),
                contents: HashMap::new(),
                changes: HashMap::new(),
                origins: HashMap::new(),
                walked: HashSet::new(),
                grid_rows: HashMap::new(),
                descendant_readers: HashSet::new(),
                recorded: Vec::new(),
                events: Vec::new(),
                output: new_output(),
                source: None,
                window: None,
                rows_painted_in: HashMap::new(),
                cutting_off_what_they_hold: HashSet::new(),
                bounds_asked: 0,
            }
        }
    }

    const ROW_HEIGHT: f32 = 512.0;

    fn bound_of_rows(first: i32, last: i32) -> InkBound {
        InkBound::in_space(
            crate::painting::display_list::commands::SpatialNodeIndex(0),
            rect_of_rows(first, last),
        )
    }

    fn rect_of_rows(first: i32, last: i32) -> libgfx_rust::FloatRect {
        libgfx_rust::FloatRect::new(
            0.0,
            first as f32 * ROW_HEIGHT,
            100.0,
            (last - first + 1) as f32 * ROW_HEIGHT,
        )
    }

    impl TestHost {
        fn left_out_scopes(&self) -> Vec<PaintScope> {
            self.events
                .iter()
                .filter_map(|event| match event {
                    AssemblyEvent::ScopeLeftOut(scope) => Some(*scope),
                    _ => None,
                })
                .collect()
        }

        fn rows_recorded(&self) -> std::collections::BTreeSet<u32> {
            self.recorded.iter().map(|(owner, _)| owner.slot_index()).collect()
        }

        fn plan(&mut self, scope: PaintScope, active: bool, items: &[PaintOrderItem]) {
            self.plans.insert(
                scope,
                ScopePlan {
                    active,
                    items: items.iter().copied().collect(),
                },
            );
        }

        fn produce(&mut self, owner: NodeSlotId, kind: ProducerKind, content: Content) {
            self.contents.insert((owner, kind), content);
        }

        fn change(&mut self, owner: NodeSlotId, paint_ancestors: &[NodeSlotId]) {
            self.changes.entry(owner).or_default().inputs = true;
            for ancestor in paint_ancestors {
                self.changes.entry(*ancestor).or_default().below = true;
            }
        }

        fn change_order(&mut self, owner: NodeSlotId) {
            self.changes.entry(owner).or_default().order = true;
        }

        fn recorded_rows(&self) -> Vec<u32> {
            let mut rows: Vec<u32> = self.recorded.iter().map(|(owner, _)| owner.slot_index()).collect();
            rows.dedup();
            rows
        }

        fn copied_scopes(&self) -> Vec<PaintScope> {
            self.events
                .iter()
                .filter_map(|event| match event {
                    AssemblyEvent::ScopeCopied(scope) => Some(*scope),
                    _ => None,
                })
                .collect()
        }

        fn action_of(&self, scope: PaintScope) -> Option<ScopeAction> {
            self.events.iter().find_map(|event| match event {
                AssemblyEvent::ScopeBegin(begun, action) if *begun == scope => Some(*action),
                _ => None,
            })
        }
    }

    impl AssemblyHost for TestHost {
        fn plan_scope(&mut self, scope: PaintScope) -> ScopePlan {
            self.plans.get(&scope).cloned().unwrap_or_default()
        }

        fn cut_at(&mut self, _scope: PaintScope, item: PaintOrderItem) -> ItemCut {
            match item {
                PaintOrderItem::Scope(child) if self.walked.contains(&child) => ItemCut::Walked,
                PaintOrderItem::Scope(child) => ItemCut::InRun {
                    grid_row: self.grid_rows.get(&child.owner).copied(),
                },
                PaintOrderItem::Producer(_) => ItemCut::InRun { grid_row: None },
            }
        }

        fn record_producer(&mut self, owner: NodeSlotId, kind: ProducerKind) -> ProducerOutcome {
            let Some(content) = self.contents.get(&(owner, kind)).copied() else {
                return ProducerOutcome::default();
            };
            self.recorded.push((owner, kind));
            let origin = self.origins.get(&owner).copied().unwrap_or_default() as i32;
            for index in 0..content.rects {
                self.output.fill_rect(
                    IntRect {
                        x: owner.slot_index() as i32 + origin,
                        y: i32::from(code_of(kind)) * 100 + index as i32,
                        width: 10,
                        height: i32::from(content.version),
                    },
                    Color::from_rgb(content.version, code_of(kind), 7),
                    ForceDarkRole::Background,
                );
            }
            ProducerOutcome {
                live: content.live,
                reads_window_focus: content.reads_window_focus,
                reads_accumulated_scale: content.reads_accumulated_scale,
                blocking_wheel_event_regions: content.blocking,
            }
        }

        fn take_run_chunk(&mut self) -> Option<Arc<RecordedChunk>> {
            let chunk = self.output.take_chunk();
            (!chunk.records.is_empty()).then(|| RecordedChunk::new(chunk, false))
        }

        fn bound_of_item(&mut self, _scope: PaintScope, item: PaintOrderItem) -> Option<InkBound> {
            self.bounds_asked += 1;
            let PaintOrderItem::Scope(child) = item else {
                return None;
            };
            let (first, last) = *self.rows_painted_in.get(&child.owner)?;
            Some(bound_of_rows(first, last))
        }

        fn is_outside_window(&mut self, bound: &InkBound) -> bool {
            let (first, last) = self.window.expect("only a recording of a window asks");
            let window = rect_of_rows(first, last);
            bound
                .rects()
                .iter()
                .all(|(_, rect)| rect.intersected(window).is_empty())
        }

        fn is_wholly_inside_window(&mut self, bound: &InkBound) -> bool {
            let (first, last) = self.window.expect("only a recording of a window asks");
            let window = rect_of_rows(first, last);
            bound.rects().iter().all(|(_, rect)| window.contains_rect(*rect))
        }

        fn cuts_off_what_it_holds(&mut self, owner: NodeSlotId) -> bool {
            self.cutting_off_what_they_hold.contains(&owner)
        }

        fn changes_of_row(&mut self, row: NodeSlotId) -> RowChanges {
            self.changes.get(&row).copied().unwrap_or_default()
        }

        fn placement_of_row(&mut self, row: NodeSlotId) -> RowPlacement {
            RowPlacement {
                origin: CssPixelPoint::new(
                    CssPixels::from_integer(self.origins.get(&row).copied().unwrap_or_default()),
                    CssPixels::default(),
                ),
                later_fragments: 0,
            }
        }

        fn producer_reads_descendants(&mut self, owner: NodeSlotId, kind: ProducerKind) -> bool {
            self.descendant_readers.contains(&(owner, kind))
        }

        fn observe(&mut self, event: AssemblyEvent) {
            self.events.push(event);
        }
    }

    // Records the canvas prologue and assembles the viewport context from the host's source frame.
    fn record_frame_with_frame_changes(
        host: &mut TestHost,
        window_focus_changed: bool,
        accumulated_scales_changed: bool,
    ) -> Frame {
        host.output = new_output();
        host.recorded.clear();
        host.events.clear();
        host.output.fill_rect(
            IntRect {
                x: 0,
                y: 0,
                width: 800,
                height: 600,
            },
            Color::from_rgb(255, 255, 255),
            ForceDarkRole::Background,
        );
        let prologue = host.output.take_chunk();
        let published = host.source.as_ref().map(|frame| Arc::clone(&frame.root));
        let window = host.window;
        let coverage = match (window, host.source.as_ref().map(|frame| frame.window)) {
            (None, _) => Coverage::WholeDocument,
            (Some(window), Some(Some(published_window))) if window == published_window => Coverage::PublishedWindow,
            (Some(_), _) => Coverage::AnotherWindow,
        };
        let root = Assembler::new(host, window_focus_changed, accumulated_scales_changed, coverage)
            .assemble_root(context(row(0)), published.as_ref());
        let mut chunks = vec![Arc::new(prologue)];
        root.for_each_chunk(&mut |chunk| chunks.push(Arc::clone(chunk.latest())));
        Frame {
            display_list: RecordedDisplayList::of_chunks(chunks.iter().map(|chunk| &**chunk)),
            root,
            window,
        }
    }

    fn record_frame(host: &mut TestHost) -> Frame {
        record_frame_with_frame_changes(host, false, false)
    }

    // Publishes `frame`: the next frame reuses from it, and asks what changed since.
    fn publish(host: &mut TestHost, frame: Frame) {
        host.source = Some(frame);
        host.changes.clear();
    }

    fn assert_matches_from_scratch(host: &mut TestHost, assembled: &Frame) {
        let recorded = std::mem::take(&mut host.recorded);
        let events = std::mem::take(&mut host.events);
        let source = host.source.take();
        let fresh = record_frame(host);
        host.source = source;
        host.recorded = recorded;
        host.events = events;
        assert_eq!(
            assembled.display_list.bytes, fresh.display_list.bytes,
            "command bytes differ"
        );
        assert_eq!(assembled.display_list.command_runs, fresh.display_list.command_runs);
        assert_eq!(assembled.display_list.chunks, fresh.display_list.chunks);
        assert_eq!(assembled.root.facts, fresh.root.facts);
    }

    // A viewport context (row 0) that draws a background and lists the backgrounds and then the foregrounds of
    // six cards (rows 1..=6), two to a grid row. A card draws a background and a foreground.
    fn cards_host() -> TestHost {
        let mut host = TestHost::default();
        let cards = 1..=6;
        let mut items = vec![phase(PaintPhase::Background)];
        items.extend(cards.clone().map(|card| child(backgrounds(row(card)))));
        items.extend(cards.clone().map(|card| child(foreground(row(card)))));
        host.plan(context(row(0)), true, &items);
        host.produce(row(0), ProducerKind::DrawBackground, content(1));
        for card in cards {
            let grid_row = (card as i32 - 1) / 2;
            host.grid_rows.insert(row(card), grid_row);
            host.rows_painted_in.insert(row(card), (grid_row, grid_row));
            host.plan(backgrounds(row(card)), true, &[phase(PaintPhase::Background)]);
            host.plan(foreground(row(card)), true, &[phase(PaintPhase::Foreground)]);
            host.produce(row(card), ProducerKind::DrawBackground, content(1));
            host.produce(row(card), ProducerKind::DrawForeground, content(card));
        }
        host
    }

    #[test]
    fn a_plan_is_cut_where_its_items_leave_a_grid_row_and_around_walked_children() {
        let mut host = cards_host();
        host.walked.insert(foreground(row(4)));

        let frame = record_frame(&mut host);

        // The background and the backgrounds of the cards of a grid row, three times, and then the foregrounds,
        // where the grid starts over and the walked card stands alone.
        assert_eq!(
            frame.root.plan.segments,
            vec![
                Segment::Run(0..3),
                Segment::Run(3..5),
                Segment::Run(5..7),
                Segment::Run(7..9),
                Segment::Run(9..10),
                Segment::Walked(10),
                Segment::Run(11..13),
            ]
        );
        // A chunk for the prologue, one for each of the six runs and one for the run of the walked card.
        assert_eq!(frame.display_list.chunks.len(), 1 + 6 + 1);
        assert!(frame_is_unchanged(
            &frame.root,
            false,
            false,
            false,
            Coverage::WholeDocument
        ));
        assert!(!frame_is_unchanged(
            &frame.root,
            true,
            false,
            false,
            Coverage::WholeDocument
        ));
    }

    #[test]
    fn a_run_never_holds_more_items_than_the_cap() {
        let mut host = TestHost::default();
        let items: Vec<_> = (1..=MAX_ITEMS_IN_A_RUN as u32 + 2)
            .map(|card| child(foreground(row(card))))
            .collect();
        host.plan(context(row(0)), true, &items);

        let frame = record_frame(&mut host);

        let cap = MAX_ITEMS_IN_A_RUN as u32;
        assert_eq!(
            frame.root.plan.segments,
            vec![Segment::Run(0..cap), Segment::Run(cap..cap + 2)]
        );
    }

    #[test]
    fn a_change_records_the_runs_that_hold_the_row_and_the_runs_beside_them_are_reused() {
        let mut host = cards_host();
        let first = record_frame(&mut host);
        publish(&mut host, first);
        host.contents
            .get_mut(&(row(3), ProducerKind::DrawForeground))
            .unwrap()
            .version = 2;
        host.change(row(3), &[row(0)]);

        let frame = record_frame(&mut host);

        // The backgrounds and then the foregrounds of the second grid row record again: the card's stamp says that
        // it changed, not which of its producers did.
        assert_eq!(host.recorded_rows(), vec![3, 4, 3, 4]);
        assert_eq!(host.action_of(context(row(0))), Some(ScopeAction::Assembled));
        assert!(!host.copied_scopes().contains(&backgrounds(row(3))));
        assert!(host.copied_scopes().contains(&backgrounds(row(5))));
        assert!(host.copied_scopes().contains(&foreground(row(2))));
        assert!(!host.copied_scopes().contains(&foreground(row(4))));
        assert_matches_from_scratch(&mut host, &frame);
    }

    #[test]
    fn a_walked_child_is_reused_whole_until_something_below_it_changes() {
        let mut host = cards_host();
        host.walked.insert(foreground(row(4)));
        host.plan(
            foreground(row(4)),
            true,
            &[phase(PaintPhase::Foreground), child(foreground(row(7)))],
        );
        host.plan(foreground(row(7)), true, &[phase(PaintPhase::Foreground)]);
        host.produce(row(7), ProducerKind::DrawForeground, content(2));
        let first = record_frame(&mut host);
        publish(&mut host, first);

        // A change beside the walked child leaves it reused as one piece, with the node the published recording
        // kept for it.
        host.change(row(1), &[row(0)]);
        let second = record_frame(&mut host);
        assert!(host.copied_scopes().contains(&foreground(row(4))));
        assert_eq!(host.action_of(foreground(row(4))), None);
        let walked_child_of = |frame: &Frame| {
            frame
                .root
                .segments
                .iter()
                .find_map(|segment| match segment {
                    RecordedSegment::Walked(scope) => Some(Arc::clone(scope)),
                    RecordedSegment::Run(_) | RecordedSegment::Outside => None,
                })
                .unwrap()
        };
        assert!(Arc::ptr_eq(
            &walked_child_of(&second),
            &walked_child_of(host.source.as_ref().unwrap())
        ));
        assert_matches_from_scratch(&mut host, &second);
        publish(&mut host, second);

        // A change below it has it walked, and the run that holds the changed row recorded. The stamp that something
        // below the card changed does not say in which of its scopes, so the run with the card's backgrounds records
        // too.
        host.contents
            .get_mut(&(row(7), ProducerKind::DrawForeground))
            .unwrap()
            .version = 2;
        host.change(row(7), &[row(4), row(0)]);
        let third = record_frame(&mut host);
        assert_eq!(host.action_of(foreground(row(4))), Some(ScopeAction::Assembled));
        assert_eq!(host.recorded_rows(), vec![3, 4, 7]);
        assert_matches_from_scratch(&mut host, &third);
    }

    #[test]
    fn a_child_scope_an_ancestor_moved_records_again_though_nothing_stamped_it() {
        let mut host = cards_host();
        let first = record_frame(&mut host);
        publish(&mut host, first);
        host.origins.insert(row(5), 40);

        let frame = record_frame(&mut host);

        assert_eq!(host.recorded_rows(), vec![5, 6, 5, 6]);
        assert!(host.copied_scopes().contains(&foreground(row(4))));
        assert_matches_from_scratch(&mut host, &frame);
    }

    #[test]
    fn a_plan_that_gains_an_item_still_copies_the_runs_it_did_not_touch() {
        let mut host = cards_host();
        let first = record_frame(&mut host);
        publish(&mut host, first);
        let mut items = host.plans[&context(row(0))].items.to_vec();
        items.push(child(foreground(row(7))));
        host.plan(context(row(0)), true, &items);
        host.grid_rows.insert(row(7), 3);
        host.plan(foreground(row(7)), true, &[phase(PaintPhase::Foreground)]);
        host.produce(row(7), ProducerKind::DrawForeground, content(1));
        host.change_order(row(0));

        let frame = record_frame(&mut host);

        assert_eq!(host.action_of(context(row(0))), Some(ScopeAction::Replanned));
        assert_eq!(host.recorded_rows(), vec![7]);
        assert_matches_from_scratch(&mut host, &frame);
    }

    #[test]
    fn output_recorded_every_frame_or_read_from_the_window_focus_is_not_copied() {
        let mut host = cards_host();
        host.contents
            .get_mut(&(row(1), ProducerKind::DrawForeground))
            .unwrap()
            .live = true;
        host.contents
            .get_mut(&(row(5), ProducerKind::DrawForeground))
            .unwrap()
            .reads_window_focus = true;
        let first = record_frame(&mut host);
        assert!(!frame_is_unchanged(
            &first.root,
            false,
            false,
            false,
            Coverage::WholeDocument
        ));
        publish(&mut host, first);

        let quiet = record_frame(&mut host);
        assert_eq!(host.recorded_rows(), vec![1, 2]);
        assert_matches_from_scratch(&mut host, &quiet);
        publish(&mut host, quiet);

        let refocused = record_frame_with_frame_changes(&mut host, true, false);
        assert_eq!(host.recorded_rows(), vec![1, 2, 5, 6]);
        assert_matches_from_scratch(&mut host, &refocused);
    }

    #[test]
    fn output_that_read_an_accumulated_scale_is_not_copied_once_one_changed() {
        let mut host = cards_host();
        host.contents
            .get_mut(&(row(5), ProducerKind::DrawForeground))
            .unwrap()
            .reads_accumulated_scale = true;
        let first = record_frame(&mut host);
        assert!(frame_is_unchanged(
            &first.root,
            false,
            false,
            false,
            Coverage::WholeDocument
        ));
        assert!(!frame_is_unchanged(
            &first.root,
            false,
            false,
            true,
            Coverage::WholeDocument
        ));
        publish(&mut host, first);

        let quiet = record_frame(&mut host);
        assert_eq!(host.recorded_rows(), Vec::<u32>::new());
        assert_matches_from_scratch(&mut host, &quiet);
        publish(&mut host, quiet);

        let rescaled = record_frame_with_frame_changes(&mut host, false, true);
        assert_eq!(host.recorded_rows(), vec![5, 6]);
        assert_matches_from_scratch(&mut host, &rescaled);
    }

    #[test]
    fn a_stacking_context_that_stops_painting_records_nothing() {
        let mut host = cards_host();
        let first = record_frame(&mut host);
        publish(&mut host, first);
        let items = host.plans[&context(row(0))].items.to_vec();
        host.plan(context(row(0)), false, &items);
        host.change_order(row(0));

        let frame = record_frame(&mut host);

        assert_eq!(host.action_of(context(row(0))), Some(ScopeAction::Inactive));
        assert!(host.recorded.is_empty());
        assert_eq!(frame.root.facts, OutputFacts::default());
        assert_eq!(frame.display_list.chunks.len(), 1);
    }

    #[test]
    fn a_recording_of_a_window_leaves_out_the_runs_and_walked_children_outside_it() {
        let mut host = cards_host();
        host.walked.insert(foreground(row(4)));
        host.window = Some((2, 2));

        let frame = record_frame(&mut host);

        // The run with the viewport's own background is not bounded, so the backgrounds of the first two cards
        // are recorded with it.
        assert_eq!(
            host.left_out_scopes(),
            vec![
                backgrounds(row(3)),
                backgrounds(row(4)),
                foreground(row(1)),
                foreground(row(2)),
                foreground(row(3)),
                foreground(row(4)),
            ]
        );
        assert_eq!(
            host.rows_recorded().into_iter().collect::<Vec<_>>(),
            vec![0, 1, 2, 5, 6]
        );
        assert!(matches!(frame.root.segments[1], RecordedSegment::Outside));
        assert!(frame.root.leaves_out_what_lies_outside_the_window);
        assert_matches_from_scratch(&mut host, &frame);
    }

    #[test]
    fn a_window_that_moved_records_what_came_into_it_and_drops_what_left_it() {
        let mut host = cards_host();
        host.window = Some((0, 0));
        let first = record_frame(&mut host);
        assert_eq!(host.rows_recorded().into_iter().collect::<Vec<_>>(), vec![0, 1, 2]);
        publish(&mut host, first);

        host.window = Some((1, 2));
        let second = record_frame(&mut host);
        assert_eq!(host.rows_recorded().into_iter().collect::<Vec<_>>(), vec![3, 4, 5, 6]);
        assert_eq!(host.left_out_scopes(), vec![foreground(row(1)), foreground(row(2))]);
        assert!(host.copied_scopes().contains(&backgrounds(row(1))));
        assert_matches_from_scratch(&mut host, &second);
        publish(&mut host, second);

        // With the window where it was, nothing is recorded, and nothing asks what bounds the segments again but
        // for the viewport's scope itself.
        host.bounds_asked = 0;
        let third = record_frame(&mut host);
        assert!(host.recorded.is_empty());
        assert_eq!(host.bounds_asked, 1);
        assert!(frame_is_unchanged(
            &third.root,
            false,
            false,
            false,
            Coverage::PublishedWindow
        ));
        assert!(!frame_is_unchanged(
            &third.root,
            false,
            false,
            false,
            Coverage::AnotherWindow
        ));
        assert!(!frame_is_unchanged(
            &third.root,
            false,
            false,
            false,
            Coverage::WholeDocument
        ));
        assert_matches_from_scratch(&mut host, &third);
    }

    #[test]
    fn a_recording_of_the_whole_document_records_what_a_window_left_out() {
        let mut host = cards_host();
        host.window = Some((0, 0));
        let first = record_frame(&mut host);
        publish(&mut host, first);

        host.window = None;
        let second = record_frame(&mut host);
        assert_eq!(host.rows_recorded().into_iter().collect::<Vec<_>>(), vec![3, 4, 5, 6]);
        assert!(host.left_out_scopes().is_empty());
        assert!(!second.root.leaves_out_what_lies_outside_the_window);
        assert!(frame_is_unchanged(
            &second.root,
            false,
            false,
            false,
            Coverage::WholeDocument
        ));
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn a_clean_walked_child_is_reused_whole_where_neither_window_leaves_any_of_it_out() {
        let mut host = cards_host();
        // The fourth card's foreground is walked, and paints two cards of its own in the second and fourth rows.
        host.walked.insert(foreground(row(4)));
        host.rows_painted_in.insert(row(4), (1, 3));
        host.plan(
            foreground(row(4)),
            true,
            &[
                phase(PaintPhase::Foreground),
                child(foreground(row(7))),
                child(foreground(row(8))),
            ],
        );
        for (card, grid_row) in [(7, 1), (8, 3)] {
            host.grid_rows.insert(row(card), grid_row);
            host.rows_painted_in.insert(row(card), (grid_row, grid_row));
            host.plan(foreground(row(card)), true, &[phase(PaintPhase::Foreground)]);
            host.produce(row(card), ProducerKind::DrawForeground, content(1));
        }
        let record_in_window = |host: &mut TestHost, window: (i32, i32)| {
            host.window = Some(window);
            let frame = record_frame(host);
            assert_matches_from_scratch(host, &frame);
            frame
        };

        // The window cuts the walked card: the card in the fourth row is left out.
        let first = record_in_window(&mut host, (0, 1));
        assert!(host.rows_recorded().contains(&7));
        assert!(host.left_out_scopes().contains(&foreground(row(8))));
        publish(&mut host, first);

        // The window holds all of the walked card now, but the recording it would be reused from left some of it
        // out, so it is walked and what was left out is recorded.
        let second = record_in_window(&mut host, (0, 3));
        assert_eq!(host.action_of(foreground(row(4))), Some(ScopeAction::Assembled));
        assert_eq!(host.rows_recorded().into_iter().collect::<Vec<_>>(), vec![5, 6, 8]);
        publish(&mut host, second);

        // Both windows hold all of it: it is reused whole.
        let third = record_in_window(&mut host, (0, 5));
        assert!(host.copied_scopes().contains(&foreground(row(4))));
        assert_eq!(host.action_of(foreground(row(4))), None);
        assert!(host.recorded.is_empty());
        publish(&mut host, third);

        // The window cuts it again: it is walked, and the card in the fourth row is dropped.
        record_in_window(&mut host, (0, 1));
        assert_eq!(host.action_of(foreground(row(4))), Some(ScopeAction::Assembled));
        assert!(host.left_out_scopes().contains(&foreground(row(8))));
        assert!(host.recorded.is_empty());
    }

    #[test]
    fn what_a_box_cuts_off_is_left_out_though_the_box_is_wholly_inside_the_window() {
        let mut host = cards_host();
        // The fourth card is in the second row, and scrolls two cards of its own, one of them far down.
        host.walked.insert(foreground(row(4)));
        host.cutting_off_what_they_hold.insert(row(4));
        host.plan(
            foreground(row(4)),
            true,
            &[
                phase(PaintPhase::Foreground),
                child(foreground(row(7))),
                child(foreground(row(8))),
            ],
        );
        for (card, grid_row) in [(7, 1), (8, 9)] {
            host.grid_rows.insert(row(card), grid_row);
            host.rows_painted_in.insert(row(card), (grid_row, grid_row));
            host.plan(foreground(row(card)), true, &[phase(PaintPhase::Foreground)]);
            host.produce(row(card), ProducerKind::DrawForeground, content(1));
        }
        host.window = Some((0, 2));

        let frame = record_frame(&mut host);

        assert_eq!(host.left_out_scopes(), vec![foreground(row(8))]);
        assert!(host.rows_recorded().contains(&7));
        assert!(frame.root.leaves_out_what_lies_outside_the_window);
        assert_matches_from_scratch(&mut host, &frame);
    }

    #[test]
    fn a_scope_that_holds_what_a_box_cuts_off_is_not_reused_whole_into_another_window() {
        let mut host = cards_host();
        // The fourth card's foreground is walked and holds a walked scope of a box that scrolls two cards, one of
        // them far down.
        host.walked.insert(foreground(row(4)));
        host.walked.insert(foreground(row(9)));
        host.cutting_off_what_they_hold.insert(row(9));
        host.plan(
            foreground(row(4)),
            true,
            &[phase(PaintPhase::Foreground), child(foreground(row(9)))],
        );
        host.grid_rows.insert(row(9), 1);
        host.rows_painted_in.insert(row(9), (1, 1));
        host.plan(
            foreground(row(9)),
            true,
            &[
                phase(PaintPhase::Foreground),
                child(foreground(row(7))),
                child(foreground(row(8))),
            ],
        );
        for (card, grid_row) in [(7, 1), (8, 9)] {
            host.grid_rows.insert(row(card), grid_row);
            host.rows_painted_in.insert(row(card), (grid_row, grid_row));
            host.plan(foreground(row(card)), true, &[phase(PaintPhase::Foreground)]);
            host.produce(row(card), ProducerKind::DrawForeground, content(1));
        }

        // A recording of the whole document holds the far card.
        let whole = record_frame(&mut host);
        assert!(host.rows_recorded().contains(&8));
        publish(&mut host, whole);

        // The fourth card is wholly inside the window, and nothing of it was left out, but what the box in it cuts
        // off reaches outside: the card is walked and the far card is dropped.
        host.window = Some((0, 2));
        let windowed = record_frame(&mut host);
        assert_eq!(host.action_of(foreground(row(4))), Some(ScopeAction::Assembled));
        assert_eq!(host.left_out_scopes(), vec![foreground(row(8))]);
        assert!(host.recorded.is_empty());
        assert_matches_from_scratch(&mut host, &windowed);
    }
}
