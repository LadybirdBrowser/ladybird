/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The walk that plans a fresh layout tree.
//!
//! It visits the nodes of the styled DOM in tree order and decides what box each one makes, reading the style mirror
//! and nothing else: it writes the rows it plans into chunks the build owns, never a row of the arena, and the style
//! engine is not changed. What needs the boxes in the tree (stamping rows from styles, interning anonymous styles,
//! resolving counters and generated content) is left to the commit and the walk after it, which follow the events
//! this walk records.

use super::child_list::ChildPosition;
use super::content_items::{ContentCounters, ContentPlan, PendingContent, PendingKind, QuoteStep, RetainedQuotes};
use super::counter_walk::{CounterFrame, CounterPlan, CounterWalk, PlacedOwner, ResolvedCounters};
use super::{
    OneByOneRowSource, PlanNode, PlanTree, RowCommittedOneByOne, box_fact, box_facts_of_display, box_facts_of_style,
};
use crate::css::style::RetainedState;
use crate::css::style::bridge::{ElementBoxKind, element_adjustment_fact, element_construction_fact};
use crate::css::style::tree::StyleNodeID;
use crate::layout::ComputedValuesView;
use crate::layout::FfiDisplay;
use crate::layout::commit::{FfiCommitMessage, FfiCommitMessageKind};
use crate::layout::counters::CounterOwner;
use crate::layout::layout_node_arena::LayoutNodeArena;
use crate::layout::math_formatting_context::MathLayoutKind;
use crate::layout::node_data::{
    GENERATED_FOR_AFTER, GENERATED_FOR_BACKDROP, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, NodeFlag, NodeKind,
    pseudo_kind_of,
};
use crate::layout::node_facts;
use crate::layout::tree_builder::{
    ComputedContentType, ElementLayoutKind, FfiPseudoElement, PseudoElementDecision, PseudoElementFacts,
    node_kind_for_display, node_kind_for_element_box_kind, pseudo_element_decision, used_display_of_box,
};
use crate::layout::tree_update_marks::layout_tree_update_reuse_reason;
use libcompositing_rust::node_slot_id::NodeSlotId;

/// How far clearing the stale boxes of a node's subtree reaches: whether the node itself is cleared, and whether SVG
/// resource boxes hanging under a box elsewhere survive (unbounded) or go when their box is inside the cleared root.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum StaleSubtreeScope {
    /// The node alone: what its own box held, and its pseudo-elements'.
    NodeOnly,
    Inclusive,
    InclusiveBoundedToRoot,
    DescendantsBoundedToRoot,
}

/// What the walk after the commit does, in tree order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PlanEvent {
    /// Resolve the counters of an element of the top layer or of SVG resource content, which the walk plans away from
    /// its place in tree order (see `counter_walk.rs`), now that its box, if it has one, is in the tree, and whether its
    /// style may change a counter.
    ResolveCounters {
        element: StyleNodeID,
        may_change_counters: bool,
        /// Whether the element comes after the owners resolved before it in tree order, which the first element of a
        /// top layer member's or SVG resource's subtree does not: it reads what the builds before stored for the
        /// owners before its place in the document.
        in_tree_order: bool,
    },
    /// Resolve the counters of a pseudo-element of the top layer or of SVG resource content, unless they are those of a
    /// `::marker`, which `ResolveMarkerCounters` resolves, and spell the texts of its content that show counter values:
    /// those of the plan's [`ContentPlan::pending`] from `first_pending` up to `end_pending`.
    FillGeneratedContent {
        element: StyleNodeID,
        generated_for: u8,
        first_pending: u32,
        end_pending: u32,
    },
    /// Split the `::first-letter` of a block container's box out of its first text, if it has one.
    SplitFirstLetter(StyleNodeID),
    /// Split the text the document's writing suggestion is shown in around it.
    CreateWritingSuggestionBoxes { element: StyleNodeID, node: NodeSlotId },
    /// Resolve the counters of the `::marker` of an element of the top layer or of SVG resource content, whose box is in
    /// the tree.
    ResolveMarkerCounters(StyleNodeID),
    /// The place of an event the walk had not decided yet when it got there, which turned out to be nothing.
    Nothing,
}

/// Where in the tree the box the plan is for the descendants of stands.
pub(crate) struct PlanContext<'a> {
    pub(crate) has_svg_root: bool,
    /// The boxes the builder is building the children of, outermost first.
    pub(crate) enclosing_ancestor_rows: &'a [NodeSlotId],
    /// The free slots of the arena, which the plan puts rows in before it claims new chunks.
    pub(crate) free_slots: crate::fork_vec::ForkVec<u32>,
}

/// A planned fresh layout tree, and what its build owes besides the boxes.
pub(crate) struct SubtreePlan {
    pub(crate) tree: PlanTree,
    pub(crate) events: Vec<PlanEvent>,
    /// The nodes the walk reached that get no box, each of which clears what its shadow-including subtree held, in the
    /// order the walk reached them, with how far the clear reaches.
    pub(crate) stale_subtrees: Vec<(StyleNodeID, StaleSubtreeScope)>,
    /// The nodes the walk reached that get no box of their own but whose layout tree update marks it answers.
    pub(crate) answered_marks: Vec<StyleNodeID>,
    /// What the build found out that the document has to be told, in the order it found it out.
    pub(crate) reports: Vec<FfiCommitMessage>,
    pub(crate) reached_unstyled_element: bool,
    /// Whether the plan gives a box to a node that had none bound, which places a box outside every rebuild root.
    pub(crate) places_boxes_of_unbound_nodes: bool,
    /// The jobs the walk left the descendants of some of its boxes to, in the order it made them.
    pub(crate) jobs: Vec<DeferredJob>,
    /// Each pair of rows of a child list planned in runs, as `(last child of a run, first child of the next)`, which
    /// the arena links as siblings once it holds them.
    pub(crate) sibling_links_between_runs: Vec<(NodeSlotId, NodeSlotId)>,
    /// What the commit does for the rows the walk made, in lists.
    commit_lists: CommitLists,
    /// The items of the generated content of the pseudo-element boxes the walk made, and what they show.
    pub(crate) content: ContentPlan,
    /// Where the rows of each box the walk left to a job go among the rows of the walk, in pre-order.
    splices: Vec<Splice>,
    /// The name the walk numbered the rows below each box it is for in pre-order, up to its first splice.
    stand_in_names: Vec<u32>,
    /// How far the pre-order positions of the rows below the box the build is for reach, its jobs' included.
    pre_order_extent: u64,
    /// For a plan of a child list that keeps its box (see [`Planner::plan_unit`]), the entries of the list the walk
    /// planned afresh for an element, as `(entry, element)`, and the last entry it kept, see
    /// [`UnitPlanning::fresh_from_entry`].
    pub(crate) fresh_element_entries: Vec<(u32, StyleNodeID)>,
    pub(crate) last_kept_entry: Option<u32>,
    /// What the walk found out about counters, see `counter_walk.rs`.
    counters: CounterPlan,
    /// For the plan of a build, the sets of the owners whose counters the build resolved, which the arena stores before
    /// anything reads them.
    pub(crate) resolved_counters: Option<ResolvedCounters>,
    /// The entries the walk kept the rows of, as `(entry, row)`, in the order of the list.
    pub(crate) kept_entries: Vec<(u32, NodeSlotId)>,
    /// The nodes the walk answered the own layout tree update mark of but not the marks below them: the box of the
    /// list, and the nodes flattened into it, whose kept children may hold marks of their own.
    pub(crate) answered_own_marks: Vec<StyleNodeID>,
}

/// Where the rows a job made below a box of the deferring walk go among the rows of that walk, in pre-order: after the
/// rows the walk numbered before it left the box's descendants, or a run of its children, to the job, and before those it
/// numbered after, below a name of their own.
#[derive(Clone, Copy)]
struct Splice {
    /// The job, and which of its roots the box is.
    job: u32,
    root: u32,
    /// The box the walk is for that the rows are below, by the stand-in for it.
    stand_in: u32,
    /// How far the positions of the rows of the walk below that box that go before reach.
    own_extent_before: u64,
    /// The name the rows of the walk after the job's are numbered below.
    name_after: u32,
    /// How long the lists of what the walk found were there.
    at: OutputLengths,
}

/// How many elements a plan below the element `identity` plans afresh, as far as forking its jobs depends on it: every
/// element of the document for a plan of the document, every element below `identity` for a new subtree, and those
/// below the entries a child list planned again does not keep for one. Children the list keeps have a box and no mark
/// that asks for it again, as do the children before the child a list planned from a window starts at.
fn planned_element_count(
    engine: &RetainedState,
    arena: &LayoutNodeArena,
    identity: StyleNodeID,
    is_document: bool,
    plans_child_list_again: bool,
    window: Option<(Option<StyleNodeID>, Option<StyleNodeID>)>,
) -> usize {
    if is_document && !plans_child_list_again {
        return engine.element_slot_count();
    }
    let element_children = |node: StyleNodeID, pending: &mut Vec<StyleNodeID>| {
        let shadow_root = engine.shadow_root_of(node);
        pending.extend(
            engine
                .dom_children(node)
                .chain(
                    shadow_root
                        .into_iter()
                        .flat_map(|shadow_root| engine.dom_children(shadow_root)),
                )
                .filter(|child| child.element_index().is_some()),
        );
    };
    let mut roots = Vec::new();
    match window {
        Some((start, end)) => {
            let mut child = match start {
                Some(start) => engine.next_sibling_in_dom_order(start),
                None => engine.first_dom_child(identity),
            };
            while let Some(current) = child.filter(|&current| Some(current) != end) {
                if current.element_index().is_some() {
                    roots.push(current);
                }
                child = engine.next_sibling_in_dom_order(current);
            }
        }
        None => element_children(identity, &mut roots),
    }
    if plans_child_list_again {
        let marks = arena.layout_tree_update_marks();
        roots.retain(|&child| {
            arena.bound_row(child).is_invalid() || (marks.needs(child) && marks.reuse_reasons(child) == 0)
        });
    }
    crate::layout::parallel_layout::planned_elements_up_to_parallel_threshold(roots, element_children)
}

/// What the serial commit of a build does for the rows one walk made, written by the walk once its rows are derived:
/// each list holds what the commit does for the rows that need it, so that the commit does it a list at a time, and
/// leaves alone a row that needs nothing else. The walk lists the rows in pre-order.
#[derive(Default)]
struct CommitLists {
    /// The element or text node of each row the walk stamped completely, with the row, which the commit binds. A build
    /// makes at most one such row for each node, so the bindings of one walk go in any order among those of others.
    bindings: Vec<(StyleNodeID, NodeSlotId)>,
    /// The records of the rows the walk stamped completely, each with how many rows in a row are stamped with it, which
    /// the commit pins.
    pins: Vec<(u64, u32)>,
    /// The text rows, which the commit enrolls for their text to be synced.
    text_rows: Vec<NodeSlotId>,
    /// The image resources the box of each element owes the host, which the arena shares once the build is over, in
    /// the order of the walk of the whole build.
    owed_images: std::sync::Arc<crate::layout::layout_node_arena::OwedImageResourcesEntries>,
    /// What the styles of the rows stamped completely say a document may have, see
    /// [`crate::layout::layout_node_arena::planned_style_may_have_facts`].
    style_may_have_facts: u32,
}

/// How long each list of what a walk found that the build goes through in the order of the walk of the whole build was
/// at some point of the walk: where the lists of a job the walk left a box to go among its own.
#[derive(Clone, Copy, Default)]
pub(crate) struct OutputLengths {
    events: u32,
    stale_subtrees: u32,
    reports: u32,
    owed_images: u32,
    rows_committed_one_by_one: u32,
    quote_steps: u32,
}

/// The entries of the list `length` measures in `stretch`.
fn entries_in(stretch: &std::ops::Range<OutputLengths>, length: fn(&OutputLengths) -> u32) -> std::ops::Range<usize> {
    length(&stretch.start) as usize..length(&stretch.end) as usize
}

impl SubtreePlan {
    fn output_lengths(&self) -> OutputLengths {
        OutputLengths {
            events: self.events.len() as u32,
            stale_subtrees: self.stale_subtrees.len() as u32,
            reports: self.reports.len() as u32,
            owed_images: self.commit_lists.owed_images.len() as u32,
            rows_committed_one_by_one: self.tree.rows_committed_one_by_one.len() as u32,
            quote_steps: self.content.quote_steps.len() as u32,
        }
    }

    fn commit_slices(&self, stretch: &std::ops::Range<OutputLengths>) -> CommitSlices<'_> {
        CommitSlices {
            owed_images: (
                &self.commit_lists.owed_images,
                entries_in(stretch, |lengths| lengths.owed_images),
            ),
            rows_committed_one_by_one: &self.tree.rows_committed_one_by_one
                [entries_in(stretch, |lengths| lengths.rows_committed_one_by_one)],
        }
    }
}

/// What the commit does in the order of the walk of the whole build for the rows one walk made between two boxes whose
/// descendants it left to jobs: a stretch of each of the [`CommitLists`] it goes through in that order.
pub(crate) struct CommitSlices<'p> {
    pub(crate) owed_images: (
        &'p std::sync::Arc<crate::layout::layout_node_arena::OwedImageResourcesEntries>,
        std::ops::Range<usize>,
    ),
    pub(crate) rows_committed_one_by_one: &'p [RowCommittedOneByOne],
}

impl CommitSlices<'_> {
    fn is_empty(&self) -> bool {
        self.owed_images.1.is_empty() && self.rows_committed_one_by_one.is_empty()
    }
}

/// What the commit does for the nodes of the rows one walk made, in any order: see [`CommitLists`].
pub(crate) struct CommitNodeLists<'p> {
    pub(crate) bindings: &'p [(StyleNodeID, NodeSlotId)],
    pub(crate) pins: &'p [(u64, u32)],
    pub(crate) text_rows: &'p [NodeSlotId],
}

impl SubtreePlan {
    pub(crate) fn counters_mut(&mut self) -> &mut CounterPlan {
        &mut self.counters
    }

    pub(crate) fn counters_and_jobs_mut(&mut self) -> (&CounterPlan, &mut [DeferredJob]) {
        (&self.counters, &mut self.jobs)
    }

    /// Replays what the walk found out about counters, once its jobs ran, see [`CounterPlan::replay`].
    fn replay_counters(&mut self) {
        let job_plans: Vec<Option<&CounterPlan>> = self
            .jobs
            .iter()
            .map(|job| job.plan.as_ref().map(|plan| &plan.counters))
            .collect();
        self.counters.replay(&job_plans);
    }

    /// Spells every text of the generated content of the plan and of its jobs that waits for the quote depth or counter
    /// values, once the build resolved the counters, but for those whose counters the build resolves after the commit.
    /// `retained` tells the depth before the plan, and what boxes a child list planned again keeps do to it.
    pub(crate) fn fill_generated_content(
        &mut self,
        engine: &RetainedState,
        arena: &LayoutNodeArena,
        retained: &mut dyn RetainedQuotes,
    ) {
        let mut shows_quote_depth = false;
        self.for_each_plan(&mut |plan| shows_quote_depth |= plan.content.shows_quote_depth());
        if shows_quote_depth {
            let depth = retained.entry_depth();
            self.assign_quote_depths(0..self.content.quote_steps.len(), depth, retained);
        }
        let mut instantiated = Vec::new();
        self.fill_contents(engine, arena, &mut instantiated);
        if !instantiated.is_empty() {
            self.resolved_counters
                .as_mut()
                .expect("the build resolved its counters")
                .sets
                .extend(instantiated);
        }
    }

    /// Hands each content of the quote steps `steps` of the walk the quote depth before it, from `depth` before them,
    /// down the jobs, and answers the depth after them.
    fn assign_quote_depths(
        &mut self,
        steps: std::ops::Range<usize>,
        mut depth: u32,
        retained: &mut dyn RetainedQuotes,
    ) -> u32 {
        let mut saved_depths = Vec::new();
        for index in steps {
            match self.content.quote_steps[index] {
                QuoteStep::Content { pending, transfer } => {
                    if let Some(pending) = pending {
                        self.content.pending[pending as usize].quote_depth = Some(depth);
                    }
                    depth = transfer.apply(depth);
                }
                QuoteStep::Hole { job, root } => {
                    let job = &mut self.jobs[job as usize];
                    let steps = entries_in(&job.roots[root as usize].outputs, |lengths| lengths.quote_steps);
                    let plan = job.plan.as_mut().expect("a job ran before its plan is filled");
                    depth = plan.assign_quote_depths(steps, depth, retained);
                }
                QuoteStep::Save => saved_depths.push(depth),
                QuoteStep::Restore => depth = saved_depths.pop().expect("a restored quote depth was saved"),
                QuoteStep::KeptElement(element) => depth = retained.after_kept_element(element, depth),
                QuoteStep::KeptPseudoElement(element, generated_for) => {
                    depth = retained.after_kept_pseudo_element(element, generated_for, depth);
                }
                QuoteStep::Nothing => {}
            }
        }
        depth
    }

    fn fill_contents(
        &mut self,
        engine: &RetainedState,
        arena: &LayoutNodeArena,
        instantiated: &mut Vec<(CounterOwner, Option<crate::layout::counters::CountersSet>)>,
    ) {
        if !self.content.pending.is_empty() {
            super::content_items::fill_plan_contents(engine, arena, &mut self.content, &self.counters);
            instantiated.append(&mut self.content.instantiated_counters);
        }
        for job in &mut self.jobs {
            if let Some(plan) = job.plan.as_mut() {
                plan.fill_contents(engine, arena, instantiated);
            }
        }
    }

    /// Calls `visit` with this plan and the plan of every job below it.
    /// Takes what the jobs of the walk found once every one of them ran, and gives the boxes the walk left to them the
    /// children the jobs planned.
    fn take_job_results(&mut self, shared: &JobShared<'_>) {
        if self.jobs.is_empty() {
            return;
        }
        let results: Vec<(Vec<DeferredRoot>, SubtreePlan)> = self
            .jobs
            .iter_mut()
            .map(|job| job.slot.take().expect("every job is sealed").take_result(shared))
            .collect();
        // The job, if any, whose plan holds the last child so far of each box whose children were planned in runs.
        let mut last_child_of_split_boxes: crate::css::style::fast_hash::FastMap<NodeSlotId, Option<usize>> =
            Default::default();
        for job in 0..results.len() {
            for index in 0..results[job].0.len() {
                let node = results[job].0[index].node;
                if results[job].0[index].children.is_none() {
                    self.tree.take_children_of_stand_in(node, &results[job].1.tree);
                    continue;
                }
                // NB: The job's stand-in for the box is named by the box's slot.
                let stand_in = results[job].1.tree.node(node);
                let (first_child, last_child, children_facts) =
                    (stand_in.first_child(), stand_in.last_child(), stand_in.facts());
                if first_child.is_invalid() {
                    continue;
                }
                let holder = last_child_of_split_boxes.entry(node).or_insert(None);
                let previous_last_child = match *holder {
                    None => self.tree.node(node).last_child(),
                    Some(previous_job) => results[previous_job].1.tree.node(node).last_child(),
                };
                // NB: The rows of a job may be in a chunk it handed on, so the arena links runs once it holds them.
                if !previous_last_child.is_invalid() {
                    self.sibling_links_between_runs.push((previous_last_child, first_child));
                }
                *holder = Some(job);
                self.tree
                    .append_children_of_stand_in(node, first_child, last_child, children_facts);
            }
        }
        for (job, (roots, plan)) in self.jobs.iter_mut().zip(results) {
            self.reached_unstyled_element |= plan.reached_unstyled_element;
            self.places_boxes_of_unbound_nodes |= plan.places_boxes_of_unbound_nodes;
            job.roots = roots;
            job.plan = Some(plan);
        }
    }

    /// Adds to how far the positions of the rows below each of `own_roots`, the boxes this walk is a job for, reach those
    /// of the rows the jobs of this walk made below it, once they ran.
    fn count_rows_of_jobs(&self, own_roots: &mut [DeferredRoot]) {
        for splice in &self.splices {
            own_roots[splice.stand_in as usize].pre_order_size +=
                self.jobs[splice.job as usize].roots[splice.root as usize].pre_order_size;
        }
    }

    pub(crate) fn for_each_plan<'p>(&'p self, visit: &mut impl FnMut(&'p SubtreePlan)) {
        visit(self);
        for job in &self.jobs {
            if let Some(plan) = &job.plan {
                plan.for_each_plan(visit);
            }
        }
    }

    /// Takes the rows this plan and the plans of its jobs made: the chunks they filled, in the order of their indices,
    /// and the rows they put in free slots of the arena.
    pub(crate) fn take_rows(&mut self) -> (Vec<super::FilledChunk>, Vec<super::ReusedRows>) {
        let mut filled = Vec::new();
        let mut reused = Vec::new();
        self.take_rows_into(&mut filled, &mut reused);
        filled.sort_by_key(|chunk| chunk.index);
        (filled, reused)
    }

    fn take_rows_into(&mut self, filled: &mut Vec<super::FilledChunk>, reused: &mut Vec<super::ReusedRows>) {
        let (own_filled, own_reused) = self.tree.take_rows();
        filled.extend(self.tree.claims().take_open_chunks());
        filled.extend(own_filled);
        reused.extend(own_reused);
        for job in &mut self.jobs {
            if let Some(plan) = &mut job.plan {
                plan.take_rows_into(filled, reused);
            }
        }
    }

    /// Gives up the plan before anything of it is committed: the rows it put in free slots of the arena leave them free
    /// again. Answers the free slots of the arena, for the caller to give back.
    pub(crate) fn discard(mut self) -> crate::fork_vec::ForkVec<u32> {
        let (_, reused) = self.take_rows();
        let claims = self.tree.claims();
        for rows in &reused {
            claims.give_back_free_slots(&rows.slots);
        }
        claims.take_free_slots()
    }

    /// Calls `visit` with each stale subtree the walk and its jobs found, in the order a walk without jobs finds them.
    pub(crate) fn for_each_stale_subtree(&self, visit: &mut impl FnMut(StyleNodeID, StaleSubtreeScope)) {
        self.for_each_stretch_in_walk_order(&mut |plan, stretch| {
            for &(node, scope) in &plan.stale_subtrees[entries_in(&stretch, |lengths| lengths.stale_subtrees)] {
                visit(node, scope);
            }
        });
    }

    /// The reports the walk and its jobs made, in the order a walk without jobs makes them.
    pub(crate) fn reports_in_walk_order(&self) -> Vec<FfiCommitMessage> {
        let mut reports = Vec::new();
        self.for_each_stretch_in_walk_order(&mut |plan, stretch| {
            reports.extend_from_slice(&plan.reports[entries_in(&stretch, |lengths| lengths.reports)]);
        });
        reports
    }

    /// Calls `visit` with each event the walk and its jobs recorded, with the plan it is of, in the order a walk without
    /// jobs records them.
    pub(crate) fn for_each_event_in_walk_order<'p>(&'p self, visit: &mut impl FnMut(&'p SubtreePlan, &'p PlanEvent)) {
        self.for_each_stretch_in_walk_order(&mut |plan, stretch| {
            for event in &plan.events[entries_in(&stretch, |lengths| lengths.events)] {
                visit(plan, event);
            }
        });
    }

    /// Calls `visit` with what the commit does for the rows the walk and its jobs made, in stretches of their lists,
    /// each with the plan whose rows it is for, in pre-order.
    pub(crate) fn for_each_commit_slice_in_walk_order<'p>(&'p self, visit: &mut impl FnMut(CommitSlices<'p>)) {
        self.for_each_stretch_in_walk_order(&mut |plan, stretch| {
            let slices = plan.commit_slices(&stretch);
            if !slices.is_empty() {
                visit(slices);
            }
        });
    }

    /// Calls `visit` with each plan of the walk and its jobs and a stretch of its lists of what it found, in the order a
    /// walk without jobs finds what they hold: a job's go where the walk left the box they are below to it.
    fn for_each_stretch_in_walk_order<'p>(
        &'p self,
        visit: &mut dyn FnMut(&'p SubtreePlan, std::ops::Range<OutputLengths>),
    ) {
        self.visit_stretches_in_walk_order(
            OutputLengths::default()..self.output_lengths(),
            0..self.splices.len(),
            visit,
        );
    }

    fn visit_stretches_in_walk_order<'p>(
        &'p self,
        outputs: std::ops::Range<OutputLengths>,
        splices: std::ops::Range<usize>,
        visit: &mut dyn FnMut(&'p SubtreePlan, std::ops::Range<OutputLengths>),
    ) {
        let mut from = outputs.start;
        for splice in &self.splices[splices] {
            visit(self, from..splice.at);
            from = splice.at;
            let job = &self.jobs[splice.job as usize];
            let root = &job.roots[splice.root as usize];
            job.plan
                .as_ref()
                .expect("a job ran before its plan is committed")
                .visit_stretches_in_walk_order(root.outputs.clone(), root.splices.clone(), visit);
        }
        visit(self, from..outputs.end);
    }

    /// Where the rows numbered below each name the walk and its jobs numbered rows below in pre-order start in the
    /// pre-order of the subtree the build made, in units of their positions (see
    /// [`super::ChunkColumns::pre_order_positions`]).
    pub(crate) fn pre_order_starts(&self) -> Vec<u64> {
        let mut starts = vec![0; self.tree.claims().pre_order_root_count()];
        self.fill_pre_order_starts(&[0], &mut starts);
        starts
    }

    /// How far the pre-order positions of the rows the build made reach, see [`Self::pre_order_starts`].
    pub(crate) fn pre_order_extent(&self) -> u64 {
        self.pre_order_extent
    }

    /// Writes where the rows numbered below each name the walk and its jobs numbered rows below start into `starts`, by
    /// the name: those below the boxes this walk is for start at `own_starts`.
    fn fill_pre_order_starts(&self, own_starts: &[u64], starts: &mut [u64]) {
        for (&name, &start) in self.stand_in_names.iter().zip(own_starts) {
            starts[name as usize] = start;
        }
        let mut job_starts: Vec<Vec<u64>> = self.jobs.iter().map(|job| vec![0; job.roots.len()]).collect();
        // NB: How far the rows the jobs spliced in so far below the current box the walk is for reach.
        let (mut stand_in, mut extent_of_jobs_before) = (u32::MAX, 0);
        for splice in &self.splices {
            if splice.stand_in != stand_in {
                (stand_in, extent_of_jobs_before) = (splice.stand_in, 0);
            }
            let own_start = own_starts[stand_in as usize] + splice.own_extent_before;
            job_starts[splice.job as usize][splice.root as usize] = own_start + extent_of_jobs_before;
            extent_of_jobs_before += self.jobs[splice.job as usize].roots[splice.root as usize].pre_order_size;
            // NB: The rows after the splice start after the job's.
            starts[splice.name_after as usize] = own_start + extent_of_jobs_before;
        }
        for (job, job_starts) in self.jobs.iter().zip(job_starts) {
            if let Some(plan) = &job.plan {
                plan.fill_pre_order_starts(&job_starts, starts);
            }
        }
    }

    /// Whether the walk or a job put rows out of the order of their pre-order positions, which the arena then labels
    /// again.
    pub(crate) fn pre_order_positions_out_of_order(&self) -> bool {
        let mut out_of_order = false;
        self.for_each_plan(&mut |plan| out_of_order |= plan.tree.pre_order_positions_out_of_order());
        out_of_order
    }

    /// What the commit does for the nodes of the rows of this walk, not of its jobs.
    pub(crate) fn commit_node_lists(&self) -> CommitNodeLists<'_> {
        CommitNodeLists {
            bindings: &self.commit_lists.bindings,
            pins: &self.commit_lists.pins,
            text_rows: &self.commit_lists.text_rows,
        }
    }

    /// What the styles of the rows the walk and its jobs stamped completely say a document may have, see
    /// [`crate::layout::layout_node_arena::planned_style_may_have_facts`].
    pub(crate) fn style_may_have_facts(&self) -> u32 {
        let mut facts = self.commit_lists.style_may_have_facts;
        for job in &self.jobs {
            if let Some(plan) = &job.plan {
                facts |= plan.style_may_have_facts();
            }
        }
        facts
    }
}

/// Fetches what the walk reads of the elements of a child list some children ahead of planning them, a stage of
/// [`crate::css::style::LayoutInputsPrefetch`] at a time, each closer to the child the walk plans than the one it reads.
/// OPTIMIZATION: The walk reads each element through a chain of loads that miss the cache where the style pass left
///               the element's style long ago; asking for the loads of the children ahead overlaps their misses.
struct ChildReadAhead {
    /// The child each stage fetches next, the stages in the order of [`Self::STAGES`].
    cursors: [Option<StyleNodeID>; 3],
}

impl ChildReadAhead {
    const STAGES: [(crate::css::style::LayoutInputsPrefetch, usize); 3] = [
        (crate::css::style::LayoutInputsPrefetch::Rows, 8),
        (crate::css::style::LayoutInputsPrefetch::Record, 4),
        (crate::css::style::LayoutInputsPrefetch::Groups, 2),
    ];

    fn new(engine: &RetainedState, first_child: Option<StyleNodeID>) -> Self {
        let mut read_ahead = Self {
            cursors: [first_child; 3],
        };
        for (stage, &(kind, distance)) in Self::STAGES.iter().enumerate() {
            for _ in 0..distance {
                read_ahead.fetch(engine, stage, kind);
            }
        }
        read_ahead
    }

    /// Fetches the stages of the children one child further ahead.
    fn advance(&mut self, engine: &RetainedState) {
        for (stage, &(kind, _)) in Self::STAGES.iter().enumerate() {
            self.fetch(engine, stage, kind);
        }
    }

    fn fetch(&mut self, engine: &RetainedState, stage: usize, kind: crate::css::style::LayoutInputsPrefetch) {
        let Some(child) = self.cursors[stage] else {
            return;
        };
        engine.prefetch_element_layout_inputs(child, kind);
        self.cursors[stage] = engine.next_sibling_in_dom_order(child);
    }
}

/// What planning the descendants of an element's box reads of the element, besides its box.
#[derive(Clone, Copy)]
struct DescendantFacts {
    /// See `bridge::element_construction_fact`.
    construction_facts: u32,
    content_visibility: u8,
    publishes_no_pseudo_element_records: bool,
    may_change_counters: bool,
    /// See [`crate::painting::style_queries::has_style_containment`].
    has_style_containment: bool,
}

/// A job planning the descendants of boxes the walk that made the boxes left to it, and its plan once it ran.
pub(crate) struct DeferredJob {
    pub(crate) roots: Vec<DeferredRoot>,
    pub(crate) plan: Option<SubtreePlan>,
    /// Where the job is once the walk that made it got past its boxes, until it ran.
    slot: Option<std::sync::Arc<JobSlot>>,
}

/// A job a walk made and got past the boxes of, which the first thread to take it runs: a style layout thread it was
/// handed to, or the thread of the walk once that is done.
struct JobSlot {
    state: std::sync::Mutex<JobState>,
}

enum JobState {
    Waiting(JobSpec),
    Running,
    Ran(Box<(Vec<DeferredRoot>, SubtreePlan)>),
}

impl JobSlot {
    fn new(spec: JobSpec) -> Self {
        Self {
            state: std::sync::Mutex::new(JobState::Waiting(spec)),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, JobState> {
        self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// Runs the job, unless another thread took it.
    fn run(&self, engine: &RetainedState, arena: &LayoutNodeArena, shared: &JobShared<'_>) {
        let mut state = self.lock();
        let spec = match std::mem::replace(&mut *state, JobState::Running) {
            JobState::Waiting(spec) => spec,
            taken => {
                *state = taken;
                return;
            }
        };
        drop(state);
        let plan = Planner::run_job(engine, arena, shared, spec);
        *self.lock() = JobState::Ran(Box::new(plan));
        shared.progress.note_from_outside_pool();
    }

    /// Takes what the job found, once it ran. A job the thread that started the build took may still run there after
    /// the scope it was handed on in ended. The thread waiting for it runs work of the pool meanwhile, as that job may
    /// hand jobs on and wait for the pool to run them, and sleeps while the pool has none for it.
    fn take_result(&self, shared: &JobShared<'_>) -> (Vec<DeferredRoot>, SubtreePlan) {
        loop {
            let seen = shared.progress.now();
            let mut state = self.lock();
            match std::mem::replace(&mut *state, JobState::Running) {
                JobState::Ran(result) => return *result,
                running => *state = running,
            }
            drop(state);
            if rayon::yield_now() != Some(rayon::Yield::Executed) {
                shared.progress.wait_past(seen);
            }
        }
    }
}

/// How often the thread that started a build, which is none of the pool's, handed a job to the pool or finished one.
/// A thread of the pool waiting for a job that thread runs sleeps until one of these happens.
// NB: The pool wakes its own sleeping threads for the work it is handed, but not one sleeping here.
#[derive(Default)]
struct JobProgress {
    count: std::sync::Mutex<u64>,
    changed: std::sync::Condvar,
}

impl JobProgress {
    fn now(&self) -> u64 {
        *self.count.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn note_from_outside_pool(&self) {
        if rayon::current_thread_index().is_some() {
            return;
        }
        *self.count.lock().unwrap_or_else(std::sync::PoisonError::into_inner) += 1;
        self.changed.notify_all();
    }

    fn wait_past(&self, seen: u64) {
        let count = self.count.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        drop(
            self.changed
                .wait_while(count, |count| *count == seen)
                .unwrap_or_else(std::sync::PoisonError::into_inner),
        );
    }
}

/// Hands the jobs a walk makes to threads that run them while the walk goes on.
trait DispatchJob {
    fn dispatch(&self, slot: &std::sync::Arc<JobSlot>);
}

/// Hands jobs to the style layout threads through the scope the walk runs in.
struct JobDispatch<'s, 'scope> {
    scope: &'s rayon::Scope<'scope>,
    engine: &'scope RetainedState,
    arena: &'scope LayoutNodeArena,
    shared: &'scope JobShared<'scope>,
}

impl DispatchJob for JobDispatch<'_, '_> {
    fn dispatch(&self, slot: &std::sync::Arc<JobSlot>) {
        self.shared
            .handed_on
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .push_back(slot.clone());
        let (engine, arena, shared, slot) = (self.engine, self.arena, self.shared, slot.clone());
        self.scope.spawn(move |_| slot.run(engine, arena, shared));
        self.shared.progress.note_from_outside_pool();
    }
}

/// A box whose descendants a job plans, and where what the job found goes among what the deferring walk found.
pub(crate) struct DeferredRoot {
    node: NodeSlotId,
    element: StyleNodeID,
    descendant_facts: DescendantFacts,
    /// For a run of the children of a long child list, those children, which the job places under the box after
    /// what the deferring walk and the jobs of the runs before placed there; otherwise none, and the job plans the
    /// descendants of the box's element.
    children: Option<Vec<StyleNodeID>>,
    /// What the job's walk starts from as far as counters go, or none where the box is in the top layer or SVG resource
    /// content, whose counters are resolved after the commit.
    counter_frame: Option<CounterFrame>,
    /// Which stretch of the lists of what the job's walk found is of the rows below this box, and which of the job's
    /// splices are of the boxes below it.
    outputs: std::ops::Range<OutputLengths>,
    splices: std::ops::Range<usize>,
    /// How far the pre-order positions of the rows the job made below the box reach, its own jobs' included.
    pre_order_size: u64,
}

/// What every job of one build shares: whether it may make more jobs.
#[derive(Clone)]
struct ForkBudget {
    jobs_left: std::sync::Arc<std::sync::atomic::AtomicUsize>,
    threads: usize,
    splits: ListSplits,
}

impl ForkBudget {
    /// How many jobs a build makes at most, per thread it runs them on.
    const JOBS_PER_THREAD: usize = 32;
    /// How many jobs the boxes of one child list are split into at most, per thread.
    const CHILD_LIST_JOBS_PER_THREAD: usize = 4;
    /// How many boxes of a child list a job takes at least, and at most.
    const MINIMUM_ROOTS_PER_JOB: usize = 16;
    const MAXIMUM_ROOTS_PER_JOB: usize = 128;
    /// How many children a child list has at least for the walk to split it into jobs.
    const MINIMUM_CHILDREN_TO_SPLIT: usize = 8;

    fn take_jobs(&self, count: usize) -> bool {
        self.jobs_left
            .fetch_update(
                std::sync::atomic::Ordering::Relaxed,
                std::sync::atomic::Ordering::Relaxed,
                |left| left.checked_sub(count),
            )
            .is_ok()
    }

    fn take_job(&self) -> bool {
        self.jobs_left
            .fetch_update(
                std::sync::atomic::Ordering::Relaxed,
                std::sync::atomic::Ordering::Relaxed,
                |left| left.checked_sub(1),
            )
            .is_ok()
    }
}

/// The job the boxes of the child list the walk is in go to, and how many more it takes.
#[derive(Clone, Copy)]
struct ChildListBatch {
    job: Option<u32>,
    roots_left: usize,
    capacity: usize,
}

/// The nearest box before a white space text among the boxes its layout parent lays out, as far as the text cares.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PrecedingContent {
    /// A text, into whose trailing collapsible white space a later text's white space collapses.
    Text { ends_in_collapsible_white_space: bool },
    /// A box, which counts as what it is when a text asks, unless it is out of flow then.
    Box(NodeSlotId),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PrecedingBox {
    Nothing,
    Text { ends_in_collapsible_white_space: bool },
    InlineLevel,
    TableInternal,
    LineStart,
}

pub(crate) struct Planner<'a> {
    engine: &'a RetainedState,
    arena: &'a LayoutNodeArena,
    tree: PlanTree,
    events: Vec<PlanEvent>,
    stale_subtrees: Vec<(StyleNodeID, StaleSubtreeScope)>,
    answered_marks: Vec<StyleNodeID>,
    reports: Vec<FfiCommitMessage>,
    reached_unstyled_element: bool,
    /// What came before, in each child list the walk is in, nested in the order the walk entered them.
    preceding_content: Vec<PrecedingContent>,
    /// Where what came before in the child list the walk is in starts in `preceding_content`.
    preceding_content_start: usize,
    /// Whether the plan gives a box to a node that had none bound, which places a box outside every rebuild root.
    places_boxes_of_unbound_nodes: bool,
    /// The style of the box the plan is for the descendants of.
    root_style: Option<ComputedValuesView<'a>>,
    /// The parent of the text the document's writing suggestion is shown in, if it shows one.
    writing_suggestion_parent: Option<StyleNodeID>,
    /// Whether the walk is building the document's top layer members, rather than skipping them where they are.
    layout_top_layer: bool,
    /// Whether the walk is inside an `<svg>`, and not inside a `<foreignObject>` in it.
    has_svg_root: bool,
    /// Whether the next element the walk plans is the root of a mask or clip path resource.
    layout_svg_mask_or_clip_path: bool,
    /// Whether the next element the walk plans is the root of a pattern resource.
    layout_svg_pattern: bool,
    /// The latest box planned for each SVG resource element, which its element is bound to once committed.
    latest_resource_boxes: crate::css::style::fast_hash::FastMap<StyleNodeID, NodeSlotId>,
    /// The rows the build is building the children of, above the plan's root, which is the last of them.
    enclosing_ancestor_rows: &'a [NodeSlotId],
    root_row: NodeSlotId,
    /// Whether the walk is inside SVG content, a `<foreignObject>` included.
    inside_svg_content: bool,
    fork_budget: Option<ForkBudget>,
    /// Where the walk hands the jobs it makes, where threads run them while it goes on.
    dispatch: Option<&'a dyn DispatchJob>,
    batch: Option<ChildListBatch>,
    jobs: Vec<DeferredJob>,
    sibling_links_between_runs: Vec<(NodeSlotId, NodeSlotId)>,
    /// Whether the next child list the walk plans may be planned in runs, see [`Self::plan_dom_children_in_runs`]:
    /// nothing goes after it among the children of its box.
    child_list_may_split: bool,
    /// Whether the child list the walk is in is that of a table-column-group box, which keeps only its table-column
    /// children.
    in_column_group_list: bool,
    commit_lists: CommitLists,
    /// The image resources the rows owe, listed as the walk makes the rows, until it shares them in `commit_lists`.
    owed_images: crate::layout::layout_node_arena::OwedImageResourcesEntries,
    splices: Vec<Splice>,
    stand_in_names: Vec<u32>,
    /// How far the positions of the rows numbered below the box the walk is in now, up to the current name, reach.
    own_pre_order_extent: u64,
    /// What the walk is told of the child list it plans again, where it plans one, see [`Self::plan_unit`].
    unit: Option<UnitPlanning>,
    /// The element whose box's child list the walk plans again.
    unit_element: Option<StyleNodeID>,
    /// Whether the walk is in that child list: below the box, its anonymous boxes, or the nodes flattened into it.
    in_unit_list: bool,
    /// How many entries of that list the walk reached so far, and the one it is planning.
    unit_entry_count: u32,
    unit_entry: u32,
    /// The pre-order label of the row the walk kept last.
    last_kept_label: u64,
    fresh_element_entries: Vec<(u32, StyleNodeID)>,
    last_kept_entry: Option<u32>,
    kept_entries: Vec<(u32, NodeSlotId)>,
    answered_own_marks: Vec<StyleNodeID>,
    counters: CounterWalk,
    /// Whether the walk is in the subtree of a top layer member or of SVG resource content, whose counters are resolved
    /// after the commit, one owner at a time.
    resolves_counters_after_commit: bool,
    /// Whether the next owner of counters the walk plans reads what the builds before stored for the owners before its
    /// place in the document: a top layer member or the root of SVG resource content.
    restarts_counters_at_next_owner: bool,
    content: ContentPlan,
}

/// How the counters of an element whose box's descendants the walk plans are resolved.
enum OwnerCounters {
    /// The walk placed the element, see [`CounterWalk::place_owner`].
    Placed(PlacedOwner),
    /// The job's walk starts below the element, from what the walk that placed it handed on.
    Entered,
    /// After the commit, see `PlanEvent::ResolveCounters`.
    AfterCommit { in_tree_order: bool },
}

/// Whether `child`, a child of a long child list outside SVG content and the top layer, may change a counter the siblings
/// after it hold: it may change a counter, and takes part in counter resolution as an element whose box the walk plans,
/// or as a `display: contents` element.
fn child_list_child_may_change_counters(engine: &RetainedState, arena: &LayoutNodeArena, child: StyleNodeID) -> bool {
    if child.text_index().is_some() {
        return false;
    }
    if !engine
        .element_layout_inputs(child)
        .record
        .is_some_and(|record| record.summary().may_change_counters)
    {
        return false;
    }
    let prepared = prepare_element(engine, arena, child, false, false);
    let has = |fact: u32| prepared.type_facts & fact != 0;
    // NB: See plan_element(): a top layer member is skipped at its place, and an SVG element outside an `<svg>` gets no
    //     box.
    if has(element_adjustment_fact::RENDERED_IN_TOP_LAYER)
        || (!has(element_adjustment_fact::IS_SVG_CONTAINER) && has(element_adjustment_fact::REQUIRES_SVG_CONTAINER))
    {
        return false;
    }
    let Some(style) = prepared.style else {
        return false;
    };
    !style.display.is_none() && (style.display.is_contents() || style.principal_box.is_some())
}

/// What the walk that plans the child list of a box that keeps its box again is told, see [`Planner::plan_unit`].
#[derive(Clone, Copy)]
pub(crate) struct UnitPlanning {
    /// Whether the box's own `::before`, `::after` and `::marker` boxes may be kept: no mark asked for them again.
    pub(crate) keeps_pseudo_element_boxes: bool,
    /// The first entry of the list, in the order the walk reaches them, that the walk plans afresh whatever it finds,
    /// and every one after it: what the entries before it change in counters or quotes reaches the ones after.
    pub(crate) fresh_from_entry: u32,
    /// Whether the walk asserts that every element box it keeps is the box a fresh build makes, as debug builds and
    /// `LIBWEB_VERIFY_KEPT_LAYOUT_BOXES=1` ask: a kept box that is not is one whose change no mark told the build of.
    pub(crate) verifies_kept_boxes: bool,
    /// The part of the list the walk plans, for a list that only gains children at its end or in its last run of
    /// inline content, or only changes between two boxes it keeps. The children outside it, and the box's own
    /// pseudo-element boxes, stay as they are.
    pub(crate) window: Option<ListWindow>,
}

/// The part of a child list the walk plans: from a child it keeps, or from the start of the list, to the end of the list
/// or to another child it keeps, where every change of the list lies between the two. The list's own pseudo-element
/// boxes stay as they are.
#[derive(Clone, Copy)]
pub(crate) struct ListWindow {
    /// The child the walk plans the list from, which the walk keeps: the last child that has a box, which goes last
    /// among the boxes of the list, or the first one whose box `wrapper` holds, or one whose box goes before the changes,
    /// or none where the window runs from the start of the list.
    pub(crate) start: Option<StyleNodeID>,
    /// The anonymous box that wraps the run of inline content the list ends with, which the walk plans again in an
    /// anonymous box of its own that takes it over, or the invalid slot where the window starts after it.
    pub(crate) wrapper: NodeSlotId,
    /// The child the walk plans the list up to, which the walk keeps, whose box goes after the changes, or none where
    /// the window runs to the end of the list.
    pub(crate) end: Option<StyleNodeID>,
}

impl<'a> Planner<'a> {
    fn new(engine: &'a RetainedState, arena: &'a LayoutNodeArena, counters: CounterWalk) -> Self {
        Self {
            engine,
            arena,
            tree: PlanTree::default(),
            events: Vec::new(),
            stale_subtrees: Vec::new(),
            answered_marks: Vec::new(),
            reports: Vec::new(),
            reached_unstyled_element: false,
            preceding_content: Vec::new(),
            preceding_content_start: 0,
            places_boxes_of_unbound_nodes: false,
            root_style: None,
            writing_suggestion_parent: None,
            layout_top_layer: false,
            has_svg_root: false,
            layout_svg_mask_or_clip_path: false,
            layout_svg_pattern: false,
            latest_resource_boxes: Default::default(),
            enclosing_ancestor_rows: &[],
            root_row: NodeSlotId::INVALID,
            inside_svg_content: false,
            fork_budget: None,
            dispatch: None,
            batch: None,
            jobs: Vec::new(),
            sibling_links_between_runs: Vec::new(),
            child_list_may_split: false,
            in_column_group_list: false,
            commit_lists: CommitLists::default(),
            owed_images: Vec::new(),
            splices: Vec::new(),
            stand_in_names: Vec::new(),
            own_pre_order_extent: 0,
            unit: None,
            unit_element: None,
            in_unit_list: false,
            unit_entry_count: 0,
            unit_entry: 0,
            last_kept_label: 0,
            fresh_element_entries: Vec::new(),
            last_kept_entry: None,
            kept_entries: Vec::new(),
            answered_own_marks: Vec::new(),
            counters,
            resolves_counters_after_commit: false,
            restarts_counters_at_next_owner: false,
            content: ContentPlan {
                notes_quote_steps: !engine.holds_no_records_affecting_generated_content(),
                ..ContentPlan::default()
            },
        }
    }

    /// Resolves the counters of `element`, the next child of the owner whose children the walk is among, which may
    /// change a counter itself where `may_change_counters`.
    fn place_element_counters(&mut self, element: StyleNodeID, may_change_counters: bool) -> PlacedOwner {
        let style = may_change_counters
            .then(|| self.engine.published_style_view(element, None))
            .flatten();
        self.counters.place_owner(
            self.engine,
            CounterOwner::element(element),
            style,
            &[],
            self.arena,
            self.arena.counters_sets(),
        )
    }

    /// Resolves the counters of the pseudo-element of kind `generated_for` of `element`, which gets a box, and those its
    /// content uses.
    fn place_pseudo_element_counters(
        &mut self,
        element: StyleNodeID,
        generated_for: u8,
        is_list_item_marker: bool,
    ) -> ContentCounters {
        if self.resolves_counters_after_commit {
            return ContentCounters::AfterCommit;
        }
        if !self.counters.resolves_counters() {
            return ContentCounters::Zero;
        }
        let style = self
            .engine
            .pseudo_published_style_view(element, pseudo_kind_of(generated_for));
        let mut uses = Vec::new();
        if let Some(style) = style {
            crate::layout::generated_content::for_each_counter_name_content_uses(style, is_list_item_marker, |name| {
                uses.push(name.to_vec());
            });
        }
        let owner = CounterOwner { element, generated_for };
        let placed =
            self.counters
                .place_owner(self.engine, owner, style, &uses, self.arena, self.arena.counters_sets());
        ContentCounters::Walk(placed.source())
    }

    /// How the counters of `element`, whose box the walk just placed, are resolved.
    fn element_counters(&mut self, element: StyleNodeID, may_change_counters: bool) -> OwnerCounters {
        let restarts = std::mem::take(&mut self.restarts_counters_at_next_owner);
        if self.resolves_counters_after_commit {
            return OwnerCounters::AfterCommit {
                in_tree_order: !restarts,
            };
        }
        OwnerCounters::Placed(self.place_element_counters(element, may_change_counters))
    }

    fn current_parent(&self) -> NodeSlotId {
        self.tree.current_parent()
    }

    /// Starts planning below the box `node` stands in for: the rows below it are numbered in pre-order below a name of
    /// their own.
    fn start_stand_in(&mut self) {
        let name = self.tree.claims().take_pre_order_roots(1);
        self.stand_in_names.push(name);
        self.tree.start_pre_order_name(name);
        self.own_pre_order_extent = 0;
    }

    /// How far the positions of the rows the walk numbered below the box of the current stand-in reach, once it planned
    /// them all, its jobs' aside.
    fn finish_stand_in(&mut self) -> u64 {
        self.own_pre_order_extent + self.tree.start_pre_order_name(u32::MAX)
    }

    /// Leaves the boxes of root `root` of job `job` to it here: the rows the walk makes from now on go after the job's.
    fn splice_in_job(&mut self, job: u32, root: u32) {
        self.content.note_quote_step(QuoteStep::Hole { job, root }, None);
        if !self.resolves_counters_after_commit {
            let starts_run = self.jobs[job as usize].roots[root as usize].children.is_some();
            self.counters.hole(job, root, starts_run);
        }
        let name = self.tree.claims().take_pre_order_roots(1);
        self.own_pre_order_extent += self.tree.start_pre_order_name(name);
        self.splices.push(Splice {
            job,
            root,
            stand_in: (self.stand_in_names.len() - 1) as u32,
            own_extent_before: self.own_pre_order_extent,
            name_after: name,
            at: self.output_lengths(),
        });
    }

    fn output_lengths(&self) -> OutputLengths {
        OutputLengths {
            events: self.events.len() as u32,
            stale_subtrees: self.stale_subtrees.len() as u32,
            reports: self.reports.len() as u32,
            owed_images: self.owed_images.len() as u32,
            rows_committed_one_by_one: self.tree.rows_committed_one_by_one.len() as u32,
            quote_steps: self.content.quote_steps.len() as u32,
        }
    }

    /// Whether the commit gives the row of `node`, just placed, what the lists do not one by one for its place in the
    /// tree: it is derived as the commit attaches it, or attached to the root of the build, which notes what it changes
    /// for the boxes above.
    fn placed_row_is_committed_one_by_one(&self, node: NodeSlotId) -> bool {
        let planned = self.tree.node(node);
        let parent = planned.parent();
        // NB: The parent of a row placed in a child list planned again may be an anonymous box the list had before.
        planned.has_fact(box_fact::DERIVED_ON_COMMIT) || parent == self.root_row || self.in_unit_list
    }

    /// Lists what the commit does for `node`, the principal box of `element` the walk just placed, with `record`, whose
    /// facts are `style_facts`: see [`CommitLists`].
    fn list_element_row(&mut self, node: NodeSlotId, element: StyleNodeID, record: u64, style_facts: u32) {
        use crate::layout::layout_node_arena::{
            OwedImageResources, planned_element_row_notes_its_style, planned_style_may_have_facts,
        };
        let planned = self.tree.node(node);
        let slot = node;
        let facts = planned.facts();
        if self.arena.holds_layout_tree_update_marks(element) {
            self.answered_marks.push(element);
        }
        let stamped_by_builder = facts & box_fact::STAMPED_BY_BUILDER != 0;
        self.owed_images.push((
            slot,
            OwedImageResources::StyleResources {
                owns_content_replacement_image: stamped_by_builder
                    && facts & box_fact::OWNS_CONTENT_REPLACEMENT_IMAGE != 0,
            },
        ));
        let mut one_by_one = self.placed_row_is_committed_one_by_one(node)
            || stamped_by_builder
            || facts & box_fact::HOLDS_HIGHLIGHT_STYLE != 0
            // NB: An element whose box is built again loses the pseudo-element boxes it held.
            || self.arena.has_bound_pseudo_element_rows(element);
        if !stamped_by_builder {
            let lists = &mut self.commit_lists;
            lists.bindings.push((element, slot));
            match lists.pins.last_mut() {
                Some((pinned, count)) if *pinned == record => *count += 1,
                _ => lists.pins.push((record, 1)),
            }
            lists.style_may_have_facts |= planned_style_may_have_facts(style_facts);
            one_by_one |= planned_element_row_notes_its_style(planned.row(), style_facts);
        }
        if one_by_one {
            self.commit_one_by_one(
                node,
                OneByOneRowSource::Element {
                    element,
                    record,
                    style_facts,
                },
            );
        }
    }

    /// Lists what the commit does for `node`, the box of `text` the walk just placed: see [`CommitLists`].
    fn list_text_row(&mut self, node: NodeSlotId, text: StyleNodeID) {
        if self.arena.holds_layout_tree_update_marks(text) {
            self.answered_marks.push(text);
        }
        self.commit_lists.bindings.push((text, node));
        self.commit_lists.text_rows.push(node);
        if self.placed_row_is_committed_one_by_one(node)
            || self.tree.node(node).has_fact(box_fact::PARENT_HOLDS_HIGHLIGHT_STYLE)
        {
            self.commit_one_by_one(node, OneByOneRowSource::Text(text));
        }
    }

    /// Lists `node`, a row the walk just placed, for the commit to give it what the lists do not one by one.
    fn commit_one_by_one(&mut self, node: NodeSlotId, source: OneByOneRowSource) {
        let facts = self.tree.node(node).facts();
        self.tree.rows_committed_one_by_one.push(RowCommittedOneByOne {
            slot: node,
            facts,
            source,
        });
    }

    /// Plans the boxes below `root`, the box just made for `identity`, the document or an element,
    /// whose whole subtree is to be built.
    pub(crate) fn plan_descendants(
        engine: &'a RetainedState,
        arena: &'a LayoutNodeArena,
        root: NodeSlotId,
        identity: StyleNodeID,
        is_document: bool,
        context: PlanContext<'a>,
    ) -> SubtreePlan {
        Self::plan_below(engine, arena, root, identity, is_document, context, None)
    }

    /// Plans the child list of `root`, the box of the element `identity`, or the viewport of the document `identity`
    /// for `is_document`, which keeps its box, again: the walk plans the list as [`Self::plan_descendants`] does, but a
    /// child whose box can stay as it is, which is attached in the list, holds no mark that asks for its box again, and
    /// comes after the boxes kept before it, stays: a row standing in for it is placed as its box would be, and nothing
    /// below it is planned.
    pub(crate) fn plan_unit(
        engine: &'a RetainedState,
        arena: &'a LayoutNodeArena,
        root: NodeSlotId,
        identity: StyleNodeID,
        is_document: bool,
        unit: UnitPlanning,
        context: PlanContext<'a>,
    ) -> SubtreePlan {
        Self::plan_below(engine, arena, root, identity, is_document, context, Some(unit))
    }

    // NB: Kept out of line so that profiles tell its time apart.
    #[inline(never)]
    fn plan_below(
        engine: &'a RetainedState,
        arena: &'a LayoutNodeArena,
        root: NodeSlotId,
        identity: StyleNodeID,
        is_document: bool,
        context: PlanContext<'a>,
        unit: Option<UnitPlanning>,
    ) -> SubtreePlan {
        let mut fork_budget = None;
        if crate::layout::parallel_layout::tree_build_forks_jobs(engine) {
            let planned_elements = planned_element_count(
                engine,
                arena,
                identity,
                is_document,
                unit.is_some(),
                unit.and_then(|unit| unit.window)
                    .map(|window| (window.start, window.end)),
            );
            let threads = crate::layout::parallel_layout::tree_build_thread_count(
                engine,
                planned_elements,
                arena.forces_parallel_layout(),
            );
            if threads > 1 {
                fork_budget = Some(ForkBudget {
                    jobs_left: std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(
                        threads * ForkBudget::JOBS_PER_THREAD,
                    )),
                    threads,
                    splits: if arena.forces_parallel_layout() {
                        ListSplits::FORCED
                    } else {
                        ListSplits::DEFAULT
                    },
                });
            }
        }
        let claims = super::ChunkClaims::new(arena.chunk_count(), context.free_slots);
        let shared = JobShared {
            claims: claims.clone(),
            fork_budget: fork_budget.clone(),
            writing_suggestion_parent: arena
                .writing_suggestion()
                .and_then(|suggestion| engine.text_parent(suggestion.text)),
            enclosing_ancestor_rows: context.enclosing_ancestor_rows,
            root_row: root,
            root_style: arena.style_view(root),
            handed_on: std::sync::Mutex::new(std::collections::VecDeque::new()),
            progress: Default::default(),
            counter_ids: Default::default(),
            removes_stale_counters_sets: !(is_document && unit.is_none()) && !arena.has_no_counters(),
            resolves_counters: !engine.holds_no_records_affecting_generated_content() || !arena.has_no_counters(),
        };
        let makes_jobs = fork_budget.is_some();
        let (mut plan, own_pre_order_extent) =
            crate::layout::parallel_layout::in_tree_build_job_scope(arena, engine, makes_jobs, |scope| {
                let dispatch = scope.map(|scope| JobDispatch {
                    scope,
                    engine,
                    arena,
                    shared: &shared,
                });
                let counters = CounterWalk::new(
                    shared.counter_ids.clone(),
                    shared.removes_stale_counters_sets,
                    shared.resolves_counters,
                );
                let mut planner = Planner::new(engine, arena, counters);
                planner.dispatch = dispatch.as_ref().map(|dispatch| dispatch as &dyn DispatchJob);
                planner.fork_budget = fork_budget;
                planner.unit = unit;
                if unit.is_some() {
                    planner.unit_element = Some(identity);
                }
                planner.tree = PlanTree::new(claims);
                planner.has_svg_root = context.has_svg_root;
                planner.enclosing_ancestor_rows = shared.enclosing_ancestor_rows;
                planner.root_row = root;
                planner.writing_suggestion_parent = shared.writing_suggestion_parent;
                planner.root_style = shared.root_style;
                let is_svg_row = |row: NodeSlotId| {
                    let kind = arena.data(row).kind.get();
                    node_facts::kind_is_svg_content(kind)
                };
                planner.inside_svg_content = context.has_svg_root
                    || is_svg_row(root)
                    || shared.enclosing_ancestor_rows.iter().any(|&row| is_svg_row(row));
                let data = arena.data(root);
                let kind = data.kind.get();
                let mut flags = data.flags.get();
                if unit.is_some_and(|unit| unit.window.is_none()) {
                    // NB: The child list planned again decides whether the box's children are inline, as it does for a new box.
                    flags &= !(NodeFlag::ChildrenAreInline as u32);
                }
                // NB: The viewport shows no style until the host installs the document's.
                let facts = arena.style_view(root).map_or(0, |style| {
                    box_facts_of_style(style) | item_context_facts(kind, flags, style.display())
                });
                planner.tree.push_stand_in(
                    root,
                    PlanNode {
                        style: data.style.get(),
                        ..PlanNode::new(kind, flags, facts)
                    },
                );
                let root_index = root;
                planner.tree.row_mut(root_index).ancestor_facts = data.ancestor_facts;
                planner
                    .tree
                    .note_stand_in_math_layout_kind(root_index, arena.math_layout_kind(root));
                planner.start_stand_in();
                if is_document {
                    planner.tree.open_parent(root_index);
                    // NB: The viewport's list is the document element's box and the top layer members' boxes.
                    planner.in_unit_list = unit.is_some();
                    planner.counters.start_root(&CounterFrame::default());
                    planner.plan_dom_children(identity);
                    // Elements in the top layer do not lay out normally based on their position in the document; instead they
                    // generate boxes as if they were siblings of the root element.
                    planner.layout_top_layer = true;
                    planner.resolves_counters_after_commit = true;
                    let mut index = 0;
                    while let Some(member) = engine.top_layer_element(index) {
                        index += 1;
                        if engine.element_adjustment_facts(member) & element_adjustment_fact::RENDERED_IN_TOP_LAYER == 0
                        {
                            continue;
                        }
                        if planner.has_unrendered_flat_tree_ancestor(member) {
                            planner
                                .stale_subtrees
                                .push((member, StaleSubtreeScope::InclusiveBoundedToRoot));
                            continue;
                        }
                        // NB: A member reads the counters of the owners before its place in the document.
                        planner.restarts_counters_at_next_owner = true;
                        planner.plan_node(member);
                    }
                    planner.restarts_counters_at_next_owner = false;
                    planner.resolves_counters_after_commit = false;
                    planner.counters.finish_root();
                    planner.layout_top_layer = false;
                    let is_unit_list = std::mem::take(&mut planner.in_unit_list);
                    planner.tree.close_parent();
                    planner.fix_up_tables_in_child_list(root_index, is_unit_list);
                } else {
                    let inputs = engine.element_layout_inputs(identity);
                    let summary = inputs.record.map(|record| record.summary());
                    let descendant_facts = DescendantFacts {
                        construction_facts: inputs.construction_facts,
                        content_visibility: summary.map_or(0, |summary| summary.content_visibility),
                        publishes_no_pseudo_element_records: engine.publishes_no_pseudo_element_records(identity),
                        may_change_counters: summary.is_none_or(|summary| summary.may_change_counters),
                        has_style_containment: planner
                            .root_style
                            .is_some_and(crate::painting::style_queries::has_style_containment),
                    };
                    let sets = arena.counters_sets();
                    planner
                        .counters
                        .start_root_after_retained(arena, engine, sets, CounterOwner::element(identity));
                    let placed = planner.place_element_counters(identity, descendant_facts.may_change_counters);
                    planner.plan_element_box_descendants(
                        identity,
                        root_index,
                        descendant_facts,
                        OwnerCounters::Placed(placed),
                    );
                    planner.counters.finish_root();
                }
                if unit.is_some() {
                    planner.answered_own_marks.push(identity);
                } else {
                    planner.answered_marks.push(identity);
                }
                let own_pre_order_extent = planner.finish_stand_in();
                planner.run_jobs_left(&shared);
                (planner.into_plan(), own_pre_order_extent)
            });
        plan.take_job_results(&shared);
        plan.replay_counters();
        plan.resolved_counters = Some(super::counter_walk::resolve_counters_of_build(
            &mut plan,
            is_document && unit.is_none(),
        ));
        plan.pre_order_extent = own_pre_order_extent
            + plan
                .splices
                .iter()
                .map(|splice| plan.jobs[splice.job as usize].roots[splice.root as usize].pre_order_size)
                .sum::<u64>();
        plan
    }

    fn into_plan(mut self) -> SubtreePlan {
        self.tree.refresh_facts_of_rows_committed_one_by_one();
        SubtreePlan {
            tree: self.tree,
            events: self.events,
            stale_subtrees: self.stale_subtrees,
            answered_marks: self.answered_marks,
            reports: self.reports,
            reached_unstyled_element: self.reached_unstyled_element,
            places_boxes_of_unbound_nodes: self.places_boxes_of_unbound_nodes,
            jobs: self.jobs,
            sibling_links_between_runs: self.sibling_links_between_runs,
            commit_lists: CommitLists {
                owed_images: std::sync::Arc::new(self.owed_images),
                ..self.commit_lists
            },
            splices: self.splices,
            stand_in_names: self.stand_in_names,
            pre_order_extent: 0,
            fresh_element_entries: self.fresh_element_entries,
            last_kept_entry: self.last_kept_entry,
            kept_entries: self.kept_entries,
            answered_own_marks: self.answered_own_marks,
            counters: self.counters.into_plan(),
            content: self.content,
            resolved_counters: None,
        }
    }

    /// Whether the walk may leave the descendants of `node`, an element's box it just placed, to a job: the box keeps
    /// the boxes below it, as a box that is not an inline box does, and nothing above it in SVG content or the top
    /// layer decides how they are built.
    fn may_defer_descendants_of(&self, element: StyleNodeID, node: NodeSlotId) -> bool {
        let kind = self.tree.node(node).kind();
        self.batch.is_some()
            && (self.engine.first_dom_child(element).is_some() || self.engine.shadow_root_of(element).is_some())
            && !self.inside_svg_content
            && !self.has_svg_root
            && !self.layout_svg_mask_or_clip_path
            && !self.layout_svg_pattern
            && !self.layout_top_layer
            && kind != NodeKind::InlineNode
            && !node_facts::kind_is_svg_content(kind)
    }

    /// Leaves the descendants of `node`, the box of `element`, to the job of the child list the walk is in, if it can
    /// make one, and answers whether it did.
    fn defer_descendants(
        &mut self,
        element: StyleNodeID,
        node: NodeSlotId,
        descendant_facts: DescendantFacts,
        counter_frame: Option<CounterFrame>,
    ) -> bool {
        let Some(mut batch) = self.batch else {
            return false;
        };
        let job = match batch.job {
            Some(job) if batch.roots_left > 0 => job,
            _ => {
                if !self.fork_budget.as_ref().is_some_and(ForkBudget::take_job) {
                    return false;
                }
                self.jobs.push(DeferredJob {
                    roots: Vec::new(),
                    plan: None,
                    slot: None,
                });
                batch.roots_left = batch.capacity;
                (self.jobs.len() - 1) as u32
            }
        };
        batch.job = Some(job);
        batch.roots_left -= 1;
        self.batch = Some(batch);
        let roots = &mut self.jobs[job as usize].roots;
        let root = roots.len() as u32;
        roots.push(DeferredRoot {
            node,
            children: None,
            element,
            descendant_facts,
            counter_frame,
            outputs: Default::default(),
            splices: 0..0,
            pre_order_size: 0,
        });
        self.splice_in_job(job, root);
        true
    }

    /// Gets the jobs the walk made from the `first` on ready to run, now that it got past their boxes: what each takes
    /// of the boxes it plans below is final. A thread of the pool takes each one where the walk hands its jobs on.
    fn seal_jobs_from(&mut self, first: usize) {
        for job in first..self.jobs.len() {
            if self.jobs[job].slot.is_some() {
                continue;
            }
            let roots = std::mem::take(&mut self.jobs[job].roots);
            let spec = JobSpec {
                roots: roots
                    .into_iter()
                    .map(|root| {
                        let node = self.tree.node(root.node);
                        let stand_in = PlanNode {
                            flags: node.flags(),
                            facts: node.facts(),
                            kind: node.kind(),
                            generated_for: node.generated_for(),
                            style: node.row().style.get(),
                        };
                        JobRoot {
                            stand_in,
                            ancestor_facts: node.ancestor_facts(),
                            math_layout_kind: self.tree.math_layout_kind_of(root.node),
                            root,
                        }
                    })
                    .collect(),
            };
            let slot = std::sync::Arc::new(JobSlot::new(spec));
            if let Some(dispatch) = self.dispatch {
                dispatch.dispatch(&slot);
            }
            self.jobs[job].slot = Some(slot);
        }
    }

    /// Runs the jobs of the walk that no other thread took, on this thread, once the walk is done: the newest first
    /// where the walk handed them on, as the threads of the pool take the oldest first.
    fn run_jobs_left(&mut self, shared: &JobShared<'_>) {
        self.seal_jobs_from(0);
        let slots: Vec<std::sync::Arc<JobSlot>> = self
            .jobs
            .iter()
            .map(|job| job.slot.clone().expect("every job is sealed"))
            .collect();
        let (engine, arena) = (self.engine, self.arena);
        if self.dispatch.is_some() || crate::layout::parallel_layout::tree_build_jobs_run_reversed() {
            slots.iter().rev().for_each(|slot| slot.run(engine, arena, shared));
        } else {
            slots.iter().for_each(|slot| slot.run(engine, arena, shared));
        }
        // NB: The thread that started the build is none of the pool's, so it cannot take work from the pool as it waits
        //     for the jobs it handed on, and the pool gets to the tasks of jobs this thread ran only once it has nothing
        //     else to do. It runs every job of the build no thread took yet instead, the oldest first, as a thread of
        //     the pool takes work from another. No scope or join of the pool lets a thread outside it run the pool's
        //     work as it waits, and a scope in which this thread only waits made large builds up to a tenth slower.
        if self.dispatch.is_some() && rayon::current_thread_index().is_none() {
            let next = || {
                shared
                    .handed_on
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .pop_front()
            };
            while crate::layout::parallel_layout::stack_has_room_for_forked_jobs(arena)
                && let Some(slot) = next()
            {
                slot.run(engine, arena, shared);
            }
        }
    }

    /// Plans the descendants of the boxes of a job, each below the row standing in for it, in the order the deferring
    /// walk reached them.
    fn run_job(
        engine: &'a RetainedState,
        arena: &'a LayoutNodeArena,
        shared: &JobShared<'a>,
        spec: JobSpec,
    ) -> (Vec<DeferredRoot>, SubtreePlan) {
        let makes_jobs = shared.fork_budget.is_some();
        let (mut roots, mut plan) =
            crate::layout::parallel_layout::in_tree_build_job_scope(arena, engine, makes_jobs, |scope| {
                let dispatch = scope.map(|scope| JobDispatch {
                    scope,
                    engine,
                    arena,
                    shared,
                });
                let counters = CounterWalk::new(
                    shared.counter_ids.clone(),
                    shared.removes_stale_counters_sets,
                    shared.resolves_counters,
                );
                let mut planner = Planner::new(engine, arena, counters);
                planner.dispatch = dispatch.as_ref().map(|dispatch| dispatch as &dyn DispatchJob);
                planner.tree = PlanTree::new(shared.claims.clone());
                planner.fork_budget = shared.fork_budget.clone();
                planner.writing_suggestion_parent = shared.writing_suggestion_parent;
                planner.enclosing_ancestor_rows = shared.enclosing_ancestor_rows;
                planner.root_row = shared.root_row;
                planner.root_style = shared.root_style;
                let mut roots = Vec::with_capacity(spec.roots.len());
                for JobRoot {
                    mut root,
                    stand_in,
                    ancestor_facts,
                    math_layout_kind,
                } in spec.roots
                {
                    let node = root.node;
                    planner.tree.push_stand_in(node, stand_in);
                    planner.tree.row_mut(node).ancestor_facts = ancestor_facts;
                    planner.tree.note_stand_in_math_layout_kind(node, math_layout_kind);
                    planner.start_stand_in();
                    let (outputs, splices) = (planner.output_lengths(), planner.splices.len());
                    planner.resolves_counters_after_commit = root.counter_frame.is_none();
                    if let Some(frame) = &root.counter_frame {
                        planner.counters.start_root(frame);
                    }
                    if let Some(children) = &root.children {
                        planner.tree.open_parent(node);
                        planner.plan_children(children);
                        planner.tree.close_parent();
                        // NB: Each run starts with a block that is no part of a table, so no sequence table fixup
                        //     wraps, and no white space it removes, spans two runs.
                        planner.fix_up_tables_in_child_list(node, false);
                        planner.preceding_content.clear();
                    } else {
                        let counters = match root.counter_frame {
                            Some(_) => OwnerCounters::Entered,
                            None => OwnerCounters::AfterCommit { in_tree_order: true },
                        };
                        planner.plan_element_box_descendants(root.element, node, root.descendant_facts, counters);
                    }
                    if root.counter_frame.is_some() {
                        planner.counters.finish_root();
                    }
                    root.outputs = outputs..planner.output_lengths();
                    root.splices = splices..planner.splices.len();
                    // NB: The rows of the jobs are added once they ran.
                    root.pre_order_size = planner.finish_stand_in();
                    roots.push(root);
                }
                planner.run_jobs_left(shared);
                (roots, planner.into_plan())
            });
        plan.take_job_results(shared);
        plan.replay_counters();
        plan.count_rows_of_jobs(&mut roots);
        // NB: The jobs linked their rows below the boxes of the walk they ran for.
        plan.tree.hand_on_open_chunk();
        (roots, plan)
    }

    fn plan_dom_children(&mut self, parent: StyleNodeID) {
        let may_split = std::mem::take(&mut self.child_list_may_split);
        let enclosing_batch = self.batch.take();
        let first_job = self.jobs.len();
        let window = self
            .unit
            .and_then(|unit| unit.window)
            .filter(|_| self.in_unit_list && Some(parent) == self.unit_element);
        // NB: A window in the run of inline content the list ends with plans the run in an anonymous box, as a walk does
        //     after the block the run follows.
        if let Some(window) = window
            && !window.wrapper.is_invalid()
        {
            let wrapper = self
                .tree
                .create_anonymous_wrapper(self.current_parent(), NodeSlotId::INVALID);
            self.tree.set_flag(wrapper, NodeFlag::ChildrenAreInline, true);
        }
        // NB: The children before the window keep their boxes, so the walk starts after what they did to the counters.
        if let Some(start) = window.and_then(|window| window.start)
            && !self.resolves_counters_after_commit
        {
            self.counters
                .start_children_after_retained(crate::layout::counters::nearest_rendered_preceding_child(
                    self.arena,
                    self.engine,
                    parent,
                    start,
                ));
        }
        let first_child = window
            .and_then(|window| window.start)
            .or_else(|| self.engine.first_dom_child(parent));
        // NB: A window that ends at a child the list keeps plans no child after it.
        let last_child = window.and_then(|window| window.end);
        let engine = self.engine;
        let next_child = move |child: StyleNodeID| {
            (Some(child) != last_child)
                .then(|| engine.next_sibling_in_dom_order(child))
                .flatten()
        };
        let mut child_count = 0;
        // OPTIMIZATION: The walk goes along a long list once, into the list its runs or windows are cut from.
        let mut long_list = Vec::new();
        if let Some(budget) = &self.fork_budget {
            let siblings = || std::iter::successors(first_child, |&child| next_child(child));
            let children_of_a_long_list = budget.splits.children_of_a_long_list();
            child_count = siblings().take(children_of_a_long_list).count();
            if child_count == children_of_a_long_list {
                long_list = siblings().collect::<Vec<_>>();
                child_count = long_list.len();
            }
            if child_count >= budget.splits.children_to_split {
                let capacity = child_count.div_ceil(budget.threads * ForkBudget::CHILD_LIST_JOBS_PER_THREAD);
                self.batch = Some(ChildListBatch {
                    job: None,
                    roots_left: 0,
                    capacity: capacity.clamp(budget.splits.minimum_roots_per_job, ForkBudget::MAXIMUM_ROOTS_PER_JOB),
                });
            }
        }
        if may_split
            && self
                .fork_budget
                .as_ref()
                .is_some_and(|budget| child_count >= budget.splits.children_to_plan_in_runs)
            && self.plan_dom_children_in_runs(parent, &long_list)
        {
            // NB: The runs after the first are jobs.
        } else if !long_list.is_empty()
            && !self.layout_svg_mask_or_clip_path
            && !self.layout_svg_pattern
            && !self.in_unit_list
        {
            self.plan_prepared_dom_children(&long_list);
        } else {
            let mut child = first_child;
            let mut read_ahead = ChildReadAhead::new(engine, child);
            while let Some(current) = child {
                read_ahead.advance(engine);
                self.plan_node(current);
                child = next_child(current);
            }
        }
        // NB: The jobs of the list run once the walk is past it, as its boxes are then final.
        self.seal_jobs_from(first_job);
        self.batch = enclosing_batch;
    }

    /// Plans `children`, the children of the box the walk is in, as [`Self::plan_dom_children`] plans a child list.
    fn plan_children(&mut self, children: &[StyleNodeID]) {
        let enclosing_batch = self.batch.take();
        let first_job = self.jobs.len();
        if let Some(budget) = &self.fork_budget
            && children.len() >= budget.splits.children_to_split
        {
            let capacity = children
                .len()
                .div_ceil(budget.threads * ForkBudget::CHILD_LIST_JOBS_PER_THREAD);
            self.batch = Some(ChildListBatch {
                job: None,
                roots_left: 0,
                capacity: capacity.clamp(budget.splits.minimum_roots_per_job, ForkBudget::MAXIMUM_ROOTS_PER_JOB),
            });
        }
        // OPTIMIZATION: The children are siblings in DOM order: the walk reads ahead along them as along a child list.
        let mut read_ahead = ChildReadAhead::new(self.engine, children.first().copied());
        for &child in children {
            read_ahead.advance(self.engine);
            self.plan_node(child);
        }
        self.seal_jobs_from(first_job);
        self.batch = enclosing_batch;
    }

    /// Plans the children of `parent`, a long child list of the box the walk is in, in runs whose boxes are placed
    /// alike wherever the run starts: each run but the first starts with an element whose box is block-level and in
    /// flow, after another such box, so that it goes last among the box's children, and every box after it goes where
    /// it would after that box. The walk plans the first run, which ends after the first such box, and leaves each other
    /// one to a job, whose boxes go after those of the runs before. Answers whether it did, or left the list to the
    /// caller to plan, where no such runs are found or no jobs are left.
    fn plan_dom_children_in_runs(&mut self, parent: StyleNodeID, children: &[StyleNodeID]) -> bool {
        let Some(budget) = self.fork_budget.clone() else {
            return false;
        };
        let run_length = children
            .len()
            .div_ceil(budget.threads * ForkBudget::CHILD_LIST_JOBS_PER_THREAD)
            .max(budget.splits.children_per_run);
        let starts_run = |index: usize| self.starts_independent_run(children[index]);
        let Some(first_block) = (0..children.len().min(RUN_START_SEARCH_LENGTH)).find(|&index| starts_run(index))
        else {
            return false;
        };
        let mut run_starts = Vec::new();
        // NB: The jobs plan the list while the walk waits for them, so the walk keeps no more of it than it has to.
        let mut target = first_block + 1;
        while target < children.len() {
            let search_end = children.len().min(target + RUN_START_SEARCH_LENGTH);
            let Some(start) = (target..search_end).find(|&index| starts_run(index)) else {
                target = search_end;
                continue;
            };
            run_starts.push(start);
            target = start + run_length;
        }
        if run_starts.is_empty() || !budget.take_jobs(run_starts.len()) {
            return false;
        }
        let node = self.current_parent();
        self.plan_children(&children[..run_starts[0]]);
        let mut counter_frames = self.counter_frames_of_runs(children, &run_starts).into_iter();
        for (run, &start) in run_starts.iter().enumerate() {
            let end = run_starts.get(run + 1).copied().unwrap_or(children.len());
            self.jobs.push(DeferredJob {
                roots: vec![DeferredRoot {
                    node,
                    children: Some(children[start..end].to_vec()),
                    counter_frame: counter_frames.next().expect("every run has a frame"),
                    element: parent,
                    descendant_facts: DescendantFacts {
                        construction_facts: 0,
                        content_visibility: 0,
                        publishes_no_pseudo_element_records: true,
                        may_change_counters: false,
                        has_style_containment: false,
                    },
                    outputs: Default::default(),
                    splices: 0..0,
                    pre_order_size: 0,
                }],
                plan: None,
                slot: None,
            });
            let job = (self.jobs.len() - 1) as u32;
            self.splice_in_job(job, 0);
        }
        true
    }

    /// What the walk of each run of a long child list split at `run_starts` starts from as far as counters go, once the
    /// walk planned the children before the first: the counters the children of the runs before it instantiated, whose
    /// own steps a look at the children that may change a counter, in parallel, finds.
    fn counter_frames_of_runs(&self, children: &[StyleNodeID], run_starts: &[usize]) -> Vec<Option<CounterFrame>> {
        if self.resolves_counters_after_commit {
            return vec![None; run_starts.len()];
        }
        let (engine, arena) = (self.engine, self.arena);
        // OPTIMIZATION: Where no owner holds a counter and no record changes one, there is nothing to look for.
        if !self.counters.resolves_counters() {
            return vec![Some(CounterFrame::default()); run_starts.len()];
        }
        // NB: The children of the last run change nothing a run after it starts from.
        let runs: Vec<&[StyleNodeID]> = (0..run_starts.len())
            .map(|run| match run_starts.get(run + 1) {
                Some(&end) if !engine.holds_no_records_affecting_generated_content() => &children[run_starts[run]..end],
                _ => &[],
            })
            .collect();
        // OPTIMIZATION: No record changes a counter, so no child does: the runs start from what the walk holds.
        if runs.iter().all(|run| run.is_empty()) {
            return self
                .counters
                .frames_of_runs(engine, runs.iter().map(|_| Vec::new()))
                .into_iter()
                .map(Some)
                .collect();
        }
        let owners = crate::layout::parallel_layout::run_tree_build_jobs(arena, engine, runs, |run| {
            run.iter()
                .copied()
                .filter(|&child| child_list_child_may_change_counters(engine, arena, child))
                .collect::<Vec<_>>()
        });
        let with_styles = owners.into_iter().map(|owners| {
            owners
                .into_iter()
                .filter_map(|child| Some((child, engine.published_style_view(child, None)?)))
                .collect::<Vec<_>>()
        });
        self.counters
            .frames_of_runs(engine, with_styles)
            .into_iter()
            .map(Some)
            .collect()
    }

    /// Whether `child` makes a box that is block-level and in flow, which a run of a child list may start with (see
    /// [`Self::plan_dom_children_in_runs`]).
    fn starts_independent_run(&self, child: StyleNodeID) -> bool {
        if child.text_index().is_some() {
            return false;
        }
        let prepared = prepare_element(self.engine, self.arena, child, false, false);
        if prepared.type_facts
            & (element_adjustment_fact::RENDERED_IN_TOP_LAYER
                | element_adjustment_fact::IS_SVG_CONTAINER
                | element_adjustment_fact::REQUIRES_SVG_CONTAINER
                | element_adjustment_fact::IS_SVG_FOREIGN_OBJECT_ELEMENT)
            != 0
        {
            return false;
        }
        let Some(style) = prepared.style else {
            return false;
        };
        if style.display.is_none() || style.display.is_contents() {
            return false;
        }
        let Some(principal_box) = style.principal_box else {
            return false;
        };
        principal_box.box_facts
            & (box_fact::INLINE_OUTSIDE
                | box_fact::FLOATING
                | box_fact::ABSOLUTELY_POSITIONED
                | box_fact::TABLE_NON_ROOT
                | box_fact::TABLE_NON_ROOT_BEFORE_BLOCKIFICATION)
            == 0
            && !node_facts::kind_is_svg_content(principal_box.kind)
    }

    /// Plans `children`, a long child list, reading what the walk needs of each element among them ahead of placing
    /// them, in windows of children read in parallel tasks.
    fn plan_prepared_dom_children(&mut self, children: &[StyleNodeID]) {
        let engine = self.engine;
        let arena = self.arena;
        let splits = self
            .fork_budget
            .as_ref()
            .map_or(ListSplits::DEFAULT, |budget| budget.splits);
        for window in children.chunks(splits.prepared_children_per_window) {
            let tasks: Vec<&[StyleNodeID]> = window.chunks(splits.prepared_children_per_task).collect();
            let prepared = crate::layout::parallel_layout::run_tree_build_jobs(arena, engine, tasks, |task| {
                task.iter()
                    .map(|&child| {
                        (child.text_index().is_none()).then(|| prepare_element(engine, arena, child, false, false))
                    })
                    .collect::<Vec<_>>()
            });
            let mut prepared = prepared.into_iter().flatten();
            for &child in window {
                let prepared = prepared.next().expect("every child of the window was read");
                match prepared {
                    Some(prepared) => self.plan_element(child, Some(prepared)),
                    None => self.plan_text(child),
                }
            }
        }
    }

    /// Plans an element or text node and its subtree.
    fn plan_node(&mut self, identity: StyleNodeID) {
        if self.in_unit_list {
            self.unit_entry = self.unit_entry_count;
            self.unit_entry_count += 1;
        }
        if identity.text_index().is_some() {
            return self.plan_text(identity);
        }
        if self.in_unit_list {
            if self.plan_kept_element(identity) {
                return;
            }
            self.fresh_element_entries.push((self.unit_entry, identity));
        }
        self.plan_element(identity, None);
    }

    /// The row of `node`, an entry of the child list the walk plans again, that the walk can keep, and the top row of
    /// its entry: the row of its table wrapper for a table, or of the anonymous inline box a text renders in below a
    /// `display: contents` element. The row is attached in the list, possibly in an anonymous box of it, comes after
    /// every row the walk kept before, and no mark asks for its box again.
    fn keepable_row(&self, node: StyleNodeID) -> Option<(NodeSlotId, NodeSlotId)> {
        let unit = self.unit?;
        if self.unit_entry >= unit.fresh_from_entry {
            return None;
        }
        let arena = self.arena;
        let row = arena.bound_row(node);
        if row.is_invalid() {
            return None;
        }
        let marks = arena.layout_tree_update_marks();
        if marks.needs(node) && marks.reuse_reasons(node) == 0 {
            return None;
        }
        let entry_top = arena.list_entry_top_row(row);
        if !arena.is_in_child_list_of(entry_top, self.root_row) {
            return None;
        }
        if arena.node_pre_order_label(entry_top) <= self.last_kept_label {
            return None;
        }
        Some((row, entry_top))
    }

    /// Plans `element`, an entry of the child list the walk plans again, as the box it has, if the walk can keep it
    /// (see [`Self::keepable_row`]), and answers whether it did.
    fn plan_kept_element(&mut self, element: StyleNodeID) -> bool {
        // NB: A top layer member's box is not in the list where the member is in the document, and the walk skips it
        //     there as it always does. It is in the viewport's list, after the box of its ::backdrop.
        let is_top_layer_member =
            self.engine.element_adjustment_facts(element) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0;
        if is_top_layer_member && !self.layout_top_layer {
            return false;
        }
        let Some((row, entry_top)) = self.keepable_row(element) else {
            return false;
        };
        let backdrop = if is_top_layer_member {
            match self.keepable_backdrop_row(element, entry_top) {
                Some(backdrop) => backdrop,
                None => return false,
            }
        } else {
            None
        };
        let arena = self.arena;
        let data = arena.data(row);
        let (kind, flags) = (data.kind.get(), data.flags.get());
        let marks = arena.layout_tree_update_marks();
        let is_restyled =
            marks.needs(element) && marks.reuse_reasons(element) & layout_tree_update_reuse_reason::RESTYLE != 0;
        if is_restyled && !self.restyled_box_may_stay(element, row, entry_top) {
            return false;
        }
        // NB: The builder adjusts the display of a fieldset and of a replaced element, which their styles do not show.
        let display_is_adjusted = kind == NodeKind::FieldSetBox || flags & NodeFlag::IsReplacedElement as u32 != 0;
        let mut facts = if entry_top == row && !display_is_adjusted && !is_restyled {
            if self.unit.is_some_and(|unit| unit.verifies_kept_boxes) {
                self.verify_kept_element(element, row);
            }
            let style = arena.style_view(row).expect("an element's box has a style");
            box_facts_of_style(style) | item_context_facts(kind, flags, style.display())
        } else {
            // NB: The wrapper of a table took over the table's outer properties, which its box shows no more, so the
            //     facts of a kept table, as those of a box whose display the builder adjusts, are what a fresh build
            //     decides for it. A restyled box shows its old style until its own list is planned again.
            let prepared = prepare_element(
                self.engine,
                arena,
                element,
                self.layout_svg_mask_or_clip_path,
                self.layout_svg_pattern,
            );
            let Some(style) = prepared.style else {
                return false;
            };
            if fresh_box_shape(&style).is_none_or(|shape| Some(shape) != row_box_shape(arena, row)) {
                return false;
            }
            let principal_box = style.principal_box.expect("a box shape is that of a principal box");
            if entry_top == row {
                principal_box.box_facts
            } else {
                // NB: Table fixup made the wrapper of the table the list keeps already.
                principal_box.box_facts & !(box_fact::TABLE_INSIDE | box_fact::TABULAR_CONTAINER)
            }
        };
        let top = arena.data(entry_top);
        if entry_top != row {
            facts &= !box_fact::STAMPED_BY_BUILDER;
        }
        let node = PlanNode {
            style: top.style.get(),
            generated_for: top.generated_for.get(),
            ..PlanNode::new(top.kind.get(), top.flags.get(), facts)
        };
        if let Some((backdrop, backdrop_node)) = backdrop {
            let parent = self.current_parent();
            self.tree.push_stand_in(backdrop, backdrop_node);
            self.place_pseudo_element_box(parent, backdrop, false, ChildPosition::Append);
            self.last_kept_label = self.arena.node_pre_order_label(backdrop);
        }
        let is_svg_box = node_facts::kind_is_svg_box(kind);
        self.place_kept_row(entry_top, node, is_svg_box);
        self.preceding_content.push(PrecedingContent::Box(entry_top));
        if !self.resolves_counters_after_commit {
            self.counters.keep_element(element);
        }
        self.content.note_quote_step(QuoteStep::KeptElement(element), None);
        true
    }

    /// The box of the `::backdrop` of `member`, a top layer member whose box `entry_top` the walk keeps in the viewport's
    /// list, with what it stands in as, where the walk can keep it too: a fresh build makes a box of the same kind for
    /// it, and it goes just before the member's box. `Some(None)` where neither a fresh build nor the list has one.
    #[allow(clippy::option_option)]
    fn keepable_backdrop_row(
        &self,
        member: StyleNodeID,
        entry_top: NodeSlotId,
    ) -> Option<Option<(NodeSlotId, PlanNode)>> {
        let arena = self.arena;
        let row = arena.bound_pseudo_element_row(member, GENERATED_FOR_BACKDROP);
        let fresh = self.pseudo_element_box_shape(member, GENERATED_FOR_BACKDROP, self.current_parent());
        let Some(shape) = fresh else {
            return row.is_invalid().then_some(None);
        };
        if row.is_invalid() || arena.data(entry_top).previous_sibling.get() != row {
            return None;
        }
        let data = arena.data(row);
        if data.kind.get() != shape.kind
            || (data.flags.get() ^ shape.flags) & NodeFlag::IsPseudoElementPrincipalBox as u32 != 0
        {
            return None;
        }
        if arena.node_pre_order_label(row) <= self.last_kept_label {
            return None;
        }
        let mut node = PlanNode::new(data.kind.get(), data.flags.get(), shape.box_facts);
        node.generated_for = data.generated_for.get();
        node.style = data.style.get();
        Some(Some((row, node)))
    }

    /// Places `entry_top`, the top row of an entry the walk keeps, standing in as `node`, in the list as its box would be
    /// placed: SVG boxes are appended.
    fn place_kept_row(&mut self, entry_top: NodeSlotId, node: PlanNode, appends: bool) {
        let is_inline_outside = node.facts & box_fact::INLINE_OUTSIDE != 0;
        self.tree.push_stand_in(entry_top, node);
        let parent = self.current_parent();
        if appends {
            self.tree.attach(parent, entry_top, NodeSlotId::INVALID);
        } else {
            self.place(parent, entry_top, is_inline_outside, ChildPosition::Append);
        }
        self.note_kept_row(entry_top);
    }

    /// Notes that the walk kept the row `entry_top`: the owners of counters below it are not resolved again.
    fn note_kept_row(&mut self, entry_top: NodeSlotId) {
        self.last_kept_label = self.arena.node_pre_order_label(entry_top);
        self.last_kept_entry = Some(self.unit_entry);
        self.kept_entries.push((self.unit_entry, entry_top));
    }

    /// Asserts that the box `row` of `element`, which the walk keeps as no mark asks for it, has the shape a fresh build
    /// gives the element, and shows the element's record where the build stamped it. A kept box that does not is one
    /// whose change no mark told the build of.
    fn verify_kept_element(&self, element: StyleNodeID, row: NodeSlotId) {
        assert!(
            !self.arena.node_style_record_is_stamped(row)
                || self
                    .arena
                    .element_box_style_record(Some(element))
                    .map(|(record, _)| record)
                    == Some(self.arena.node_style_record(row)),
            "the kept layout box of {element:?} shows a record its element no longer has"
        );
        let fresh = prepare_element(
            self.engine,
            self.arena,
            element,
            self.layout_svg_mask_or_clip_path,
            self.layout_svg_pattern,
        );
        assert_eq!(
            fresh.style.as_ref().and_then(fresh_box_shape),
            row_box_shape(self.arena, row),
            "the kept layout box of {element:?} is not the box a fresh build makes"
        );
    }

    /// Whether the box `row` of `element`, whose style changed in a way its box is built again for (see
    /// [`layout_tree_update_reuse_reason::RESTYLE`]), may stay where a fresh build gives it a box of the same shape: its
    /// change moves no counter, quote or list item count, and the box still shows the record the build stamped it with,
    /// which its shape is read from. Its own list and pseudo-elements are planned again where it stays, which installs
    /// its new record. A table, a fieldset, a replaced element, an SVG element, a top layer member, a shadow host and
    /// the boxes of a shadow tree the user agent made are built again, as their boxes follow more than their styles.
    fn restyled_box_may_stay(&self, element: StyleNodeID, row: NodeSlotId, entry_top: NodeSlotId) -> bool {
        let (engine, arena) = (self.engine, self.arena);
        let marks = arena.layout_tree_update_marks();
        let data = arena.data(row);
        entry_top == row
            && !marks.generated_content_changed(element)
            && !marks.list_item_changed(element)
            && arena.node_style_record_is_stamped(row)
            && engine.shadow_root_of(element).is_none()
            && engine.element_construction_facts(element) & element_construction_fact::IS_IN_USER_AGENT_SHADOW_TREE == 0
            && engine.element_adjustment_facts(element)
                & (element_adjustment_fact::IS_SVG_ELEMENT | element_adjustment_fact::RENDERED_IN_TOP_LAYER)
                == 0
            && data.kind.get() != NodeKind::FieldSetBox
            && data.flags.get() & NodeFlag::IsReplacedElement as u32 == 0
    }

    fn plan_element(&mut self, element: StyleNodeID, prepared: Option<PreparedElement>) {
        let prepared = prepared.unwrap_or_else(|| {
            prepare_element(
                self.engine,
                self.arena,
                element,
                self.layout_svg_mask_or_clip_path,
                self.layout_svg_pattern,
            )
        });
        let type_facts = prepared.type_facts;
        let is_top_layer_member = type_facts & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0;
        if is_top_layer_member && !self.layout_top_layer {
            // A top layer member is skipped at its place in the DOM, and built in the viewport's list.
            return;
        }
        // The descendants of a top layer member lay out as usual.
        let is_active_top_layer_member = is_top_layer_member && self.layout_top_layer;
        let enclosing_layout_top_layer = self.layout_top_layer;
        if is_active_top_layer_member {
            self.layout_top_layer = false;
        }
        let has = |fact: u32| type_facts & fact != 0;
        let requires_svg_container = has(element_adjustment_fact::REQUIRES_SVG_CONTAINER);
        let enclosing_has_svg_root = self.has_svg_root;
        let enclosing_inside_svg_content = self.inside_svg_content;
        if has(element_adjustment_fact::IS_SVG_CONTAINER) {
            self.has_svg_root = true;
            self.inside_svg_content = true;
            self.plan_element_in_layout(element, &prepared, is_active_top_layer_member);
        } else if requires_svg_container && !self.has_svg_root {
            // An SVG element outside of an `<svg>` gets no box.
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
        } else if has(element_adjustment_fact::IS_SVG_FOREIGN_OBJECT_ELEMENT) {
            self.has_svg_root = false;
            self.plan_element_in_layout(element, &prepared, is_active_top_layer_member);
        } else if !requires_svg_container && self.has_svg_root {
            // HTML inside SVG outside a `<foreignObject>` gets no box.
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
        } else {
            self.plan_element_in_layout(element, &prepared, is_active_top_layer_member);
        }
        self.has_svg_root = enclosing_has_svg_root;
        self.inside_svg_content = enclosing_inside_svg_content;
        self.layout_top_layer = enclosing_layout_top_layer;
    }

    fn plan_element_in_layout(
        &mut self,
        element: StyleNodeID,
        prepared: &PreparedElement,
        is_active_top_layer_member: bool,
    ) {
        let type_facts = prepared.type_facts;
        let Some(prepared_style) = prepared.style else {
            // Nothing published a style for the element, so it gets no box in this build: the document styles it once
            // the build is over, and builds its box in the next one.
            self.reports.push(FfiCommitMessage::new(
                element.raw(),
                FfiCommitMessageKind::UnstyledElementReached,
            ));
            self.reached_unstyled_element = true;
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
            return;
        };
        let PreparedStyle {
            record,
            display,
            content_visibility,
            may_change_counters,
            box_kind,
            layout_kind,
            principal_box,
        } = prepared_style;
        // https://html.spec.whatwg.org/multipage/rendering.html#frames-and-framesets
        // A frameset's grid holds only the frame and frameset elements that are its children.
        let frameset_refuses_element = self.tree.node(self.current_parent()).kind() == NodeKind::FrameSetBox
            && !matches!(box_kind, ElementBoxKind::Frame | ElementBoxKind::FrameSet);
        if display.is_none() || frameset_refuses_element {
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
            return;
        }
        if display.is_contents() {
            return self.plan_display_contents(element);
        }
        match layout_kind {
            // Only direct mask and clip-path uses inherit this construction mode.
            ElementLayoutKind::SvgMask | ElementLayoutKind::SvgClipPath => {
                self.layout_svg_mask_or_clip_path = false;
            }
            // Only the directly referenced pattern inherits this construction mode.
            ElementLayoutKind::SvgPattern => self.layout_svg_pattern = false,
            ElementLayoutKind::ContentReplacement | ElementLayoutKind::Normal => {}
        }
        let Some(PreparedBox {
            kind,
            construction_facts,
            publishes_no_pseudo_element_records,
            has_style_containment,
            flags,
            box_facts,
            payloads,
            style_facts,
            is_unbound,
            stamped_row,
        }) = principal_box
        else {
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
            return;
        };
        // NB: See plan_element_box_descendants(): a table-column-group keeps only its table-column children.
        if self.in_column_group_list && !(display.is_table_column() && box_facts & box_fact::TABLE_NON_ROOT != 0) {
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
            return;
        }
        self.places_boxes_of_unbound_nodes |= is_unbound;
        let node = self.tree.push(PlanNode {
            style: Some(payloads),
            ..PlanNode::new(kind, flags, box_facts)
        });
        if let Some(stamped_row) = stamped_row {
            self.stamp_element_row(node, flags, element, record, payloads, stamped_row);
        }
        let math_layout_kind = MathLayoutKind::from_construction_facts(construction_facts);
        if math_layout_kind != MathLayoutKind::NotMathML && stamped_row.is_some() {
            self.tree.math_layout_kinds.insert(node, math_layout_kind);
        }

        let parent = self.current_parent();
        let is_inline_outside = box_facts & box_fact::INLINE_OUTSIDE != 0;
        if type_facts
            & (element_adjustment_fact::IS_SVG_MASK_ELEMENT
                | element_adjustment_fact::IS_SVG_CLIP_PATH_ELEMENT
                | element_adjustment_fact::IS_SVG_PATTERN_ELEMENT)
            != 0
        {
            self.latest_resource_boxes.insert(element, node);
        }
        // A backdrop is a sibling of its originating top-layer element, placed before it.
        if is_active_top_layer_member {
            self.plan_pseudo_element(element, GENERATED_FOR_BACKDROP, parent, ChildPosition::Append, None);
        }
        // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
        // 3. Generate missing parents:
        //     3. An anonymous table-wrapper box must be generated around each table-root.
        // NB: The wrapper goes where the table would go, and the table goes in it, so the boxes below the table are
        //     planned below the wrapper from the start. Table fixup makes no wrapper inside SVG content.
        let table_wrapper = (box_facts & box_fact::TABLE_INSIDE != 0
            && !node_facts::kind_is_svg_box(kind)
            && !self.tree.node(parent).is_svg_content())
        .then(|| self.tree.push_table_wrapper(node));
        // SVG layout ignores the inline/block distinction, so an SVG box is simply appended.
        if node_facts::kind_is_svg_box(kind) {
            self.tree.attach(parent, node, NodeSlotId::INVALID);
        } else if let Some(wrapper) = table_wrapper {
            self.place(parent, wrapper, is_inline_outside, ChildPosition::Append);
            // NB: The table is the box that was lifted out of an inline box, as table fixup wraps it once it is placed.
            if let Some(lifted) = self.tree.lifted_out_of_inline_boxes.last_mut()
                && lifted.0 == wrapper
            {
                lifted.0 = node;
            }
            self.tree.attach(wrapper, node, NodeSlotId::INVALID);
        } else {
            self.place(parent, node, is_inline_outside, ChildPosition::Append);
        }
        self.list_element_row(node, element, record, style_facts);
        if let Some(wrapper) = table_wrapper {
            self.tree.note_table_wrapper(wrapper, node);
        }

        let descendant_facts = DescendantFacts {
            construction_facts,
            content_visibility,
            publishes_no_pseudo_element_records,
            may_change_counters,
            has_style_containment,
        };
        let counters = self.element_counters(element, may_change_counters);
        let counter_frame = match &counters {
            OwnerCounters::Placed(placed) => Some(placed.frame()),
            OwnerCounters::AfterCommit { in_tree_order } => {
                debug_assert!(*in_tree_order || !self.may_defer_descendants_of(element, node));
                None
            }
            OwnerCounters::Entered => unreachable!("only a job's walk starts below an element"),
        };
        if !self.may_defer_descendants_of(element, node)
            || !self.defer_descendants(element, node, descendant_facts, counter_frame)
        {
            // NB: The child lists below a box planned here are its own, which no job of this one's takes.
            let enclosing_batch = self.batch.take();
            self.plan_element_box_descendants(element, node, descendant_facts, counters);
            self.batch = enclosing_batch;
        }
        self.preceding_content.push(PrecedingContent::Box(node));
    }

    /// Writes the row of `node`, the principal box of `element`, as stamping it with `record`, whose groups are
    /// `payloads`, writes a row: its flags, its style, the paint facts and table spans its element published, and what
    /// the arena keeps beside it.
    fn stamp_element_row(
        &mut self,
        node: NodeSlotId,
        flags: u32,
        element: StyleNodeID,
        record: u64,
        payloads: crate::layout::node_data::LayoutStyleRef,
        stamped_row: StampedRow,
    ) {
        let flags = flags | stamped_row.flags;
        let row = self.tree.row_mut(node);
        *row.flags.get_mut() = flags;
        *row.style.get_mut() = Some(payloads);
        *row.dom_paint_facts.get_mut() = stamped_row.dom_paint_facts;
        row.table_column_span = stamped_row.spans.column_span;
        row.table_row_span = stamped_row.spans.row_span;
        self.tree.set_columns(
            node,
            super::RowColumns {
                style_record: record,
                style_node: Some(element),
                unique_node_id: stamped_row.unique_node_id,
                ..Default::default()
            },
        );
    }

    fn plan_element_box_descendants(
        &mut self,
        element: StyleNodeID,
        node: NodeSlotId,
        descendant_facts: DescendantFacts,
        counters: OwnerCounters,
    ) {
        let DescendantFacts {
            construction_facts,
            content_visibility,
            publishes_no_pseudo_element_records,
            may_change_counters,
            has_style_containment,
        } = descendant_facts;
        let engine = self.engine;
        // NB: Only the child list of the box whose list the walk plans again keeps boxes, not those below it.
        let enclosing_in_unit_list =
            std::mem::replace(&mut self.in_unit_list, self.unit.is_some() && node == self.root_row);
        let has_assigned_nodes = engine.assigned_node_count(element) != 0;
        let shadow_root = engine.shadow_root_of(element);
        let content_visibility_hidden = content_visibility == crate::css::css_enums::content_visibility::HIDDEN;
        let planned = self.tree.node(node);
        let (kind, flags, facts) = (planned.kind(), planned.flags(), planned.facts());
        // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
        // 1. Remove irrelevant boxes:
        // The following boxes are discarded as if they were display:none:
        //     1. Children of a table-column.
        //     2. Children of a table-column-group which are not a table-column.
        let table_display = (facts & box_fact::TABLE_NON_ROOT != 0)
            .then(|| self.tree.style_of(node).map(|style| style.display()))
            .flatten();
        let is_table_column = table_display.is_some_and(|display| display.is_table_column());
        let is_table_column_group = table_display.is_some_and(|display| display.is_table_column_group());
        // NB: Neither the children nor the pseudo-elements of a box whose children are discarded get a box.
        let children_are_discarded = content_visibility_hidden || is_table_column;
        let enclosing_in_column_group_list = std::mem::replace(&mut self.in_column_group_list, is_table_column_group);
        // https://w3c.github.io/mathml-core/#space-mspace
        // The mspace empty element represents a blank space of any desired size, as set by its attributes.
        // NB: Its children therefore generate no boxes.
        let can_have_children = kind_can_have_children(kind, flags)
            && MathLayoutKind::from_construction_facts(construction_facts) != MathLayoutKind::Mspace;
        let may_have_pseudo_elements =
            can_have_children && !children_are_discarded && !self.has_svg_root && !publishes_no_pseudo_element_records;
        let can_have_generated_children =
            may_have_pseudo_elements && box_can_have_generated_children(kind, flags, construction_facts);

        // https://www.w3.org/TR/css-contain-2/#containment-style
        // The effects of the 'content' property's 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote'
        // must be scoped to the element's sub-tree.
        if has_style_containment {
            self.content.note_quote_step(QuoteStep::Save, None);
        }

        // Resolve counters now that we exist in the layout tree.
        let enters_counter_children = match &counters {
            OwnerCounters::Placed(placed) => {
                self.counters.enter_children(placed);
                true
            }
            OwnerCounters::Entered => false,
            OwnerCounters::AfterCommit { in_tree_order } => {
                self.events.push(PlanEvent::ResolveCounters {
                    element,
                    may_change_counters,
                    in_tree_order: *in_tree_order,
                });
                false
            }
        };
        // NB: The pseudo-element boxes of a box whose list the walk plans from a window stay as they are.
        let plans_window = self.in_unit_list && self.unit.is_some_and(|unit| unit.window.is_some());
        // NB: A ::marker comes first among its element's pseudo-elements and children in tree order, so its counters
        //     are resolved, and its content made, before theirs, but its box is placed once the children's are.
        let marker_event = self.events.len();
        let mut marker_counters = ContentCounters::AfterCommit;
        let mut marker_quote_step = None;
        if may_have_pseudo_elements && kind == NodeKind::ListItemBox {
            self.events.push(PlanEvent::Nothing);
            self.events.push(PlanEvent::Nothing);
            marker_quote_step = Some(self.content.reserve_quote_step());
            if !self.resolves_counters_after_commit
                && !plans_window
                && let Some(shape) = self.pseudo_element_box_shape(element, GENERATED_FOR_MARKER, node)
            {
                marker_counters =
                    self.place_pseudo_element_counters(element, GENERATED_FOR_MARKER, shape.is_list_item_marker);
            }
        }

        let enclosing_preceding_content_start = self.preceding_content_start;
        self.preceding_content_start = self.preceding_content.len();
        if can_have_generated_children && !plans_window {
            self.plan_pseudo_element(element, GENERATED_FOR_BEFORE, node, ChildPosition::Prepend, None);
        }

        if children_are_discarded {
            self.stale_subtrees
                .push((element, StaleSubtreeScope::DescendantsBoundedToRoot));
        }

        let lays_out_dom_children = !has_assigned_nodes && engine.first_dom_child(element).is_some();
        if (shadow_root.is_some() || lays_out_dom_children) && can_have_children && !children_are_discarded {
            self.tree.open_parent(node);
            if let Some(shadow_root) = shadow_root {
                // For replaced elements with shadow DOM children, wrap the children in an anonymous BlockContainer so
                // that a BFC handles their layout.
                let is_replaced_box_with_children = node_facts::kind_is_replaced_box(kind);
                if is_replaced_box_with_children {
                    let first_child = self.tree.node(node).first_child();
                    let wrapper =
                        if first_child.is_invalid() || !self.tree.node(first_child).has_flag(NodeFlag::Anonymous) {
                            self.tree.create_anonymous_wrapper(node, NodeSlotId::INVALID)
                        } else {
                            first_child
                        };
                    self.tree.open_parent(wrapper);
                }
                self.plan_dom_children(shadow_root);
                self.answer_marks_of_flattened_node(shadow_root);
                if is_replaced_box_with_children {
                    self.tree.close_parent();
                }
            } else if engine.element_adjustment_facts(element) & element_adjustment_fact::IS_SVG_SWITCH_ELEMENT != 0 {
                self.plan_svg_switch_children(element);
            } else {
                // NB: Only the box's ::after goes after its children here, and the box's children are its own. A
                //     fieldset or button box wraps its whole child list once the list is planned, which a run job's
                //     children would be left out of.
                self.child_list_may_split = self.fork_budget.is_some()
                    && !self.in_unit_list
                    && !has_assigned_nodes
                    && !self.has_svg_root
                    && !self.inside_svg_content
                    && !self.layout_top_layer
                    && !matches!(
                        kind,
                        NodeKind::InlineNode | NodeKind::ListItemBox | NodeKind::FrameSetBox | NodeKind::FieldSetBox
                    )
                    && flags & NodeFlag::UsesButtonLayout as u32 == 0
                    && facts & box_fact::TABULAR_CONTAINER == 0
                    && !is_table_column_group
                    && (!can_have_generated_children
                        || engine
                            .pseudo_published_style_record(element, pseudo_kind_of(GENERATED_FOR_AFTER))
                            .is_none());
                self.plan_dom_children(element);
            }
            self.tree.close_parent();
        }

        if has_assigned_nodes {
            if children_are_discarded {
                self.clear_assigned_slottables(element);
            } else {
                self.tree.open_parent(node);
                self.plan_assigned_slottables(element);
                self.tree.close_parent();
            }
        }

        let svg_attributes = self.arena.style_node_svg_attribute_facts(element);
        if svg_attributes.is_graphics_element {
            // NB: The box of an SVG resource a graphics element references is built again with the element's list.
            let in_unit_list = std::mem::take(&mut self.in_unit_list);
            self.plan_svg_resources(element, node, svg_attributes);
            self.in_unit_list = in_unit_list;
        }
        if can_have_children && !children_are_discarded && !self.has_svg_root && !plans_window {
            // Add ::marker and ::after once normal children are complete.
            if may_have_pseudo_elements && kind == NodeKind::ListItemBox {
                self.plan_pseudo_element(
                    element,
                    GENERATED_FOR_MARKER,
                    node,
                    ChildPosition::Prepend,
                    Some((
                        marker_counters,
                        marker_quote_step.expect("a list item's marker has a quote step"),
                    )),
                );
                if let Some(&PlanEvent::FillGeneratedContent {
                    generated_for: GENERATED_FOR_MARKER,
                    ..
                }) = self.events.last()
                {
                    if self.resolves_counters_after_commit {
                        self.events[marker_event] = PlanEvent::ResolveMarkerCounters(element);
                    }
                    self.events[marker_event + 1] = self.events.pop().expect("the marker's box is to be filled");
                }
            }
            if can_have_generated_children {
                self.plan_pseudo_element(element, GENERATED_FOR_AFTER, node, ChildPosition::Append, None);
            }
            // NB: The first letter of a box whose child list the walk plans again is split out again once the build
            //     is over, as every child list below the box may have changed.
            if !publishes_no_pseudo_element_records
                && !self.in_unit_list
                && node_facts::kind_is_block_container(kind)
                && engine.has_published_first_letter_style(element)
            {
                self.events.push(PlanEvent::SplitFirstLetter(element));
            }
            if self.writing_suggestion_parent == Some(element) {
                self.events
                    .push(PlanEvent::CreateWritingSuggestionBoxes { element, node });
            }
        }
        if kind == NodeKind::FieldSetBox {
            self.wrap_fieldset_contents_if_needed(node);
        }
        if flags & NodeFlag::UsesButtonLayout as u32 != 0 {
            self.wrap_button_contents_if_needed(node);
        }
        if has_style_containment {
            self.content.note_quote_step(QuoteStep::Restore, None);
        }
        if enters_counter_children {
            self.counters
                .leave_children(self.arena, self.engine, self.arena.counters_sets());
        }
        self.preceding_content.truncate(self.preceding_content_start);
        self.preceding_content_start = enclosing_preceding_content_start;
        self.in_column_group_list = enclosing_in_column_group_list;
        let is_unit_list = std::mem::replace(&mut self.in_unit_list, enclosing_in_unit_list);
        self.fix_up_tables_in_child_list(node, is_unit_list);
    }

    /// Fixes up the parts of tables in the child list of `node`, which the walk planned all of, see
    /// [`super::table_fixup`]: but for the list a walk plans again, which keeps boxes table fixup made before, and
    /// which the commit fixes up.
    fn fix_up_tables_in_child_list(&mut self, node: NodeSlotId, is_unit_list: bool) {
        if is_unit_list {
            self.tree.note_table_fixup_list_if_needed(node);
            return;
        }
        let engine = self.engine;
        self.tree
            .fix_up_tables_in_child_list(node, &|text| engine.text_is_ascii_whitespace(text));
    }

    /// Answers the layout tree update marks of `node`, which the walk flattened into the child list it is in: all of them
    /// where every box below it is new, and its own where the walk plans a list again, whose kept children may hold
    /// marks of their own.
    fn answer_marks_of_flattened_node(&mut self, node: StyleNodeID) {
        if self.in_unit_list {
            self.answered_own_marks.push(node);
        } else {
            self.answered_marks.push(node);
        }
    }

    fn wrap_button_contents_if_needed(&mut self, node: NodeSlotId) {
        use crate::css::style::layout_style::AnonymousStyleKind;
        // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
        // If the element is an input element, or if it is a button element and its computed value for 'display' is not
        // 'inline-grid', 'grid', 'inline-flex', or 'flex', then the element's box has a child anonymous button content
        // box with the following behaviors:
        if self
            .tree
            .style_of(node)
            .is_some_and(|style| style.display().is_grid_inside() || style.display().is_flex_inside())
        {
            return;
        }
        let (flex_wrapper, content_box) = self.tree.wrap_button_contents(node);
        self.commit_one_by_one(
            flex_wrapper,
            OneByOneRowSource::ContentsWrapper {
                style_parent: node,
                style_kind: AnonymousStyleKind::ButtonFlexWrapper,
            },
        );
        self.commit_one_by_one(
            content_box,
            OneByOneRowSource::ContentsWrapper {
                style_parent: node,
                style_kind: AnonymousStyleKind::ButtonContentBox,
            },
        );
    }

    fn wrap_fieldset_contents_if_needed(&mut self, node: NodeSlotId) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
        // The anonymous fieldset content box is expected to appear after the rendered legend and is expected to contain
        // the content (including the '::before' and '::after' pseudo-elements) of the fieldset element except for the
        // rendered legend, if there is one.
        let legend = self.tree.rendered_legend(node);
        let is_flex = self
            .tree
            .style_of(node)
            .is_some_and(|style| style.display().is_flex_inside());
        if legend.is_invalid() && !is_flex {
            return;
        }
        let wrapper = self.tree.wrap_fieldset_contents(node, legend, is_flex);
        self.commit_one_by_one(
            wrapper,
            OneByOneRowSource::ContentsWrapper {
                style_parent: node,
                style_kind: crate::css::style::layout_style::AnonymousStyleKind::FieldsetContentWrapper,
            },
        );
    }

    fn plan_display_contents(&mut self, element: StyleNodeID) {
        let engine = self.engine;
        let has_assigned_nodes = engine.assigned_node_count(element) != 0;
        let lays_out_dom_children = !has_assigned_nodes && engine.first_dom_child(element).is_some();
        let (content_visibility_hidden, may_change_counters) =
            engine
                .element_layout_inputs(element)
                .record
                .map_or((false, true), |record| {
                    let summary = record.summary();
                    (
                        summary.content_visibility == crate::css::css_enums::content_visibility::HIDDEN,
                        summary.may_change_counters,
                    )
                });
        // The element's subtree loses whatever boxes it had before its children are built again.
        // NB: In a child list planned again, the boxes of its children are entries of the list, which the list keeps or
        //     drops.
        if !self.in_unit_list {
            self.stale_subtrees.push((element, StaleSubtreeScope::Inclusive));
        }
        let counters = self.element_counters(element, may_change_counters);
        match &counters {
            OwnerCounters::Placed(placed) => self.counters.enter_children(placed),
            OwnerCounters::AfterCommit { in_tree_order } => self.events.push(PlanEvent::ResolveCounters {
                element,
                may_change_counters,
                in_tree_order: *in_tree_order,
            }),
            OwnerCounters::Entered => unreachable!("only a job's walk starts below an element"),
        }
        let parent = self.current_parent();
        if !content_visibility_hidden {
            self.plan_pseudo_element(element, GENERATED_FOR_BEFORE, parent, ChildPosition::Append, None);
            if let Some(shadow_root) = engine.shadow_root_of(element) {
                self.plan_dom_children(shadow_root);
                self.answer_marks_of_flattened_node(shadow_root);
            } else if lays_out_dom_children {
                self.plan_dom_children(element);
            }
        }
        if has_assigned_nodes {
            if content_visibility_hidden {
                self.clear_assigned_slottables(element);
            } else {
                self.plan_assigned_slottables(element);
            }
        }
        if !content_visibility_hidden {
            self.plan_pseudo_element(element, GENERATED_FOR_AFTER, parent, ChildPosition::Append, None);
        }
        if matches!(counters, OwnerCounters::Placed(_)) {
            self.counters
                .leave_children(self.arena, self.engine, self.arena.counters_sets());
        }
        self.answer_marks_of_flattened_node(element);
    }

    /// Plans the first child of an SVG `<switch>` that renders, and clears the others.
    /// https://svgwg.org/svg2-draft/struct.html#SwitchElement
    /// The ‘switch’ element evaluates the ‘requiredExtensions’ and ‘systemLanguage’ attributes on its direct child
    /// elements in order, and then processes and renders the first child for which these attributes evaluate to
    /// true. All others will be bypassed and therefore not rendered.
    fn plan_svg_switch_children(&mut self, switch_element: StyleNodeID) {
        let engine = self.engine;
        // NB: A child whose conditional processing attributes evaluate to false asks for no box.
        let rendered_child = engine.dom_children(switch_element).find(|&child| {
            engine.element_adjustment_facts(child) & element_adjustment_fact::IS_SVG_ELEMENT != 0
                && engine.element_box_kind(child) != ElementBoxKind::NoBox
        });
        for child in engine.dom_children(switch_element) {
            if Some(child) != rendered_child {
                self.stale_subtrees.push((child, StaleSubtreeScope::NodeOnly));
            }
        }
        if let Some(rendered_child) = rendered_child {
            self.plan_node(rendered_child);
        }
    }

    /// Plans the boxes of the SVG resources a graphics element's `mask`, `clip-path`, `fill` and `stroke` name, as
    /// children of the element's box, `node`.
    fn plan_svg_resources(
        &mut self,
        referrer: StyleNodeID,
        node: NodeSlotId,
        svg_attributes: crate::layout::svg_formatting_context::FfiSvgAttributeFacts,
    ) {
        let reference = |atom, required_fact: u32| {
            self.arena
                .element_by_svg_reference(referrer, atom)
                .filter(|&resolved| self.engine.element_adjustment_facts(resolved) & required_fact != 0)
        };
        let masks = [
            reference(
                svg_attributes.mask_reference_atom,
                element_adjustment_fact::IS_SVG_MASK_ELEMENT,
            ),
            reference(
                svg_attributes.clip_path_reference_atom,
                element_adjustment_fact::IS_SVG_CLIP_PATH_ELEMENT,
            ),
        ];
        let patterns = [
            reference(
                svg_attributes.fill_reference_atom,
                element_adjustment_fact::IS_SVG_PATTERN_ELEMENT,
            ),
            reference(
                svg_attributes.stroke_reference_atom,
                element_adjustment_fact::IS_SVG_PATTERN_ELEMENT,
            ),
        ];
        for resource in masks.into_iter().flatten() {
            let enclosing_mode = self.layout_svg_mask_or_clip_path;
            let enclosing_has_svg_root = self.has_svg_root;
            self.layout_svg_mask_or_clip_path = true;
            self.has_svg_root = true;
            self.tree.open_parent(node);
            if !self.resource_box_is_being_built(resource) {
                self.plan_svg_resource_content(resource);
                self.report_svg_resource_reference(resource, referrer);
            }
            self.tree.close_parent();
            self.has_svg_root = enclosing_has_svg_root;
            self.layout_svg_mask_or_clip_path = enclosing_mode;
        }
        let mut seen_content_elements: smallvec::SmallVec<[StyleNodeID; 2]> = smallvec::SmallVec::new();
        for pattern in patterns.into_iter().flatten() {
            let Some(content_element) = self.svg_pattern_content_element(pattern) else {
                continue;
            };
            if seen_content_elements.contains(&content_element) {
                continue;
            }
            seen_content_elements.push(content_element);
            let enclosing_mode = self.layout_svg_pattern;
            self.layout_svg_pattern = true;
            self.tree.open_parent(node);
            if !self.resource_box_is_being_built(content_element) {
                self.plan_svg_resource_content(content_element);
                // The referenced pattern may inherit its content from another pattern via href. Removing either
                // element invalidates the attached resource box, so the referrer is registered with both.
                self.report_svg_resource_reference(content_element, referrer);
                if pattern != content_element {
                    self.report_svg_resource_reference(pattern, referrer);
                }
            }
            self.tree.close_parent();
            self.layout_svg_pattern = enclosing_mode;
        }
    }

    /// Plans `resource`, the root of SVG resource content, below the box of the element that references it, away from its
    /// place in tree order: its counters are resolved after the commit.
    fn plan_svg_resource_content(&mut self, resource: StyleNodeID) {
        let enclosing = std::mem::replace(&mut self.resolves_counters_after_commit, true);
        self.restarts_counters_at_next_owner = true;
        self.plan_node(resource);
        self.restarts_counters_at_next_owner = false;
        self.resolves_counters_after_commit = enclosing;
    }

    /// Whether the box the resource element has is one of the boxes the walk is building the children of: a resource
    /// that references itself, directly or not, is not built inside itself again.
    fn resource_box_is_being_built(&self, resource: StyleNodeID) -> bool {
        if let Some(&node) = self.latest_resource_boxes.get(&resource) {
            return self.tree.is_open_parent(node);
        }
        let row = self.arena.bound_row(resource);
        !row.is_invalid() && (row == self.root_row || self.enclosing_ancestor_rows.contains(&row))
    }

    /// Tells the document that a graphics element's box holds the content of an SVG resource, which it has to rebuild
    /// the referencing subtree for when the resource goes away or changes.
    fn report_svg_resource_reference(&mut self, resource: StyleNodeID, graphics_element: StyleNodeID) {
        self.reports.push(FfiCommitMessage {
            style_node: resource.raw(),
            other_style_node: graphics_element.raw(),
            kind: FfiCommitMessageKind::SvgResourceReferenced,
        });
    }

    /// The pattern whose children a `<pattern>` draws: itself if it has element children, and otherwise the pattern
    /// its `href` chain leads to, following `SVGPatternElement::pattern_content_element`.
    fn svg_pattern_content_element(&self, pattern: StyleNodeID) -> Option<StyleNodeID> {
        let arena = self.arena;
        let mut current = pattern;
        // A pattern may name itself somewhere along the chain, so every pattern stepped to is remembered and a second
        // arrival ends the walk.
        let mut seen: smallvec::SmallVec<[StyleNodeID; 4]> = smallvec::SmallVec::new();
        loop {
            if arena.has_dom_element_children(current) {
                return Some(current);
            }
            let atom = arena.style_node_svg_attribute_facts(current).reference_fragment_atom;
            let linked = arena.element_by_document_id(atom)?;
            if linked == current || seen.contains(&linked) {
                return None;
            }
            if self.engine.element_adjustment_facts(linked) & element_adjustment_fact::IS_SVG_PATTERN_ELEMENT == 0 {
                return None;
            }
            seen.push(linked);
            current = linked;
        }
    }

    /// Plans the nodes assigned to the slot `slot`, in flat tree order.
    fn plan_assigned_slottables(&mut self, slot: StyleNodeID) {
        for index in 0..self.engine.assigned_node_count(slot) {
            self.plan_node(self.engine.assigned_node(slot, index));
        }
    }

    /// Clears what the nodes assigned to the slot `slot` held, for a slot whose own box hides its content.
    fn clear_assigned_slottables(&mut self, slot: StyleNodeID) {
        for index in 0..self.engine.assigned_node_count(slot) {
            let node = self.engine.assigned_node(slot, index);
            self.stale_subtrees
                .push((node, StaleSubtreeScope::InclusiveBoundedToRoot));
        }
    }

    /// Plans the box of the pseudo-element of kind `generated_for` of `element` below `parent`, if a fresh build makes
    /// one, with the items its generated content puts in it. A `::marker` comes with where its counters are and the
    /// quote step reserved for it before its element's children, see plan_element_box_descendants().
    fn plan_pseudo_element(
        &mut self,
        element: StyleNodeID,
        generated_for: u8,
        parent: NodeSlotId,
        position: ChildPosition,
        marker: Option<(ContentCounters, usize)>,
    ) {
        let Some(shape) = self.pseudo_element_box_shape(element, generated_for, parent) else {
            return;
        };
        self.plan_pseudo_element_box(element, generated_for, parent, position, marker, shape);
    }

    // NB: Kept out of line, as most elements have no pseudo-element box, so that the walk over them stays small.
    #[inline(never)]
    fn plan_pseudo_element_box(
        &mut self,
        element: StyleNodeID,
        generated_for: u8,
        parent: NodeSlotId,
        position: ChildPosition,
        marker: Option<(ContentCounters, usize)>,
        shape: PseudoElementBoxShape,
    ) {
        let PseudoElementBoxShape {
            kind,
            flags,
            box_facts,
            display,
            is_list_item_marker,
            marker_position_is_inside,
            decision,
        } = shape;
        // NB: The counters of a ::marker are resolved before its element's children, see
        //     plan_element_box_descendants().
        let (counters, quote_step) = match marker {
            Some((counters, quote_step)) => (counters, Some(quote_step)),
            None => (
                self.place_pseudo_element_counters(element, generated_for, is_list_item_marker),
                None,
            ),
        };
        if self.in_unit_list && Some(element) == self.unit_element {
            self.unit_entry = self.unit_entry_count;
            self.unit_entry_count += 1;
            if let Some(row) = self.keepable_pseudo_element_row(element, generated_for, kind, flags) {
                let entry_top = self.arena.list_entry_top_row(row);
                let top = self.arena.data(entry_top);
                let mut kept_facts = box_facts;
                if entry_top != row {
                    // NB: Table fixup made the wrapper of a table the list keeps already.
                    kept_facts &= !(box_fact::TABLE_INSIDE | box_fact::TABULAR_CONTAINER);
                }
                let mut kept = PlanNode::new(top.kind.get(), top.flags.get(), kept_facts);
                kept.generated_for = top.generated_for.get();
                kept.style = top.style.get();
                self.tree.push_stand_in(entry_top, kept);
                self.place_pseudo_element_box(
                    parent,
                    entry_top,
                    is_list_item_marker && !marker_position_is_inside,
                    position,
                );
                if generated_for != GENERATED_FOR_MARKER {
                    self.last_kept_label = self.arena.node_pre_order_label(entry_top);
                }
                self.last_kept_entry = Some(self.unit_entry);
                if matches!(generated_for, GENERATED_FOR_BEFORE | GENERATED_FOR_AFTER) {
                    self.preceding_content.push(PrecedingContent::Box(entry_top));
                }
                self.content
                    .note_quote_step(QuoteStep::KeptPseudoElement(element, generated_for), quote_step);
                return;
            }
        }
        // NB: A list marker belongs to the list item box its element was built as, see pseudo_element_box_shape(), which
        //     shows the element's style.
        let engine = self.engine;
        let list_item_style = is_list_item_marker
            .then(|| engine.element_layout_inputs(element).record)
            .flatten()
            .map(|record| ComputedValuesView::of(record.payloads));
        let mut plan_node = PlanNode::new(kind, flags, box_facts);
        plan_node.generated_for = generated_for;
        let node = self.tree.push(plan_node);
        // NB: The box takes its style as the commit stamps it, but table fixup reads its display before.
        if box_facts & box_fact::TABLE_PART != 0 {
            self.tree.note_table_part_display(node, display);
        }
        // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
        // 1. Remove irrelevant boxes:
        // The following boxes are discarded as if they were display:none:
        //   1. Children of a table-column.
        //   2. Children of a table-column-group which are not a table-column.
        // NB: The items of generated content are never a table-column, so a box of either display gets none.
        let style_view = self
            .engine
            .pseudo_published_style_view(element, pseudo_kind_of(generated_for));
        let view = style_view.filter(|_| !display.is_table_column() && !display.is_table_column_group());
        // NB: The quotes of the content move the quote depth even where the box gets no items.
        let quote_view = style_view.filter(|view| view.content_opens_or_closes_quotes());
        if generated_for != GENERATED_FOR_BACKDROP {
            // NB: Table fixup reads whether the box holds only white space before the commit spells its content.
            if generated_for != GENERATED_FOR_MARKER
                && let Some(view) = view
                && crate::layout::generated_content::content_is_white_space(view)
            {
                self.tree.note_white_space_pseudo_element_box(node);
            }
            if quote_view.is_some() {
                self.tree.quote_generators.push(element);
            }
        }

        self.place_pseudo_element_box(
            parent,
            node,
            is_list_item_marker && !marker_position_is_inside,
            position,
        );
        self.commit_one_by_one(
            node,
            OneByOneRowSource::PseudoElement {
                element,
                generated_for,
                decision,
            },
        );
        let first_pending = self.content.pending.len() as u32;
        let pending = view.and_then(|view| {
            self.plan_generated_content(
                CounterOwner { element, generated_for },
                node,
                view,
                shape,
                counters,
                list_item_style,
            )
        });
        if let Some(quote_view) = quote_view {
            let step = QuoteStep::Content {
                pending: pending
                    .filter(|&pending| matches!(self.content.pending[pending as usize].kind, PendingKind::Content)),
                transfer: quote_view.content_quote_transfer(),
            };
            self.content.note_quote_step(step, quote_step);
        }
        // NB: Table fixup goes over the generated content of a pseudo-element box whose display is a table's once the
        //     commit spelled it.
        if box_facts & (box_fact::TABULAR_CONTAINER | box_fact::TABLE_NON_ROOT) != 0 {
            self.tree.table_fixup_lists.push(node);
        }
        if matches!(generated_for, GENERATED_FOR_BEFORE | GENERATED_FOR_AFTER) {
            self.preceding_content.push(PrecedingContent::Box(node));
        }
        if matches!(counters, ContentCounters::AfterCommit) {
            self.events.push(PlanEvent::FillGeneratedContent {
                element,
                generated_for,
                first_pending,
                end_pending: self.content.pending.len() as u32,
            });
        }
    }

    /// Plans the items the content of the pseudo-element `owner`, whose box is `node` and whose style is `view`, puts in
    /// the box: its marker string for a list marker whose content is `normal`, the
    /// list marker a list-item pseudo-element nests, and the items of its `content` list. Spells every text whose
    /// characters it knows, and answers the content of [`ContentPlan::pending`] the others wait in, if any.
    fn plan_generated_content(
        &mut self,
        owner: CounterOwner,
        node: NodeSlotId,
        view: ComputedValuesView<'a>,
        shape: PseudoElementBoxShape,
        counters: ContentCounters,
        list_item_style: Option<ComputedValuesView<'a>>,
    ) -> Option<u32> {
        use crate::layout::generated_content::{
            AccessibleText, ContentItem, ContentRecord, MarkerContent, content_is_normal, marker_content,
            marker_counter_styles_in_use, resolve_content,
        };
        let engine = self.engine;
        let arena = self.arena;
        let CounterOwner { element, generated_for } = owner;
        let tree_scope = engine.counter_style_tree_scope(element);
        let display = view.display();
        let keeps_empty_text = display.is_inline_outside() && display.is_flow_inside();

        if shape.kind == NodeKind::ListItemBox {
            self.plan_nested_list_marker(element, generated_for, node, view, counters, tree_scope);
        }

        if shape.is_list_item_marker && content_is_normal(view.content_value()) {
            let list_item_style = list_item_style.expect("a list marker belongs to a list item box with a style");
            let content = marker_content(arena, tree_scope, view, list_item_style);
            let counter_styles = marker_counter_styles_in_use(&content);
            self.content.records.push((
                owner,
                ContentRecord {
                    counter_styles,
                    accessible_text: AccessibleText::Unchanged,
                },
            ));
            let flags_before_items = self.tree.node(node).flags();
            return match content {
                MarkerContent::Image => {
                    self.place_generated_image(
                        element,
                        generated_for,
                        node,
                        node,
                        super::GeneratedImage::ListStyleImage,
                    );
                    None
                }
                MarkerContent::String(string) => {
                    if keeps_empty_text || !string.is_empty() {
                        let row = self.push_generated_text_row(element, generated_for);
                        self.place(node, row, true, ChildPosition::Append);
                        self.content.texts.push((row, string.to_vec()));
                    }
                    None
                }
                MarkerContent::CounterStyle(counter_style) => {
                    if !counter_style
                        .as_deref()
                        .unwrap_or(crate::css::counter_representation::decimal())
                        .representation_is_constant()
                    {
                        self.report_list_item_counter_rendering(element);
                    }
                    let row = self.push_generated_text_row(element, generated_for);
                    self.place(node, row, true, ChildPosition::Append);
                    Some(self.push_pending_content(PendingContent {
                        owner,
                        kind: PendingKind::MarkerCounter(counter_style),
                        box_row: node,
                        rows: vec![row],
                        keeps_empty_text,
                        flags_before_items,
                        counters,
                        tree_scope,
                        quote_depth: None,
                    }))
                }
            };
        }

        let resolved = resolve_content(arena, tree_scope, view, None, None);
        if resolved.renders_list_item_counter_value {
            self.report_list_item_counter_rendering(element);
        }
        let decision_makes_items = resolved.is_list && shape.decision != PseudoElementDecision::ContentReplacement;
        let flags_before_items = self.tree.node(node).flags();
        let mut rows = Vec::new();
        let mut is_pending = matches!(resolved.accessible_text, AccessibleText::Pending);
        for item in &resolved.items {
            if !decision_makes_items {
                break;
            }
            let row = match item {
                ContentItem::Text { text: Some(text), made } => {
                    if !made || (!keeps_empty_text && text.is_empty()) {
                        rows.push(NodeSlotId::INVALID);
                        continue;
                    }
                    let row = self.push_generated_text_row(element, generated_for);
                    self.place(node, row, true, ChildPosition::Append);
                    self.content.texts.push((row, text.clone()));
                    row
                }
                ContentItem::Text { text: None, .. } => {
                    is_pending = true;
                    let row = self.push_generated_text_row(element, generated_for);
                    self.place(node, row, true, ChildPosition::Append);
                    row
                }
                ContentItem::Image(content_index) => self.place_generated_image(
                    element,
                    generated_for,
                    node,
                    node,
                    super::GeneratedImage::Content(*content_index as u32),
                ),
            };
            rows.push(row);
        }
        if !decision_makes_items {
            rows = vec![NodeSlotId::INVALID; resolved.items.len()];
        }
        if !is_pending {
            self.content.records.push((
                owner,
                ContentRecord {
                    counter_styles: resolved.counter_styles,
                    accessible_text: resolved.accessible_text,
                },
            ));
            return None;
        }
        Some(self.push_pending_content(PendingContent {
            owner,
            kind: PendingKind::Content,
            box_row: node,
            rows,
            keeps_empty_text,
            flags_before_items,
            counters,
            tree_scope,
            quote_depth: None,
        }))
    }

    /// Plans the list marker a list-item pseudo-element of kind `generated_for` of `element`, whose box is `node` and
    /// whose style is `view`, nests as its first child, with its marker string, if it shows anything.
    // FIXME: This code actually computes style for element::marker, and shouldn't for element::pseudo::marker.
    fn plan_nested_list_marker(
        &mut self,
        element: StyleNodeID,
        generated_for: u8,
        node: NodeSlotId,
        view: ComputedValuesView<'a>,
        counters: ContentCounters,
        tree_scope: u32,
    ) {
        use crate::layout::generated_content::{MarkerContent, marker_content};
        let engine = self.engine;
        // NB: The commit styles the marker with the record of the element's `::marker`, which the engine derives for
        //     that read alone where the element holds none. The walk, which changes nothing in the engine, reads the
        //     `list-style-image` the marker inherits from the element's own record where it published none for its
        //     `::marker`. The marker is inline, as `display` does not apply to a `::marker`.
        let Some(marker_style) = engine
            .pseudo_published_style_view(element, pseudo_kind_of(GENERATED_FOR_MARKER))
            .or_else(|| {
                engine
                    .element_layout_inputs(element)
                    .record
                    .map(|record| ComputedValuesView::of(record.payloads))
            })
        else {
            return;
        };
        // As for an element's ::marker, the marker shows its own list-style-image, or else the list item's
        // list-style-type, which shows nothing where it's none. Where neither shows anything, there's no marker.
        if !marker_style.list_style_image_is_set() && view.list_style_type_is_none() {
            return;
        }
        let mut flags = node_facts::construction_flags(NodeKind::ListItemMarkerBox, true, 0);
        if view.list_style_position_is_inside() {
            flags |= NodeFlag::ListMarkerIsInside as u32;
        }
        let mut marker_node = PlanNode::new(
            NodeKind::ListItemMarkerBox,
            flags,
            box_facts_of_display(FfiDisplay::outside_and_inside(
                crate::css::css_enums::display_outside::INLINE,
                crate::css::css_enums::display_inside::FLOW,
                false,
            )),
        );
        marker_node.generated_for = generated_for;
        let marker = self.tree.push(marker_node);
        self.tree.attach(node, marker, NodeSlotId::INVALID);
        self.commit_one_by_one(
            marker,
            OneByOneRowSource::NestedListMarker {
                generator: element,
                generated_for,
            },
        );
        let flags_before_items = self.tree.node(marker).flags();
        let item = match marker_content(self.arena, tree_scope, marker_style, view) {
            MarkerContent::Image => {
                let row = self.push_generated_image_row(
                    element,
                    generated_for,
                    marker,
                    super::GeneratedImage::ListStyleImage,
                );
                self.tree.attach(marker, row, NodeSlotId::INVALID);
                row
            }
            MarkerContent::String(string) => {
                let row = self.push_generated_text_row(element, generated_for);
                self.tree.attach(marker, row, NodeSlotId::INVALID);
                self.content.texts.push((row, string.to_vec()));
                row
            }
            MarkerContent::CounterStyle(counter_style) => {
                if !counter_style
                    .as_deref()
                    .unwrap_or(crate::css::counter_representation::decimal())
                    .representation_is_constant()
                {
                    self.report_list_item_counter_rendering(element);
                }
                let row = self.push_generated_text_row(element, generated_for);
                self.tree.attach(marker, row, NodeSlotId::INVALID);
                self.push_pending_content(PendingContent {
                    owner: CounterOwner { element, generated_for },
                    kind: PendingKind::MarkerCounter(counter_style),
                    box_row: marker,
                    rows: vec![row],
                    keeps_empty_text: true,
                    flags_before_items,
                    counters,
                    tree_scope,
                    quote_depth: None,
                });
                row
            }
        };
        debug_assert_eq!(self.tree.node(item).parent(), marker);
        self.tree.set_flag(marker, NodeFlag::ChildrenAreInline, true);
    }

    fn push_pending_content(&mut self, pending: PendingContent) -> u32 {
        self.content.pending.push(pending);
        (self.content.pending.len() - 1) as u32
    }

    /// Makes the row of a text of the generated content of the pseudo-element of kind `generated_for` of `element`,
    /// which carries no style of its own and answers by its generator's name.
    fn push_generated_text_row(&mut self, element: StyleNodeID, generated_for: u8) -> NodeSlotId {
        let mut text_node = PlanNode::new(
            NodeKind::GeneratedTextNode,
            node_facts::construction_flags(NodeKind::GeneratedTextNode, true, 0),
            box_fact::INLINE_OUTSIDE,
        );
        text_node.generated_for = generated_for;
        let row = self.tree.push(text_node);
        self.tree.set_columns(
            row,
            super::RowColumns {
                style_record: 0,
                style_node: Some(element),
                unique_node_id: self.engine.element_unique_node_id(element),
                ..Default::default()
            },
        );
        self.content.text_rows.push(row);
        row
    }

    /// Makes the row of an image of the generated content of the pseudo-element of kind `generated_for` of `element`,
    /// an inline replaced box whose style the commit derives from that of `style_box`.
    fn push_generated_image_row(
        &mut self,
        element: StyleNodeID,
        generated_for: u8,
        style_box: NodeSlotId,
        image: super::GeneratedImage,
    ) -> NodeSlotId {
        // https://drafts.csswg.org/css-content-3/#content-property
        // For <image>, this is an inline anonymous replaced element.
        let display = FfiDisplay::outside_and_inside(
            crate::css::css_enums::display_outside::INLINE,
            crate::css::css_enums::display_inside::FLOW,
            false,
        );
        let mut image_node = PlanNode::new(
            NodeKind::ImageBox,
            node_facts::construction_flags(NodeKind::ImageBox, true, 0),
            box_facts_of_display(display) | box_fact::HAS_STYLE,
        );
        image_node.generated_for = generated_for;
        let row = self.tree.push(image_node);
        self.commit_one_by_one(
            row,
            OneByOneRowSource::GeneratedImage {
                generator: element,
                generated_for,
                style_box,
                image,
            },
        );
        row
    }

    /// Makes the row of an image of generated content and places it in `node`, a pseudo-element's box.
    fn place_generated_image(
        &mut self,
        element: StyleNodeID,
        generated_for: u8,
        node: NodeSlotId,
        style_box: NodeSlotId,
        image: super::GeneratedImage,
    ) -> NodeSlotId {
        let row = self.push_generated_image_row(element, generated_for, style_box, image);
        self.place(node, row, true, ChildPosition::Append);
        row
    }

    /// Tells the document that the generated content of a pseudo-element of `element` shows the value of a `list-item`
    /// counter.
    fn report_list_item_counter_rendering(&mut self, element: StyleNodeID) {
        self.reports.push(FfiCommitMessage::new(
            element.raw(),
            FfiCommitMessageKind::ListItemCounterValueRendered,
        ));
    }

    /// The box a fresh build makes for the pseudo-element of kind `generated_for` of `element`, placed below `parent`,
    /// if it makes one.
    fn pseudo_element_box_shape(
        &self,
        element: StyleNodeID,
        generated_for: u8,
        parent: NodeSlotId,
    ) -> Option<PseudoElementBoxShape> {
        let engine = self.engine;
        if engine.element_box_kind(element) == ElementBoxKind::FrameSet {
            return None;
        }
        let view = engine.pseudo_published_style_view(element, pseudo_kind_of(generated_for))?;
        let pseudo_element = match generated_for {
            GENERATED_FOR_BEFORE => FfiPseudoElement::Before,
            GENERATED_FOR_AFTER => FfiPseudoElement::After,
            GENERATED_FOR_BACKDROP => FfiPseudoElement::Backdrop,
            _ => FfiPseudoElement::Marker,
        };
        let display = view.display();
        // NB: See plan_element_box_descendants(): a table-column-group keeps only its table-column children.
        if self.in_column_group_list && !display.is_table_column() {
            return None;
        }
        let mut facts = PseudoElementFacts {
            has_style: true,
            pseudo_element,
            content_type: if !view.content_is_keyword() {
                ComputedContentType::List
            } else if view.content_keyword_is_none() {
                ComputedContentType::None
            } else {
                ComputedContentType::Normal
            },
            display_is_none: display.is_none(),
            display_is_contents: display.is_contents(),
            display_is_list_item: display.is_list_item(),
            has_content_replacement: view.content_is_single_image(),
            normal_marker_has_content: false,
            marker_position_is_inside: false,
        };
        // A ::marker belongs to the list item box its element was built as, and takes its position and its default
        // content from that box's style.
        let is_marker_of_list_item =
            pseudo_element == FfiPseudoElement::Marker && self.tree.node(parent).kind() == NodeKind::ListItemBox;
        if is_marker_of_list_item && let Some(list_style) = self.tree.style_of(parent) {
            facts.normal_marker_has_content =
                !list_style.list_style_type_is_none() || list_style.list_style_image_is_set();
            facts.marker_position_is_inside = list_style.list_style_position_is_inside();
        }
        let decision = pseudo_element_decision(facts, is_marker_of_list_item);
        let is_list_item_marker = decision == PseudoElementDecision::Box && is_marker_of_list_item;
        let kind = match decision {
            PseudoElementDecision::None => return None,
            // The image box owns the image it replaces the pseudo-element's contents with.
            PseudoElementDecision::ContentReplacement => NodeKind::ImageBox,
            PseudoElementDecision::Contents => NodeKind::InlineNode,
            PseudoElementDecision::Box if is_list_item_marker => NodeKind::ListItemMarkerBox,
            PseudoElementDecision::Box => node_kind_for_display(display)?,
        };
        let mut flags = node_facts::construction_flags(kind, true, 0) | NodeFlag::IsPseudoElementPrincipalBox as u32;
        if is_list_item_marker && facts.marker_position_is_inside {
            flags |= NodeFlag::ListMarkerIsInside as u32;
        }
        let display = used_display_of_box(kind, flags, display);
        let box_facts = if decision == PseudoElementDecision::Contents {
            box_facts_of_display(display) | box_fact::HAS_STYLE
        } else {
            (box_facts_of_style(view) & !box_fact::OF_DISPLAY)
                | box_facts_of_display(display)
                | item_context_facts(kind, flags, display)
        };
        Some(PseudoElementBoxShape {
            kind,
            flags,
            box_facts,
            display,
            is_list_item_marker,
            marker_position_is_inside: facts.marker_position_is_inside,
            decision,
        })
    }

    /// Places the box `node` of a pseudo-element among the descendants of `parent`.
    fn place_pseudo_element_box(
        &mut self,
        parent: NodeSlotId,
        node: NodeSlotId,
        is_outside_list_item_marker: bool,
        position: ChildPosition,
    ) {
        // https://drafts.csswg.org/css-lists-3/#list-style-position-outside
        // "the marker box is a block container and is placed outside the principal block box"
        if is_outside_list_item_marker {
            let first_child = self.tree.node(parent).first_child();
            self.tree.attach(parent, node, first_child);
        } else {
            let is_inline_outside = self.tree.node(node).has_fact(box_fact::INLINE_OUTSIDE);
            self.place(parent, node, is_inline_outside, position);
        }
    }

    /// The box of the `::before`, `::after` or `::marker` of the element whose child list the walk plans again that the
    /// walk can keep: no mark asked for it again, a fresh build makes a box of the same kind for it, and it is attached
    /// in the list. A `::marker` goes first among the boxes whatever comes before it in the walk.
    fn keepable_pseudo_element_row(
        &self,
        element: StyleNodeID,
        generated_for: u8,
        kind: NodeKind,
        flags: u32,
    ) -> Option<NodeSlotId> {
        let unit = self.unit?;
        if !unit.keeps_pseudo_element_boxes || self.unit_entry >= unit.fresh_from_entry {
            return None;
        }
        // NB: A replaced box lays out the children of its shadow root in its first anonymous child, which is a list of
        //     the list, and may be its ::before box.
        if node_facts::kind_is_replaced_box(self.tree.node(self.root_row).kind()) {
            return None;
        }
        let arena = self.arena;
        let row = arena.bound_pseudo_element_row(element, generated_for);
        if row.is_invalid() {
            return None;
        }
        let data = arena.data(row);
        const PLACEMENT_FLAGS: u32 = NodeFlag::IsPseudoElementPrincipalBox as u32 | NodeFlag::ListMarkerIsInside as u32;
        if data.kind.get() != kind || (data.flags.get() ^ flags) & PLACEMENT_FLAGS != 0 {
            return None;
        }
        let entry_top = arena.list_entry_top_row(row);
        if !arena.is_in_child_list_of(entry_top, self.root_row) {
            return None;
        }
        if generated_for != GENERATED_FOR_MARKER && arena.node_pre_order_label(entry_top) <= self.last_kept_label {
            return None;
        }
        Some(row)
    }

    fn plan_text(&mut self, text: StyleNodeID) {
        let engine = self.engine;
        let parent = self.current_parent();
        // NB: See plan_element_box_descendants(): a table-column-group keeps only its table-column children.
        if self.tree.node(parent).kind() == NodeKind::FrameSetBox || self.in_column_group_list {
            self.note_text_without_box(text);
            self.note_stale_text(text);
            return;
        }
        let text_is_ascii_whitespace = engine.text_is_ascii_whitespace(text);
        if text_is_ascii_whitespace && !self.whitespace_text_needs_box(parent, text) {
            self.note_text_without_box(text);
            self.note_stale_text(text);
            return;
        }
        let facts = engine.text_style_parent_facts(text);
        let needs_style_wrapper = crate::layout::tree_builder::display_contents_text_needs_style_wrapper(
            facts.has_style_parent,
            facts.parent_display_is_contents,
            text_is_ascii_whitespace,
            facts.parent_collapses_whitespace,
        );
        if self.in_unit_list && self.plan_kept_text(text, needs_style_wrapper) {
            return;
        }
        let construction_facts = engine.element_construction_facts(text);
        let mut flags = node_facts::construction_flags(NodeKind::TextNode, false, construction_facts);
        let is_in_user_agent_shadow_tree =
            construction_facts & element_construction_fact::IS_IN_USER_AGENT_SHADOW_TREE != 0;
        if crate::layout::tree_builder::text_produces_line_box_fragment_when_empty(
            engine,
            text,
            is_in_user_agent_shadow_tree,
        ) {
            flags |= NodeFlag::ProducesLineBoxFragmentWhenEmpty as u32;
        }
        if self.arena.style_node_has_scroll_offset(text) {
            flags |= NodeFlag::HasScrollOffset as u32;
        }
        let mut text_facts = box_fact::INLINE_OUTSIDE;
        if let Some(parent) = engine
            .text_parent(text)
            .filter(|parent| parent.element_index().is_some())
            && !engine.publishes_no_pseudo_element_records(parent)
            && self.holds_highlight_style(parent)
        {
            text_facts |= box_fact::PARENT_HOLDS_HIGHLIGHT_STYLE;
        }
        self.places_boxes_of_unbound_nodes |= self.arena.bound_row(text).is_invalid();
        let text_node = self.tree.push(PlanNode::new(NodeKind::TextNode, flags, text_facts));
        *self.tree.row_mut(text_node).dom_paint_facts.get_mut() = engine.node_dom_paint_facts(text);
        self.tree.set_columns(
            text_node,
            super::RowColumns {
                style_record: 0,
                style_node: Some(text),
                unique_node_id: 0,
                ..Default::default()
            },
        );
        if needs_style_wrapper {
            // NB: The children of the wrapper are derived as the commit attaches them, as the wrapper's style is.
            let wrapper = self.tree.push(PlanNode::new(
                NodeKind::InlineNode,
                node_facts::construction_flags(NodeKind::InlineNode, true, 0) | NodeFlag::ChildrenAreInline as u32,
                box_facts_of_display(FfiDisplay::outside_and_inside(
                    crate::css::css_enums::display_outside::INLINE,
                    crate::css::css_enums::display_inside::FLOW,
                    false,
                )) | box_fact::HAS_STYLE
                    | box_fact::OWN_CHILDREN_DERIVED_ON_COMMIT,
            ));
            self.place(parent, wrapper, true, ChildPosition::Append);
            self.commit_one_by_one(
                wrapper,
                OneByOneRowSource::DisplayContentsTextWrapper {
                    text,
                    record: facts.style_record,
                },
            );
            self.tree.attach(wrapper, text_node, NodeSlotId::INVALID);
        } else {
            self.place(parent, text_node, true, ChildPosition::Append);
        }
        self.list_text_row(text_node, text);
        self.note_text_with_box(text);
    }

    /// Plans `text`, an entry of the child list the walk plans again that renders, as the box it has, if the walk can keep
    /// it (see [`Self::keepable_row`]), and answers whether it did. A text whose box a `::first-letter` or a writing
    /// suggestion split is built again, which splits it again.
    fn plan_kept_text(&mut self, text: StyleNodeID, needs_style_wrapper: bool) -> bool {
        let Some((row, entry_top)) = self.keepable_row(text) else {
            return false;
        };
        let arena = self.arena;
        if arena.text_has_source_range(row) || arena.data(row).kind.get() != NodeKind::TextNode {
            return false;
        }
        let has_style_wrapper = entry_top != row;
        if has_style_wrapper != needs_style_wrapper {
            assert!(
                !self.unit.is_some_and(|unit| unit.verifies_kept_boxes),
                "the kept layout box of {text:?} is not in the style wrapper a fresh build makes"
            );
            return false;
        }
        let top = arena.data(entry_top);
        let facts = if has_style_wrapper {
            box_facts_of_display(FfiDisplay::outside_and_inside(
                crate::css::css_enums::display_outside::INLINE,
                crate::css::css_enums::display_inside::FLOW,
                false,
            )) | box_fact::HAS_STYLE
                | box_fact::OWN_CHILDREN_DERIVED_ON_COMMIT
        } else {
            box_fact::INLINE_OUTSIDE
        };
        let node = PlanNode {
            style: top.style.get(),
            ..PlanNode::new(top.kind.get(), top.flags.get(), facts)
        };
        self.place_kept_row(entry_top, node, false);
        self.note_text_with_box(text);
        true
    }

    /// Places a box in the child list of `parent`, by the insertion rules of [`super::child_list`].
    fn place(&mut self, parent: NodeSlotId, node: NodeSlotId, is_inline_outside: bool, position: ChildPosition) {
        self.tree
            .insert_node_into_inline_or_block_ancestor(parent, node, is_inline_outside, position);
    }

    /// Whether a flat-tree ancestor of the element keeps it out of the rendered tree. Only an element ever hides a
    /// subtree, and no record at all means the style update pass skipped a display:none subtree.
    fn has_unrendered_flat_tree_ancestor(&self, element: StyleNodeID) -> bool {
        let mut ancestor = self.arena.flat_tree_parent(element);
        while let Some(current) = ancestor {
            if !self
                .engine
                .element_published_box_facts(current)
                .is_some_and(|facts| !facts.display.is_none())
            {
                return true;
            }
            ancestor = self.arena.flat_tree_parent(current);
        }
        false
    }

    /// Whether the element published a record for a highlight pseudo-element.
    fn holds_highlight_style(&self, element: StyleNodeID) -> bool {
        element_holds_highlight_style(self.engine, element)
    }

    /// A text's contribution to what comes before later white space texts among its layout parent's boxes.
    fn text_preceding_content(&self, text: StyleNodeID, has_box: bool) -> Option<PrecedingContent> {
        let engine = self.engine;
        let (is_empty, ends_in_white_space) = engine.text_data_ends(text);
        let is_collapsing_whitespace =
            engine.text_is_ascii_whitespace(text) && !is_empty && engine.text_collapses_white_space(text) == Some(true);
        if is_collapsing_whitespace {
            return Some(PrecedingContent::Text {
                ends_in_collapsible_white_space: true,
            });
        }
        if !has_box {
            return None;
        }
        Some(PrecedingContent::Text {
            ends_in_collapsible_white_space: ends_in_white_space
                && engine.text_collapses_white_space(text) == Some(true),
        })
    }

    /// Notes that the text node `text` gets no box, which clears what it held. A text that holds no box and no layout
    /// tree update mark holds nothing to clear.
    fn note_stale_text(&mut self, text: StyleNodeID) {
        if !self.arena.bound_row(text).is_invalid() || self.arena.layout_tree_update_marks().holds_any(text) {
            self.stale_subtrees.push((text, StaleSubtreeScope::Inclusive));
        }
    }

    fn note_text_with_box(&mut self, text: StyleNodeID) {
        if let Some(content) = self.text_preceding_content(text, true) {
            self.preceding_content.push(content);
        }
    }

    fn note_text_without_box(&mut self, text: StyleNodeID) {
        if let Some(content) = self.text_preceding_content(text, false) {
            self.preceding_content.push(content);
        }
    }

    /// The nearest box before a text among the boxes of `layout_parent`.
    fn preceding_box(&self, layout_parent: NodeSlotId) -> PrecedingBox {
        for content in self.preceding_content[self.preceding_content_start..].iter().rev() {
            match *content {
                PrecedingContent::Text {
                    ends_in_collapsible_white_space,
                } => {
                    return PrecedingBox::Text {
                        ends_in_collapsible_white_space,
                    };
                }
                PrecedingContent::Box(node) => {
                    let data = self.tree.node(node);
                    if crate::layout::tree_builder::node_kind_is_node_with_style(data.kind())
                        && self.tree.is_out_of_flow_child_of(layout_parent, node)
                    {
                        continue;
                    }
                    if data.kind() == NodeKind::BreakNode {
                        return PrecedingBox::LineStart;
                    }
                    if data.is_table_non_root_for_table_fixup() {
                        return PrecedingBox::TableInternal;
                    }
                    if data.has_fact(box_fact::INLINE_OUTSIDE) {
                        return PrecedingBox::InlineLevel;
                    }
                    return PrecedingBox::LineStart;
                }
            }
        }
        PrecedingBox::Nothing
    }

    /// Whether the text node `text`, which holds nothing but white space, gets a box among the children of
    /// `layout_parent`.
    fn whitespace_text_needs_box(&self, layout_parent: NodeSlotId, text: StyleNodeID) -> bool {
        let engine = self.engine;
        let parent = self.tree.node(layout_parent);
        if parent.is_svg_content() {
            // SVG renders text only inside a text content element.
            let mut ancestor = layout_parent;
            loop {
                let node = self.tree.node(ancestor);
                match node.kind() {
                    NodeKind::SVGTextBox | NodeKind::SVGTextPathBox => return true,
                    NodeKind::SVGSVGBox => return false,
                    _ => {}
                }
                if node.parent().is_invalid() {
                    break;
                }
                ancestor = node.parent();
            }
            // Above the plan's root, the ancestors are rows of the tree.
            let mut row = self.arena.data(self.root_row).parent.get();
            while !row.is_invalid() {
                match self.arena.data(row).kind.get() {
                    NodeKind::SVGTextBox | NodeKind::SVGTextPathBox => return true,
                    NodeKind::SVGSVGBox => return false,
                    _ => row = self.arena.data(row).parent.get(),
                }
            }
            return false;
        }
        if engine.text_collapses_white_space(text) != Some(true)
            || crate::layout::tree_builder::text_produces_line_box_fragment_when_empty(
                engine,
                text,
                engine.text_is_in_user_agent_shadow_tree(text),
            )
        {
            return true;
        }
        let preceding = self.preceding_box(layout_parent);
        if parent.has_fact(box_fact::FLEX_OR_GRID_INSIDE | box_fact::MATH_INSIDE) {
            // Only white space that continues a run of text joins an item; a run of nothing but white space is none.
            return preceding
                == PrecedingBox::Text {
                    ends_in_collapsible_white_space: false,
                };
        }
        match preceding {
            // An inline box keeps the white space that starts it, and an inside marker comes before it once it is
            // built, after the children.
            PrecedingBox::Nothing => {
                parent.kind() == NodeKind::InlineNode
                    || (parent.kind() == NodeKind::ListItemBox && parent.has_fact(box_fact::LIST_STYLE_POSITION_INSIDE))
            }
            PrecedingBox::Text {
                ends_in_collapsible_white_space,
            } => !ends_in_collapsible_white_space,
            PrecedingBox::InlineLevel => true,
            PrecedingBox::TableInternal => !parent.has_fact(box_fact::TABULAR_CONTAINER),
            PrecedingBox::LineStart => false,
        }
    }
}

/// Whether the element published a record for a highlight pseudo-element.
/// The box a fresh build makes for a pseudo-element, see [`Planner::pseudo_element_box_shape`].
#[derive(Clone, Copy)]
struct PseudoElementBoxShape {
    kind: NodeKind,
    flags: u32,
    box_facts: u32,
    /// The display of the box, which the commit gives it.
    display: FfiDisplay,
    is_list_item_marker: bool,
    marker_position_is_inside: bool,
    decision: PseudoElementDecision,
}

pub(crate) fn element_holds_highlight_style(engine: &RetainedState, element: StyleNodeID) -> bool {
    crate::painting::selection::HighlightPseudoElement::ALL
        .iter()
        .any(|highlight| {
            engine
                .pseudo_published_style_record(element, highlight.pseudo_kind())
                .is_some()
        })
        || !engine.pseudo_published_custom_highlight_records(element).is_empty()
}

/// What the walk reads of an element to plan its box, before it places anything: what the style mirror published for
/// it, and what its row is stamped with. It depends on nothing the walk decided but whether the walk plans the
/// content of an SVG mask, clip path or pattern, so the walk reads it for the children of a long child list ahead of
/// placing them, on as many threads as it has.
#[derive(Clone, Copy)]
pub(crate) struct PreparedElement {
    type_facts: u32,
    /// What its published style says, or `None` where nothing published a style for the element.
    style: Option<PreparedStyle>,
}

#[derive(Clone, Copy)]
struct PreparedStyle {
    record: u64,
    /// The computed `display`.
    display: FfiDisplay,
    content_visibility: u8,
    may_change_counters: bool,
    box_kind: ElementBoxKind,
    layout_kind: ElementLayoutKind,
    /// The element's principal box, where its display makes one.
    principal_box: Option<PreparedBox>,
}

#[derive(Clone, Copy)]
struct PreparedBox {
    kind: NodeKind,
    construction_facts: u32,
    publishes_no_pseudo_element_records: bool,
    has_style_containment: bool,
    flags: u32,
    /// See [`box_fact`].
    box_facts: u32,
    payloads: crate::layout::node_data::LayoutStyleRef,
    /// See `planned_style_facts_for_kind`.
    style_facts: u32,
    /// Whether the element has no box bound to it.
    is_unbound: bool,
    /// What the walk stamps the box's row with, unless the builder's own steps stamp it.
    stamped_row: Option<StampedRow>,
}

/// What an element's row is stamped with besides what its plan node holds.
#[derive(Clone, Copy)]
struct StampedRow {
    /// The flags the row's style and scroll offset add.
    flags: u32,
    dom_paint_facts: u8,
    spans: crate::css::style::tree::TableSpans,
    unique_node_id: i64,
}

/// What decides whether the walk keeps an element's box: it keeps a box whose shape is the one a fresh build gives.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct BoxShape {
    kind: NodeKind,
    /// See [`NodeFlag::CONSTRUCTION`].
    flags: u32,
    /// See [`used_display_of_box`].
    display: FfiDisplay,
}

/// The shape of the principal box a fresh build makes from `style`, if any.
fn fresh_box_shape(style: &PreparedStyle) -> Option<BoxShape> {
    let principal_box = style.principal_box.as_ref()?;
    Some(BoxShape {
        kind: principal_box.kind,
        flags: principal_box.flags,
        display: used_display_of_box(principal_box.kind, principal_box.flags, style.display),
    })
}

/// The shape of the box `row` as the style it shows makes it.
fn row_box_shape(arena: &LayoutNodeArena, row: NodeSlotId) -> Option<BoxShape> {
    let data = arena.data(row);
    let (kind, flags) = (data.kind.get(), data.flags.get() & NodeFlag::CONSTRUCTION);
    let display = arena.style_view(row)?.display();
    Some(BoxShape {
        kind,
        flags,
        display: used_display_of_box(kind, flags, display),
    })
}

/// Reads what the walk needs of `element` to plan its box, for a walk that plans the content of an SVG mask or clip
/// path, or of an SVG pattern, as `layout_svg_mask_or_clip_path` and `layout_svg_pattern` say.
fn prepare_element(
    engine: &RetainedState,
    arena: &LayoutNodeArena,
    element: StyleNodeID,
    layout_svg_mask_or_clip_path: bool,
    layout_svg_pattern: bool,
) -> PreparedElement {
    let inputs = engine.element_layout_inputs(element);
    let type_facts = inputs.adjustment_facts;
    let Some(published) = inputs.record else {
        return PreparedElement {
            type_facts,
            style: None,
        };
    };
    let summary = published.summary();
    let record = published.raw;
    let box_kind = inputs.box_kind;
    let published_display = summary.display;
    let mut prepared_style = PreparedStyle {
        record,
        display: published_display,
        content_visibility: summary.content_visibility,
        may_change_counters: summary.may_change_counters,
        box_kind,
        layout_kind: ElementLayoutKind::Normal,
        principal_box: None,
    };
    if published_display.is_none() || published_display.is_contents() {
        return PreparedElement {
            type_facts,
            style: Some(prepared_style),
        };
    }
    let has_content_replacement = summary.content_is_single_image;
    let construction_facts = inputs.construction_facts;
    let uses_button_layout = construction_facts & element_construction_fact::USES_BUTTON_LAYOUT != 0;
    let layout_kind = crate::layout::tree_builder::element_layout_kind(
        has_content_replacement,
        type_facts,
        layout_svg_mask_or_clip_path,
        layout_svg_pattern,
    );
    prepared_style.layout_kind = layout_kind;
    let kind = match layout_kind {
        ElementLayoutKind::ContentReplacement => Some(NodeKind::ImageBox),
        ElementLayoutKind::SvgMask => Some(NodeKind::SVGMaskBox),
        ElementLayoutKind::SvgClipPath => Some(NodeKind::SVGClipBox),
        ElementLayoutKind::SvgPattern => Some(NodeKind::SVGPatternBox),
        ElementLayoutKind::Normal => {
            match node_kind_for_element_box_kind(box_kind, published_display, summary.appearance) {
                Some(NodeKind::ListItemBox) if uses_button_layout => Some(NodeKind::BlockContainer),
                kind => kind,
            }
        }
    };
    let Some(kind) = kind else {
        return PreparedElement {
            type_facts,
            style: Some(prepared_style),
        };
    };
    let publishes_no_pseudo_element_records = engine.publishes_no_pseudo_element_records(element);
    let holds_highlight_style = !publishes_no_pseudo_element_records && element_holds_highlight_style(engine, element);
    let groups = published.payloads;
    let mut flags = node_facts::construction_flags(kind, false, construction_facts);
    // A media element renders the children of its shadow root, such as its controls.
    if matches!(kind, NodeKind::AudioBox | NodeKind::VideoBox) && engine.shadow_root_of(element).is_some() {
        flags |= NodeFlag::ReplacedBoxCanHaveChildren as u32;
    }
    let mut box_facts = summary.box_facts;
    let display = used_display_of_box(kind, flags, published_display);
    box_facts = (box_facts & !box_fact::OF_DISPLAY) | box_facts_of_display(display);
    box_facts |= item_context_facts(kind, flags, display);
    // NB: A row of a kind its element asks for itself, or whose display the build adjusts, is made by the builder's
    //     own steps, which the planned row's facts do not cover.
    if box_kind != ElementBoxKind::FromDisplay
        || layout_kind != ElementLayoutKind::Normal
        || display != published_display
        || flags & NodeFlag::IsReplacedElement as u32 != 0
    {
        box_facts |= box_fact::STAMPED_BY_BUILDER;
    }
    // NB: The builder's own steps adjust a replaced element's display, which its children are derived from.
    if box_facts & box_fact::STAMPED_BY_BUILDER != 0 && flags & NodeFlag::IsReplacedElement as u32 != 0 {
        box_facts |= box_fact::OWN_CHILDREN_DERIVED_ON_COMMIT;
    }
    if has_content_replacement {
        box_facts |= box_fact::OWNS_CONTENT_REPLACEMENT_IMAGE;
    }
    if holds_highlight_style {
        box_facts |= box_fact::HOLDS_HIGHLIGHT_STYLE;
    }
    let style_facts = summary.style_facts(kind);
    let stamped_row = (box_facts & box_fact::STAMPED_BY_BUILDER == 0).then(|| {
        let mut flags = crate::layout::layout_node_arena::planned_style_flags(style_facts);
        if arena.style_node_has_scroll_offset(element) {
            flags |= NodeFlag::HasScrollOffset as u32;
        }
        StampedRow {
            flags,
            dom_paint_facts: engine.node_dom_paint_facts(element),
            spans: engine.element_table_spans(element),
            unique_node_id: engine.element_unique_node_id(element),
        }
    });
    prepared_style.principal_box = Some(PreparedBox {
        kind,
        construction_facts,
        publishes_no_pseudo_element_records,
        has_style_containment: summary.has_style_containment,
        flags,
        box_facts,
        payloads: crate::layout::node_data::LayoutStyleRef::of_groups(groups),
        style_facts,
        is_unbound: arena.bound_row(element).is_invalid(),
        stamped_row,
    });
    PreparedElement {
        type_facts,
        style: Some(prepared_style),
    }
}

/// Whether a box of `kind`, with `flags`, holds the boxes of its children. See `node_facts::node_can_have_children`.
fn kind_can_have_children(kind: NodeKind, flags: u32) -> bool {
    match kind {
        NodeKind::BreakNode => false,
        NodeKind::AudioBox | NodeKind::VideoBox => flags & NodeFlag::ReplacedBoxCanHaveChildren as u32 != 0,
        NodeKind::SVGSVGBox => true,
        kind if node_facts::kind_is_replaced_box(kind) => false,
        _ => true,
    }
}

/// Whether the box of an element can hold the boxes of its ::before and ::after pseudo-elements. A form control whose
/// content the user agent supplies holds none: a text control, a range input, an input element shown as a button, or a
/// select.
fn box_can_have_generated_children(kind: NodeKind, flags: u32, construction_facts: u32) -> bool {
    if matches!(
        kind,
        NodeKind::TextInputBox | NodeKind::TextAreaBox | NodeKind::RangeInputBox
    ) {
        return false;
    }
    if flags & NodeFlag::IsHtmlInputElement as u32 != 0 && flags & NodeFlag::UsesButtonLayout as u32 != 0 {
        return false;
    }
    construction_facts & element_construction_fact::IS_HTML_SELECT_ELEMENT == 0
}

/// Whether the children of a box of `kind` are its flex or grid items, as `formatting_context_type_created_by_node_data`
/// decides it for a box whose display is flex or grid.
fn item_context_facts(kind: NodeKind, flags: u32, display: FfiDisplay) -> u32 {
    if !(display.is_flex_inside() || display.is_grid_inside())
        || matches!(
            kind,
            NodeKind::SVGSVGBox | NodeKind::FrameSetBox | NodeKind::FieldSetBox
        )
        || node_facts::kind_is_replaced_box(kind)
        || !kind_can_have_children(kind, flags)
    {
        return 0;
    }
    if display.is_flex_inside() {
        box_fact::ESTABLISHES_FLEX_ITEMS
    } else {
        box_fact::ESTABLISHES_GRID_ITEMS
    }
}

/// The boxes of a job.
struct JobSpec {
    roots: Vec<JobRoot>,
}

/// A box of a job: where it goes in the deferring walk's plan, its slot, what its stand-in row is, the ancestor facts
/// and MathML layout of the box, which the job derives its children's facts from, and how many of its children count
/// as inline-level or in flow, and as block-level and in flow.
struct JobRoot {
    root: DeferredRoot,
    stand_in: PlanNode,
    ancestor_facts: u8,
    math_layout_kind: MathLayoutKind,
}

/// What the jobs of one build take from the walk that made them.
struct JobShared<'a> {
    claims: super::ChunkClaims,
    fork_budget: Option<ForkBudget>,
    writing_suggestion_parent: Option<StyleNodeID>,
    enclosing_ancestor_rows: &'a [NodeSlotId],
    root_row: NodeSlotId,
    /// The style of the box the build plans the descendants of, which the row standing in for it in a job shows.
    root_style: Option<ComputedValuesView<'a>>,
    /// The jobs of the build handed to the style layout threads, which the thread that started the build runs too
    /// once its own are done (see [`Planner::run_jobs_left`]).
    handed_on: std::sync::Mutex<std::collections::VecDeque<std::sync::Arc<JobSlot>>>,
    progress: JobProgress,
    /// Where the walks of the build name the counters they find, see `counter_walk.rs`.
    counter_ids: std::sync::Arc<std::sync::atomic::AtomicU32>,
    /// Whether the walks list the owners that a build before stored a counters set for which they no longer need: none
    /// where the build resolves every owner of the document, or where no owner has a set.
    removes_stale_counters_sets: bool,
    /// See `CounterWalk::resolves_counters`.
    resolves_counters: bool,
}

/// How many children a child list has at least for the walk to plan it in runs, how many children a run has at least,
/// and how far the walk looks for a child a run may start with.
const MINIMUM_CHILDREN_TO_PLAN_IN_RUNS: usize = 512;
const MINIMUM_CHILDREN_PER_RUN: usize = 128;
const RUN_START_SEARCH_LENGTH: usize = 32;

/// How many children of a long child list one task reads ahead, and how many the walk reads ahead before it places them.
const PREPARED_CHILDREN_PER_TASK: usize = 128;
const PREPARED_CHILDREN_PER_WINDOW: usize = 4096;

/// How the walk of a build that makes jobs splits child lists: how many children a list has at least for each way of
/// splitting it, and how big the pieces are.
#[derive(Clone, Copy)]
struct ListSplits {
    /// See [`ForkBudget::MINIMUM_CHILDREN_TO_SPLIT`] and [`ForkBudget::MINIMUM_ROOTS_PER_JOB`].
    children_to_split: usize,
    minimum_roots_per_job: usize,
    /// See [`MINIMUM_CHILDREN_TO_PLAN_IN_RUNS`] and [`MINIMUM_CHILDREN_PER_RUN`].
    children_to_plan_in_runs: usize,
    children_per_run: usize,
    /// See [`PREPARED_CHILDREN_PER_TASK`] and [`PREPARED_CHILDREN_PER_WINDOW`].
    prepared_children_per_task: usize,
    prepared_children_per_window: usize,
}

impl ListSplits {
    const DEFAULT: Self = Self {
        children_to_split: ForkBudget::MINIMUM_CHILDREN_TO_SPLIT,
        minimum_roots_per_job: ForkBudget::MINIMUM_ROOTS_PER_JOB,
        children_to_plan_in_runs: MINIMUM_CHILDREN_TO_PLAN_IN_RUNS,
        children_per_run: MINIMUM_CHILDREN_PER_RUN,
        prepared_children_per_task: PREPARED_CHILDREN_PER_TASK,
        prepared_children_per_window: PREPARED_CHILDREN_PER_WINDOW,
    };

    /// NB: Where the document forces parallel layout (see [`LayoutNodeArena::forces_parallel_layout`]), each way of
    ///     splitting a list takes the fewest children it can split, so that tests of a few dozen elements reach it.
    const FORCED: Self = Self {
        children_to_split: 2,
        minimum_roots_per_job: 1,
        children_to_plan_in_runs: 8,
        children_per_run: 2,
        prepared_children_per_task: 2,
        prepared_children_per_window: 8,
    };

    /// How many children a child list has at least for the walk to plan it from a list of them: read ahead in windows,
    /// or in runs.
    fn children_of_a_long_list(&self) -> usize {
        self.prepared_children_per_task * 2
    }
}
