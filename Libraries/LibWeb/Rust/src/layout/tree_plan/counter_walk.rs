/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS counters a layout tree build resolves while it plans the boxes, on whichever thread plans them.
//!
//! Which counters an owner (an element or pseudo-element that takes part in counter resolution) holds depends only on
//! the owners above it and the own steps of the siblings before it, never on counter values or on what the descendants
//! of earlier siblings do: a counter an owner instantiates reaches only its following siblings and their descendants.
//! So the walk that plans the boxes decides the counters of every owner it plans as it goes, a job included, from what
//! the walk that placed the job's boxes hands it.
//!
//! The values are another matter: an owner's value comes from the owner immediately before it in tree order, which may
//! be in the subtree another job plans at the same time. The walk therefore only records what changes a value, on a
//! tape in tree order, with a hole where a job plans a subtree. Once its jobs ran, a job replays its tape with what each
//! of its jobs changed of the counters it was handed, and hands on what it changed itself. The values the counters of a
//! job have where its subtree starts are known once the thread that started the build replayed its own tape, and are
//! handed down the jobs from there; the sets the build stores are then resolved from them.
//!
//! The owners of the top layer and of SVG resource content, which are planned away from their place in tree order, are
//! resolved one at a time after the commit (see `PlanEvent::ResolveCounters`).

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_string::CssString;
use crate::css::style::RetainedState;
use crate::layout::counters::{
    Counter, CounterOwner, CountersSet, CountersSets, LIST_ITEM_COUNTER_NAME, reversed_counter_start_value,
    style_has_implicit_list_item_increment,
};
use crate::layout::layout_node_arena::LayoutNodeArena;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};

/// A change of a counter value that composes with the changes after it into one: `value -> clamp(value + add, lo,
/// hi)`. Incrementing by `n` is `{n, MIN, MAX}`, as the value saturates at the limits of a counter value, and setting
/// to `v` is `{0, v, v}`.
// NB: A plain saturating addition does not compose: once one addition pinned a value at a limit, the next moves it
//     back from there, which no single addition does. A clamp of a clamp is a clamp of the clamped bounds, so this
//     composes exactly, in any grouping.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ClampShift {
    add: i64,
    lo: i32,
    hi: i32,
}

impl ClampShift {
    /// How far `add` goes: past `2^33` every counter value plus it is beyond a limit already, which then pins it.
    const ADD_LIMIT: i64 = 1 << 40;

    pub(crate) const IDENTITY: Self = Self {
        add: 0,
        lo: i32::MIN,
        hi: i32::MAX,
    };

    pub(crate) fn increment(amount: i32) -> Self {
        Self {
            add: amount.into(),
            ..Self::IDENTITY
        }
    }

    pub(crate) fn set(value: i32) -> Self {
        Self {
            add: 0,
            lo: value,
            hi: value,
        }
    }

    /// This change, then `next`.
    pub(crate) fn then(self, next: Self) -> Self {
        let bound = |value: i32| (i64::from(value) + next.add).clamp(next.lo.into(), next.hi.into()) as i32;
        Self {
            add: (self.add + next.add).clamp(-Self::ADD_LIMIT, Self::ADD_LIMIT),
            lo: bound(self.lo),
            hi: bound(self.hi),
        }
    }

    pub(crate) fn apply(self, value: i32) -> i32 {
        (i64::from(value) + self.add).clamp(self.lo.into(), self.hi.into()) as i32
    }
}

/// One counter of an owner's set as the walk knows it: which counter it is, by its name and creator, and the build's
/// name for it, which its value goes by on the tape.
#[derive(Clone, Debug)]
pub(crate) struct WalkCounter {
    name: CssString,
    creator: CounterOwner,
    reversed: bool,
    id: u32,
}

/// The counters of an owner in order, outermost first per name, or none.
pub(crate) type WalkSet = Option<Arc<Vec<WalkCounter>>>;

fn counters_of(set: &WalkSet) -> &[WalkCounter] {
    set.as_deref().map_or(&[], Vec::as_slice)
}

fn same_set(a: &WalkSet, b: &WalkSet) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(a), Some(b)) => Arc::ptr_eq(a, b),
        _ => false,
    }
}

/// What a job's walk starts from below one of its boxes: the counters of the owner whose children it plans, and the
/// counters the siblings before them instantiated, which a run of a long child list starts with.
#[derive(Clone, Default)]
pub(crate) struct CounterFrame {
    set: WalkSet,
    /// For a run of a child list, what its children start from besides the owner's set. The values may have changed
    /// since the owner where a run starts.
    run: Option<Box<RunFrame>>,
}

#[derive(Clone)]
struct RunFrame {
    layer: Vec<WalkCounter>,
    /// The names the walk that split the list gave the counters the run's children instantiate for the siblings after
    /// them, by name and creator.
    sibling_counter_ids: Vec<(CssString, CounterOwner, u32)>,
}

/// The value of a counter at a point of a job's tape: known, or a change of the value it had where the job's subtree
/// started.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Value {
    Known(i32),
    AtEntry { entry: u32, shift: ClampShift },
}

impl Value {
    fn then(self, shift: ClampShift) -> Self {
        match self {
            Self::Known(value) => Self::Known(shift.apply(value)),
            Self::AtEntry { entry, shift: before } => Self::AtEntry {
                entry,
                shift: before.then(shift),
            },
        }
    }

    fn resolve(self, entry_values: &[i32]) -> i32 {
        match self {
            Self::Known(value) => value,
            Self::AtEntry { entry, shift } => shift.apply(entry_values[entry as usize]),
        }
    }
}

#[derive(Clone, Copy, Debug)]
enum TapeEntry {
    /// The counter `id` is instantiated with `value`.
    Create {
        id: u32,
        value: i32,
    },
    Change {
        id: u32,
        shift: ClampShift,
    },
    /// Root `root` of job `job` plans the subtree that goes here.
    Hole {
        job: u32,
        root: u32,
    },
    Snapshot(u32),
}

/// The set an owner's set is stored apart from unless it is the same: that of its parent element.
#[derive(Clone, Copy, Debug)]
enum Compare {
    /// The set of the owner whose children a root of the job plans.
    Root(u32),
    Snapshot(u32),
}

/// An owner whose set the build stores, unless it turns out to be its parent's.
struct Snapshot {
    owner: CounterOwner,
    set: Arc<Vec<WalkCounter>>,
    compare: Compare,
    /// The values of the counters of `set`, once the job replayed its tape.
    values: Vec<Value>,
}

/// A box below which a job's walk plans, as far as counters go.
struct RootCounters {
    /// The counters the walk started from, as a range of `CounterPlan::entries`.
    entries: std::ops::Range<usize>,
    /// The set the owners at the top of the walk compare theirs with: that of their parent.
    compare_set: WalkSet,
    /// The values the counters the walk started from have, where they are known as it starts.
    seed: Option<Vec<i32>>,
    /// The set the owners at the top of the walk compare theirs with, where its values are not those the walk starts
    /// with: the set of the owner whose child list a run of the walk is, or that the builds before stored for the parent
    /// of the owner whose child list is planned again. Known once the build resolved the walk that made the job.
    resolved_compare_set: Option<Option<CountersSet>>,
    tape: std::ops::Range<usize>,
    /// For a run of a child list, the counters its children instantiated that its siblings after it hold.
    final_layer: Vec<WalkCounter>,
    /// Whether the root is a run of a child list whose children instantiate counters the siblings after it hold.
    hands_on_sibling_counters: bool,
    /// What the job's subtree below the box changed of the counters it started from, once replayed.
    changes: Vec<(u32, ClampShift)>,
    /// The values of the counters of `final_layer` the run instantiated, once replayed.
    sibling_counter_values: Vec<(u32, i32)>,
}

/// What a walk found out about counters, which the job replays once its jobs ran.
#[derive(Default)]
pub(crate) struct CounterPlan {
    tape: Vec<TapeEntry>,
    roots: Vec<RootCounters>,
    /// The counters each root's walk started from, by name.
    entries: Vec<u32>,
    /// The values of `entries` once the build knows them.
    entry_values: Vec<i32>,
    snapshots: Vec<Snapshot>,
    /// The owners the walk resolved a set the same as their parent's for, which a build before stored a set for.
    removals: Vec<CounterOwner>,
    /// The values the counters a job's root starts from have at the job's hole, in the order of the holes on the tape,
    /// as `(job, root, values)`.
    holes: Vec<(u32, u32, std::ops::Range<usize>)>,
    /// The set the owners at the top of the walk of each hole's root compare theirs with, where that root starts a run:
    /// the set of the owner whose child list the walk was in, in the order of the holes.
    hole_compares: Vec<Option<Compare>>,
    hole_values: Vec<Value>,
}

/// The sets of the owners a build resolved, for the arena to store.
#[derive(Default)]
pub(crate) struct ResolvedCounters {
    /// Whether every set the arena holds goes first: the build resolved every owner of the document.
    pub(crate) forgets_every_set: bool,
    pub(crate) sets: Vec<(CounterOwner, Option<CountersSet>)>,
}

/// The owners whose children a walk is among, innermost last.
struct Level {
    /// The set of the owner.
    set: WalkSet,
    /// The counters the siblings before the next child instantiated, whose names `set` lacks.
    layer: Vec<WalkCounter>,
    /// `set` and then `layer`: what the next child inherits, once asked for.
    inherited: Option<WalkSet>,
    compare: Compare,
    /// How many changes the tape had at the owner.
    changes_at_owner: u64,
    /// Whether the values may have changed between the owner and where the walk started, see
    /// [`CounterFrame::run`].
    values_changed_before_walk: bool,
    /// For the top of the walk of a run, see [`CounterFrame::sibling_counter_ids`].
    sibling_counter_ids: Vec<(CssString, CounterOwner, u32)>,
    /// The last of the children the walk kept since it placed one, see [`CounterWalk::keep_element`].
    kept: Option<CounterOwner>,
}

impl Level {
    fn inherited(&mut self) -> WalkSet {
        if self.inherited.is_none() {
            self.inherited = Some(if self.layer.is_empty() {
                self.set.clone()
            } else {
                Some(Arc::new(
                    counters_of(&self.set).iter().chain(&self.layer).cloned().collect(),
                ))
            });
        }
        self.inherited.clone().expect("the inherited set was just made")
    }
}

/// What the children of an owner the walk placed start from.
#[derive(Clone)]
pub(crate) struct PlacedOwner {
    pub(crate) set: WalkSet,
    compare: Compare,
    changes_at_owner: u64,
}

impl PlacedOwner {
    /// Where the values of the owner's counters are found once the build resolved them.
    pub(crate) fn source(&self) -> CounterSource {
        CounterSource(self.compare)
    }

    /// What a job hands the walk below the owner.
    pub(crate) fn frame(&self) -> CounterFrame {
        CounterFrame {
            set: self.set.clone(),
            run: None,
        }
    }
}

/// Where the values of the counters of an owner the walk placed are found: the snapshot the walk took of the owner, or
/// of the nearest owner above it whose counters have the same values, or the set the walk started from.
#[derive(Clone, Copy, Debug)]
pub(crate) struct CounterSource(Compare);

/// The counters part of the state of a walk.
pub(crate) struct CounterWalk {
    plan: CounterPlan,
    levels: Vec<Level>,
    /// How many changes and holes the tape has.
    changes: u64,
    /// Where the next counter's name comes from, shared by the jobs of the build.
    ids: Arc<AtomicU32>,
    list_item_name: CssString,
    /// Whether the walk lists owners that a build before stored a set for which it no longer needs.
    removes_stale_sets: bool,
    /// Whether any owner may hold a counter: some record may change one, or a build before stored a set. A walk that
    /// resolves none records nothing.
    resolves_counters: bool,
}

impl CounterWalk {
    pub(crate) fn new(ids: Arc<AtomicU32>, removes_stale_sets: bool, resolves_counters: bool) -> Self {
        Self {
            plan: CounterPlan::default(),
            levels: Vec::new(),
            changes: 0,
            ids,
            list_item_name: CssString::from_utf16(&LIST_ITEM_COUNTER_NAME),
            removes_stale_sets,
            resolves_counters,
        }
    }

    pub(crate) fn resolves_counters(&self) -> bool {
        self.resolves_counters
    }

    pub(crate) fn into_plan(self) -> CounterPlan {
        debug_assert!(self.levels.is_empty(), "every level of the walk was left");
        self.plan
    }

    fn new_id(&self) -> u32 {
        self.ids.fetch_add(1, Ordering::Relaxed)
    }

    fn record(&mut self, entry: TapeEntry) {
        self.changes += 1;
        self.plan.tape.push(entry);
    }

    /// Starts the walk below a box, from `frame`.
    pub(crate) fn start_root(&mut self, frame: &CounterFrame) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        let (layer, sibling_counter_ids) = frame.run.as_deref().map_or_else(Default::default, |run| {
            (run.layer.clone(), run.sibling_counter_ids.clone())
        });
        self.start_root_with(frame.set.clone(), layer, None, sibling_counter_ids, frame.run.is_some());
    }

    fn start_root_with(
        &mut self,
        set: WalkSet,
        layer: Vec<WalkCounter>,
        seed: Option<Vec<i32>>,
        sibling_counter_ids: Vec<(CssString, CounterOwner, u32)>,
        values_changed_before_walk: bool,
    ) {
        let start = self.plan.entries.len();
        self.plan
            .entries
            .extend(counters_of(&set).iter().chain(&layer).map(|counter| counter.id));
        let root = self.plan.roots.len() as u32;
        self.plan.roots.push(RootCounters {
            entries: start..self.plan.entries.len(),
            compare_set: set.clone(),
            seed,
            resolved_compare_set: None,
            tape: self.plan.tape.len()..self.plan.tape.len(),
            final_layer: Vec::new(),
            hands_on_sibling_counters: !sibling_counter_ids.is_empty(),
            changes: Vec::new(),
            sibling_counter_values: Vec::new(),
        });
        self.levels.push(Level {
            set,
            layer,
            inherited: None,
            compare: Compare::Root(root),
            changes_at_owner: self.changes,
            values_changed_before_walk,
            sibling_counter_ids,
            kept: None,
        });
    }

    /// Starts the walk of a child list planned again, which the boxes of the builds before surround: below `owner`, an
    /// element, from what the builds before resolved for the owners before it.
    pub(crate) fn start_root_after_retained(
        &mut self,
        arena: &LayoutNodeArena,
        engine: &RetainedState,
        sets: &CountersSets,
        owner: CounterOwner,
    ) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        // OPTIMIZATION: No owner has a counter, so there is none to inherit.
        if sets.is_empty() {
            self.start_root_with(None, Vec::new(), Some(Vec::new()), Vec::new(), true);
            self.plan
                .roots
                .last_mut()
                .expect("the root was just started")
                .resolved_compare_set = Some(None);
            return;
        }
        let (counters, parent_count, parent_set) =
            crate::layout::counters::retained_inherited_counters(arena, engine, sets, owner);
        let walk_counters: Vec<WalkCounter> = counters
            .iter()
            .map(|counter| WalkCounter {
                name: counter.name().clone(),
                creator: counter.originating_element(),
                reversed: counter.reversed(),
                id: self.new_id(),
            })
            .collect();
        let seed = counters.iter().map(Counter::value).collect();
        let set = (parent_count != 0).then(|| Arc::new(walk_counters[..parent_count].to_vec()));
        // NB: The values are those of the owner immediately before, which the siblings before may have changed.
        self.start_root_with(
            set,
            walk_counters[parent_count..].to_vec(),
            Some(seed),
            Vec::new(),
            true,
        );
        self.plan
            .roots
            .last_mut()
            .expect("the root was just started")
            .resolved_compare_set = Some(parent_set);
    }

    pub(crate) fn finish_root(&mut self) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        let level = self.levels.pop().expect("a root's walk started");
        let root = self.plan.roots.last_mut().expect("a root's walk started");
        root.tape.end = self.plan.tape.len();
        root.final_layer = level.layer;
    }

    /// Resolves the counters of `owner`, whose style is `style`, the next child of the owner whose children the walk is
    /// among. `uses` names the counters a pseudo-element's content uses, which it instantiates where it has none.
    pub(crate) fn place_owner(
        &mut self,
        engine: &RetainedState,
        owner: CounterOwner,
        style: Option<ComputedValuesView<'_>>,
        uses: &[Vec<u16>],
        arena: &LayoutNodeArena,
        sets: &CountersSets,
    ) -> PlacedOwner {
        // OPTIMIZATION: Where no owner holds a counter and no record changes one, every set is empty. A counter the
        //               content of a pseudo-element uses is instantiated as its content is made.
        if !self.resolves_counters {
            return PlacedOwner {
                set: None,
                compare: Compare::Root(0),
                changes_at_owner: 0,
            };
        }
        self.settle_kept_element(arena, engine, sets);
        let level = self.levels.last_mut().expect("an owner is placed below an owner");
        let inherited = level.inherited();
        let parent_set = level.set.clone();
        let mut step = OwnStep {
            walk: self,
            parent_count: counters_of(&parent_set).len(),
            counters: None,
            inherited: &inherited,
            owner,
            records: true,
        };
        if let Some(style) = style {
            step.run(engine, style, uses);
        }
        let created = step.counters.take();
        let changes = self.changes;
        let level = self.levels.last_mut().expect("an owner is placed below an owner");
        let set = match created {
            Some(counters) => {
                // NB: The siblings after the owner hold the counters it holds that its parent's set has no counter of
                //     the same name of.
                let parent_counters = counters_of(&parent_set);
                level.layer = counters[parent_counters.len()..]
                    .iter()
                    .filter(|counter| {
                        !parent_counters
                            .iter()
                            .any(|parent_counter| parent_counter.name == counter.name)
                    })
                    .cloned()
                    .collect();
                level.inherited = None;
                Some(Arc::new(counters))
            }
            None => inherited,
        };
        let takes_snapshot = set.is_some()
            && (!same_set(&set, &parent_set) || level.values_changed_before_walk || changes > level.changes_at_owner);
        let compare = if takes_snapshot {
            let snapshot = self.plan.snapshots.len() as u32;
            self.plan.snapshots.push(Snapshot {
                owner,
                set: set.clone().expect("a snapshot is of a set"),
                compare: level.compare,
                values: Vec::new(),
            });
            self.plan.tape.push(TapeEntry::Snapshot(snapshot));
            Compare::Snapshot(snapshot)
        } else {
            if self.removes_stale_sets && sets.holds_set_of(owner) {
                self.plan.removals.push(owner);
            }
            level.compare
        };
        PlacedOwner {
            set,
            compare,
            changes_at_owner: changes,
        }
    }

    /// Keeps `element`, a child of the owner whose children the walk is among, whose box and the boxes below it the
    /// build keeps: its counters, and the values the counters after it start from, are those the builds before
    /// resolved.
    // OPTIMIZATION: What the kept children after the last owner the walk placed hand on is what the last of them does,
    //               so only that one is read, once the walk places an owner after it.
    pub(crate) fn keep_element(&mut self, element: crate::css::style::tree::StyleNodeID) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        self.levels.last_mut().expect("a kept element is below an owner").kept = Some(CounterOwner::element(element));
    }

    /// Starts the walk of the children of the owner whose children the walk is among after children whose boxes the
    /// build keeps: what they did to the counters is what `preceding`, the nearest owner among them, holds as the
    /// builds before resolved it, so it is kept.
    pub(crate) fn start_children_after_retained(&mut self, preceding: Option<CounterOwner>) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        self.levels.last_mut().expect("the children are below an owner").kept = preceding;
    }

    fn settle_kept_element(&mut self, arena: &LayoutNodeArena, engine: &RetainedState, sets: &CountersSets) {
        let Some(owner) = self.levels.last_mut().and_then(|level| level.kept.take()) else {
            return;
        };
        // OPTIMIZATION: No owner has a counter the walk could read.
        if sets.is_empty() {
            return;
        }
        let kept = sets.retained_counters_of(engine, owner);
        let level = self.levels.last_mut().expect("a kept element is below an owner");
        let inherited = level.inherited();
        let parent_set = level.set.clone();
        let find = |counters: &[WalkCounter], counter: &Counter| {
            counters
                .iter()
                .find(|walked| walked.name == *counter.name() && walked.creator == counter.originating_element())
                .cloned()
        };
        // NB: The counters of the parent's set keep their names, and those the element or the siblings before it
        //     instantiated, which are the walk's own where it planned those siblings, too.
        let mut layer = Vec::new();
        let mut created = Vec::new();
        for counter in kept {
            if counters_of(&parent_set)
                .iter()
                .any(|parent_counter| parent_counter.name == *counter.name())
            {
                continue;
            }
            let walked = find(counters_of(&inherited), counter).unwrap_or_else(|| {
                let walked = WalkCounter {
                    name: counter.name().clone(),
                    creator: counter.originating_element(),
                    reversed: counter.reversed(),
                    id: self.ids.fetch_add(1, Ordering::Relaxed),
                };
                created.push((walked.id, counter.value()));
                walked
            });
            layer.push(walked);
        }
        let level = self.levels.last_mut().expect("a kept element is below an owner");
        level.layer = layer;
        level.inherited = None;
        let following = level.inherited();
        for (id, value) in created {
            self.record(TapeEntry::Create { id, value });
        }
        // NB: A pseudo-element has no children of its own.
        let last = if owner.generated_for == 0 {
            crate::layout::counters::last_owner_in_tree_order_within(arena, engine, owner.element)
        } else {
            owner
        };
        let values = sets.retained_counters_of(engine, last);
        for walked in counters_of(&following) {
            if let Some(counter) = values
                .iter()
                .find(|counter| *counter.name() == walked.name && counter.originating_element() == walked.creator)
            {
                self.record(TapeEntry::Create {
                    id: walked.id,
                    value: counter.value(),
                });
            }
        }
    }

    /// Starts the walk of the children of the owner just placed as `placed`.
    pub(crate) fn enter_children(&mut self, placed: &PlacedOwner) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        self.levels.push(Level {
            set: placed.set.clone(),
            layer: Vec::new(),
            inherited: None,
            compare: placed.compare,
            changes_at_owner: placed.changes_at_owner,
            values_changed_before_walk: false,
            sibling_counter_ids: Vec::new(),
            kept: None,
        });
    }

    pub(crate) fn leave_children(&mut self, arena: &LayoutNodeArena, engine: &RetainedState, sets: &CountersSets) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        // NB: The owners after the owner of the children read what the kept children after the last owner the walk
        //     placed did to the counters they share.
        self.settle_kept_element(arena, engine, sets);
        self.levels.pop().expect("the walk entered the children");
    }

    /// Leaves the subtree root `root` of job `job` plans here, whose walk starts in the child list the walk is in where
    /// `starts_run`, and below the owner the walk placed last otherwise.
    pub(crate) fn hole(&mut self, job: u32, root: u32, starts_run: bool) {
        // OPTIMIZATION: See [`Self::resolves_counters`].
        if !self.resolves_counters {
            return;
        }
        let compare = starts_run.then(|| self.levels.last().expect("a hole is below an owner").compare);
        self.plan.hole_compares.push(compare);
        self.record(TapeEntry::Hole { job, root });
    }

    /// The frames of the runs a long child list of the owner whose children the walk is among is split into, after
    /// the run the walk planned itself: each starts from the counters the children of the runs before instantiated.
    /// `children_of_run` lists the children of each run that may change a counter and take part in counter resolution,
    /// with their styles.
    pub(crate) fn frames_of_runs<'s>(
        &self,
        engine: &RetainedState,
        children_of_run: impl Iterator<Item = Vec<(crate::css::style::tree::StyleNodeID, ComputedValuesView<'s>)>>,
    ) -> Vec<CounterFrame> {
        let level = self.levels.last().expect("a split list is below an owner");
        let parent_set = level.set.clone();
        let mut layer = level.layer.clone();
        let mut frames = Vec::new();
        for children in children_of_run {
            let mut run = RunFrame {
                layer: layer.clone(),
                sibling_counter_ids: Vec::new(),
            };
            for (child, style) in children {
                let inherited: WalkSet = if layer.is_empty() {
                    parent_set.clone()
                } else {
                    Some(Arc::new(
                        counters_of(&parent_set).iter().chain(&layer).cloned().collect(),
                    ))
                };
                let mut scan = CounterWalk {
                    plan: CounterPlan::default(),
                    levels: Vec::new(),
                    changes: 0,
                    ids: self.ids.clone(),
                    list_item_name: self.list_item_name.clone(),
                    removes_stale_sets: false,
                    resolves_counters: true,
                };
                let mut step = OwnStep {
                    walk: &mut scan,
                    parent_count: counters_of(&parent_set).len(),
                    counters: None,
                    inherited: &inherited,
                    owner: CounterOwner::element(child),
                    records: false,
                };
                step.run(engine, style, &[]);
                let Some(counters) = step.counters.take() else {
                    continue;
                };
                layer = counters[counters_of(&parent_set).len()..]
                    .iter()
                    .filter(|counter| {
                        !counters_of(&parent_set)
                            .iter()
                            .any(|parent_counter| parent_counter.name == counter.name)
                    })
                    .cloned()
                    .collect();
                run.sibling_counter_ids.extend(
                    counters
                        .iter()
                        .filter(|counter| counter.creator == CounterOwner::element(child))
                        .map(|counter| (counter.name.clone(), counter.creator, counter.id)),
                );
            }
            frames.push(CounterFrame {
                set: parent_set.clone(),
                run: Some(Box::new(run)),
            });
        }
        frames
    }
}

/// The own step of one owner: what its `counter-reset`, `counter-increment`, `counter-set` and list item do, in the
/// order counter resolution does them, and what the counters its content uses instantiate.
struct OwnStep<'w, 's> {
    walk: &'w mut CounterWalk,
    /// How many of the inherited counters are those of the parent element's set.
    parent_count: usize,
    /// The owner's counters, once a step instantiated one.
    counters: Option<Vec<WalkCounter>>,
    inherited: &'s WalkSet,
    owner: CounterOwner,
    /// Whether the step records what it changes on the tape, as the walk does and a look ahead at the steps of a run's
    /// children does not.
    records: bool,
}

impl OwnStep<'_, '_> {
    fn counters(&self) -> &[WalkCounter] {
        match &self.counters {
            Some(counters) => counters,
            None => counters_of(self.inherited),
        }
    }

    fn innermost(&self, name: &[u16]) -> Option<&WalkCounter> {
        self.counters()
            .iter()
            .rev()
            .find(|counter| counter.name.units() == name)
    }

    fn record(&mut self, entry: TapeEntry) {
        if self.records {
            self.walk.record(entry);
        }
    }

    // https://drafts.csswg.org/css-lists-3/#instantiate-counter
    fn instantiate(&mut self, name: &CssString, reversed: bool, value: i32) {
        let parent_count = self.parent_count;
        let counters = self
            .counters
            .get_or_insert_with(|| counters_of(self.inherited).to_vec());
        // 2. Let innermost counter be the last counter in counters with the name name.
        //    If innermost counter’s originating element is element or a previous sibling of element,
        //    remove innermost counter from counters.
        // NB: The counters after those of the parent element's set are the siblings' before it and its own.
        if let Some(index) = counters.iter().rposition(|counter| counter.name == *name)
            && index >= parent_count
        {
            counters.remove(index);
        }
        // NB: A counter of a run's child that the siblings after the run hold goes by the name the walk that split the
        //     list gave it.
        let id = self
            .walk
            .levels
            .last()
            .and_then(|level| {
                level
                    .sibling_counter_ids
                    .iter()
                    .find(|(sibling_name, creator, _)| *sibling_name == *name && *creator == self.owner)
            })
            .map(|&(_, _, id)| id)
            .unwrap_or_else(|| self.walk.new_id());
        // 3. Append a new counter to counters with name name, originating element element,
        //    reversed being reversed, and initial value value (if given)
        counters.push(WalkCounter {
            name: name.clone(),
            creator: self.owner,
            reversed,
            id,
        });
        self.record(TapeEntry::Create { id, value });
    }

    // https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
    fn increment(&mut self, name: &CssString, amount: i32) {
        if let Some(id) = self.innermost(name.units()).map(|counter| counter.id) {
            self.record(TapeEntry::Change {
                id,
                shift: ClampShift::increment(amount),
            });
            return;
        }
        // If there is not currently a counter of the given name on the element, the element instantiates
        // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
        // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
        self.instantiate(name, false, ClampShift::increment(amount).apply(0));
    }

    // https://drafts.csswg.org/css-lists-3/#propdef-counter-set
    fn set(&mut self, name: &CssString, value: i32) {
        if let Some(id) = self.innermost(name.units()).map(|counter| counter.id) {
            self.record(TapeEntry::Change {
                id,
                shift: ClampShift::set(value),
            });
            return;
        }
        // If there is not currently a counter of the given name on the element, the element instantiates
        // a new counter of the given name with a starting value of 0 before setting or incrementing its value.
        // https://drafts.csswg.org/css-lists-3/#valdef-counter-set-counter-name-integer
        self.instantiate(name, false, value);
    }

    // https://drafts.csswg.org/css-lists-3/#auto-numbering
    fn run(&mut self, engine: &RetainedState, style: ComputedValuesView<'_>, uses: &[Vec<u16>]) {
        // https://drafts.csswg.org/css-lists-3/#counters-without-boxes
        // An element that does not generate a box (for example, an element with display set to none,
        // or a pseudo-element with content set to none) cannot set, reset, or increment a counter.
        // The counter properties are still valid on such an element, but they must have no effect.
        if !style.display().is_none() {
            // 2. New counters are instantiated (counter-reset).
            for counter in style.counter_reset() {
                let mut value = counter.integer();
                if counter.is_reversed() && value.is_none() {
                    value = Some(reversed_counter_start_value(engine, counter.name(), self.owner, style));
                }
                self.instantiate(counter.name(), counter.is_reversed(), value.unwrap_or(0));
            }

            // FIXME: Take style containment into account
            // https://drafts.csswg.org/css-contain-2/#containment-style
            // Giving an element style containment has the following effects:
            // 1. The 'counter-increment' and 'counter-set' properties must be scoped to the element’s sub-tree and create a
            //    new counter.

            // 3. Counter values are incremented (counter-increment).
            for counter in style.counter_increment() {
                self.increment(counter.name(), counter.integer().expect("an increment has a value"));
            }

            if style_has_implicit_list_item_increment(style) {
                let list_item_name = self.walk.list_item_name.clone();
                let reversed = self
                    .innermost(list_item_name.units())
                    .is_some_and(|counter| counter.reversed);
                self.increment(&list_item_name, if reversed { -1 } else { 1 });
            }

            // 4. Counter values are explicitly set (counter-set).
            for counter in style.counter_set() {
                self.set(counter.name(), counter.integer().expect("a counter-set has a value"));
            }

            // 5. Counter values are used (counter()/counters()).
            // "If no counter named <counter-name> exists on an element where counter() or counters() is used,
            // one is first instantiated with a starting value of 0."
            // NB: No counter of this name exists, so instantiating one removes none.
            for name in uses {
                if self.innermost(name).is_none() {
                    self.instantiate(&CssString::from_utf16(name), false, 0);
                }
            }
        }
    }
}

impl CounterPlan {
    /// Replays the tape once the jobs of the walk ran: what each root's subtree changes of the counters it started
    /// from, and the value of every snapshot's counters, in terms of those the roots started from.
    pub(crate) fn replay(&mut self, job_plans: &[Option<&CounterPlan>]) {
        let mut values: crate::css::style::fast_hash::FastMap<u32, Value> = Default::default();
        self.entry_values = vec![0; self.entries.len()];
        for root in 0..self.roots.len() {
            values.clear();
            let entries = self.roots[root].entries.clone();
            for (offset, &id) in self.entries[entries.clone()].iter().enumerate() {
                let value = match &self.roots[root].seed {
                    Some(seed) => Value::Known(seed[offset]),
                    None => Value::AtEntry {
                        entry: (entries.start + offset) as u32,
                        shift: ClampShift::IDENTITY,
                    },
                };
                values.insert(id, value);
            }
            for index in self.roots[root].tape.clone() {
                match self.tape[index] {
                    TapeEntry::Create { id, value } => {
                        values.insert(id, Value::Known(value));
                    }
                    TapeEntry::Change { id, shift } => {
                        let value = values.get_mut(&id).expect("a changed counter is one the walk holds");
                        *value = value.then(shift);
                    }
                    TapeEntry::Hole { job, root: job_root } => {
                        let job_plan = job_plans[job as usize].expect("a job ran before its plan is replayed");
                        let job_root_counters = &job_plan.roots[job_root as usize];
                        let start = self.hole_values.len();
                        for &id in &job_plan.entries[job_root_counters.entries.clone()] {
                            let value = *values.get(&id).expect("a job starts from counters the walk holds");
                            self.hole_values.push(value);
                        }
                        self.holes.push((job, job_root, start..self.hole_values.len()));
                        for &(id, shift) in &job_root_counters.changes {
                            let value = values.get_mut(&id).expect("a job changes counters the walk holds");
                            *value = value.then(shift);
                        }
                        for &(id, value) in &job_root_counters.sibling_counter_values {
                            values.insert(id, Value::Known(value));
                        }
                    }
                    TapeEntry::Snapshot(snapshot) => {
                        let snapshot = &mut self.snapshots[snapshot as usize];
                        snapshot.values = snapshot
                            .set
                            .iter()
                            .map(|counter| *values.get(&counter.id).expect("a set holds counters the walk holds"))
                            .collect();
                    }
                }
            }
            let root_counters = &mut self.roots[root];
            if root_counters.seed.is_none() {
                for (offset, &id) in self.entries[entries.clone()].iter().enumerate() {
                    match values[&id] {
                        Value::AtEntry { entry, shift } => {
                            debug_assert_eq!(entry as usize, entries.start + offset);
                            if shift != ClampShift::IDENTITY {
                                root_counters.changes.push((id, shift));
                            }
                        }
                        Value::Known(_) => unreachable!("only a counter of the walk's own is instantiated"),
                    }
                }
            }
            if !root_counters.hands_on_sibling_counters {
                continue;
            }
            let entry_ids = &self.entries[entries];
            root_counters.sibling_counter_values = root_counters
                .final_layer
                .iter()
                .filter(|counter| !entry_ids.contains(&counter.id))
                .map(|counter| match values[&counter.id] {
                    Value::Known(value) => (counter.id, value),
                    Value::AtEntry { .. } => unreachable!("a counter a run instantiates has a value of its own"),
                })
                .collect();
        }
    }

    /// The counters of `set` with `values` resolved.
    fn resolve_set(&self, set: &[WalkCounter], values: impl Iterator<Item = Value>) -> CountersSet {
        Arc::new(
            set.iter()
                .zip(values)
                .map(|(counter, value)| {
                    Counter::new(
                        counter.name.clone(),
                        counter.creator,
                        counter.reversed,
                        value.resolve(&self.entry_values),
                    )
                })
                .collect(),
        )
    }

    /// The set the owners at the top of the walk of root `root` compare theirs with.
    fn compare_set_of_root(&self, root: usize) -> Option<CountersSet> {
        let root_counters = &self.roots[root];
        if let Some(set) = &root_counters.resolved_compare_set {
            return set.clone();
        }
        // NB: The walk below an owner starts with the values the owner's counters have.
        let counters = counters_of(&root_counters.compare_set);
        if counters.is_empty() {
            return None;
        }
        let entries = &self.entries[root_counters.entries.clone()];
        let values = counters.iter().map(|counter| {
            let offset = entries
                .iter()
                .position(|&id| id == counter.id)
                .expect("the parent's counters are those a root starts from");
            match &root_counters.seed {
                Some(seed) => Value::Known(seed[offset]),
                None => Value::AtEntry {
                    entry: (root_counters.entries.start + offset) as u32,
                    shift: ClampShift::IDENTITY,
                },
            }
        });
        Some(self.resolve_set(counters, values))
    }

    /// Lists the sets of the owners the walk took snapshots of, now that the values the roots start from are known,
    /// and the owners whose sets the arena drops. Answers the sets of the snapshots and of the roots.
    #[allow(clippy::type_complexity)]
    fn resolve_into(
        &self,
        resolved: &mut Vec<(CounterOwner, Option<CountersSet>)>,
    ) -> (Vec<Option<CountersSet>>, Vec<Option<Option<CountersSet>>>) {
        let mut snapshot_sets: Vec<Option<CountersSet>> = Vec::with_capacity(self.snapshots.len());
        let mut root_sets: Vec<Option<Option<CountersSet>>> = vec![None; self.roots.len()];
        for snapshot in &self.snapshots {
            let set = self.resolve_set(&snapshot.set, snapshot.values.iter().copied());
            let compared: Option<CountersSet> = match snapshot.compare {
                Compare::Snapshot(index) => snapshot_sets[index as usize].clone(),
                Compare::Root(root) => root_sets[root as usize]
                    .get_or_insert_with(|| self.compare_set_of_root(root as usize))
                    .clone(),
            };
            let is_parent_set = compared.as_ref().is_some_and(|compared| **compared == *set);
            resolved.push((snapshot.owner, (!is_parent_set).then(|| set.clone())));
            snapshot_sets.push(Some(set));
        }
        resolved.extend(self.removals.iter().map(|&owner| (owner, None)));
        (snapshot_sets, root_sets)
    }
}

impl CounterPlan {
    /// The value of each counter named `name` of the set `source` names, outermost first, once the build resolved the
    /// values the walk started from.
    pub(crate) fn values_of(&self, source: CounterSource, name: &[u16]) -> Vec<i32> {
        match source.0 {
            Compare::Snapshot(index) => {
                let snapshot = &self.snapshots[index as usize];
                snapshot
                    .set
                    .iter()
                    .zip(&snapshot.values)
                    .filter(|(counter, _)| counter.name.units() == name)
                    .map(|(_, value)| value.resolve(&self.entry_values))
                    .collect()
            }
            Compare::Root(root) => {
                let root_counters = &self.roots[root as usize];
                if let Some(set) = &root_counters.resolved_compare_set {
                    return set.as_deref().map_or_else(Vec::new, |set| {
                        set.iter()
                            .filter(|counter| counter.name().units() == name)
                            .map(Counter::value)
                            .collect()
                    });
                }
                let entries = &self.entries[root_counters.entries.clone()];
                counters_of(&root_counters.compare_set)
                    .iter()
                    .filter(|counter| counter.name.units() == name)
                    .map(|counter| {
                        let offset = entries
                            .iter()
                            .position(|&id| id == counter.id)
                            .expect("the parent's counters are those a root starts from");
                        match &root_counters.seed {
                            Some(seed) => seed[offset],
                            None => self.entry_values[root_counters.entries.start + offset],
                        }
                    })
                    .collect()
            }
        }
    }
}

/// Hands each job of `plan` the values the counters it starts from have, from those of the walk that made it, down the
/// jobs, and lists the sets the build stores.
pub(crate) fn resolve_counters_of_build(
    plan: &mut crate::layout::tree_plan::planner::SubtreePlan,
    forgets_every_set: bool,
) -> ResolvedCounters {
    let mut resolved = ResolvedCounters {
        forgets_every_set,
        sets: Vec::new(),
    };
    resolve_plan(plan, &mut resolved.sets);
    resolved
}

fn resolve_plan(
    plan: &mut crate::layout::tree_plan::planner::SubtreePlan,
    resolved: &mut Vec<(CounterOwner, Option<CountersSet>)>,
) {
    let (counters, jobs) = plan.counters_and_jobs_mut();
    let (snapshot_sets, mut root_sets) = counters.resolve_into(resolved);
    for (hole, &(job, root, ref values)) in counters.holes.iter().enumerate() {
        let job_plan = jobs[job as usize]
            .plan
            .as_mut()
            .expect("a job ran before its plan is resolved");
        let job_counters = job_plan.counters_mut();
        let job_root = &mut job_counters.roots[root as usize];
        let start = job_root.entries.start;
        // NB: The owners at the top of a run compare their sets with that of the owner whose child list it is.
        if let Some(compare) = counters.hole_compares[hole] {
            job_root.resolved_compare_set = Some(match compare {
                Compare::Snapshot(index) => snapshot_sets[index as usize].clone(),
                Compare::Root(own_root) => root_sets[own_root as usize]
                    .get_or_insert_with(|| counters.compare_set_of_root(own_root as usize))
                    .clone(),
            });
        }
        for (offset, value) in counters.hole_values[values.clone()].iter().enumerate() {
            job_counters.entry_values[start + offset] = value.resolve(&counters.entry_values);
        }
    }
    for job in jobs.iter_mut() {
        if let Some(job_plan) = job.plan.as_mut() {
            resolve_plan(job_plan, resolved);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::ClampShift;

    fn shifts() -> Vec<ClampShift> {
        let mut shifts = vec![ClampShift::IDENTITY];
        for amount in [
            1,
            -1,
            7,
            -7,
            1000,
            -1000,
            i32::MAX,
            i32::MIN,
            i32::MAX - 3,
            i32::MIN + 3,
        ] {
            shifts.push(ClampShift::increment(amount));
        }
        for value in [0, 5, -5, i32::MAX, i32::MIN, i32::MAX - 2, i32::MIN + 2] {
            shifts.push(ClampShift::set(value));
        }
        shifts
    }

    const VALUES: [i32; 9] = [0, 1, -1, 100, -100, i32::MAX, i32::MIN, i32::MAX - 5, i32::MIN + 5];

    #[test]
    fn a_composed_change_is_the_changes_one_after_the_other() {
        for &a in &shifts() {
            for &b in &shifts() {
                for value in VALUES {
                    assert_eq!(
                        a.then(b).apply(value),
                        b.apply(a.apply(value)),
                        "{a:?} then {b:?} on {value}"
                    );
                }
            }
        }
    }

    #[test]
    fn changes_compose_in_any_grouping() {
        let shifts = shifts();
        for &a in &shifts {
            for &b in &shifts {
                for &c in &shifts {
                    let left = a.then(b).then(c);
                    let right = a.then(b.then(c));
                    for value in VALUES {
                        assert_eq!(left.apply(value), right.apply(value), "{a:?} {b:?} {c:?} on {value}");
                    }
                }
            }
        }
    }

    #[test]
    fn many_increments_compose_without_overflow() {
        let mut up = ClampShift::IDENTITY;
        for _ in 0..100_000 {
            up = up.then(ClampShift::increment(i32::MAX));
        }
        assert_eq!(up.apply(i32::MIN), i32::MAX);
        let back = up.then(ClampShift::increment(-10));
        assert_eq!(back.apply(0), i32::MAX - 10);
        let mut down = ClampShift::IDENTITY;
        for _ in 0..100_000 {
            down = down.then(ClampShift::increment(i32::MIN));
        }
        assert_eq!(down.apply(i32::MAX), i32::MIN);
        assert_eq!(down.then(ClampShift::increment(3)).apply(0), i32::MIN + 3);
    }
}
