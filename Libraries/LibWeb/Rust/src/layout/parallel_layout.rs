/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use rayon::prelude::*;
use std::sync::LazyLock;

#[derive(PartialEq, Eq)]
enum ForkMode {
    Sequential,
    OnStyleLayoutThreads,
    ReversedOnCallingThread,
}

struct ForkSettings {
    mode: ForkMode,
    forces_forks: bool,
    verifies_subtree_leases: bool,
}

static SUBTREE_LEASES_ARE_VERIFIED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

static FORK_SETTINGS: LazyLock<ForkSettings> = LazyLock::new(|| {
    let verification = std::env::var("LIBWEB_VERIFY_LAYOUT_FORKS").unwrap_or_default();
    let mode = if verification == "reversed" {
        ForkMode::ReversedOnCallingThread
    } else if std::env::var("LIBWEB_LAYOUT_THREADS").is_ok_and(|threads| threads == "1") {
        ForkMode::Sequential
    } else {
        ForkMode::OnStyleLayoutThreads
    };
    let verifies_subtree_leases = std::env::var("LIBWEB_VERIFY_LAYOUT_FORK_SCOPE").is_ok_and(|value| value == "1");
    SUBTREE_LEASES_ARE_VERIFIED.store(verifies_subtree_leases, std::sync::atomic::Ordering::Relaxed);
    ForkSettings {
        mode,
        forces_forks: matches!(verification.as_str(), "always" | "reversed"),
        verifies_subtree_leases,
    }
});

/// Whether layout forks its runs however few boxes they hold: where `LIBWEB_VERIFY_LAYOUT_FORKS` asks for it, or where the
/// document forces parallel layout (see [`LayoutNodeArena::forces_parallel_layout`]).
fn forces_forks(pass: &LayoutPass<'_>) -> bool {
    FORK_SETTINGS.forces_forks || pass.arena().forces_parallel_layout()
}

const BOXES_COUNTED_PER_JOB: usize = 16;
const MINIMUM_BOXES_FORKED_FROM_A_STYLE_LAYOUT_THREAD: usize = 64;
const MINIMUM_BOXES_FORKED_ONTO_STYLE_LAYOUT_THREADS: usize = 128;

pub(crate) fn fork_candidates<T: Copy>(
    pass: &LayoutPass<'_>,
    items: impl IntoIterator<Item = T>,
    root_of: impl Fn(T) -> Node,
) -> Vec<T> {
    let settings = &*FORK_SETTINGS;
    let on_style_layout_thread = rayon::current_thread_index().is_some();
    if matches!(settings.mode, ForkMode::Sequential)
        || !pass.forks_runs()
        || (matches!(settings.mode, ForkMode::OnStyleLayoutThreads)
            && !on_style_layout_thread
            && style_layout_pool(pass).is_none())
    {
        return Vec::new();
    }
    let arena = pass.arena();
    let mut candidates = Vec::new();
    let mut boxes = 0;
    let mut sizeable_jobs = 0;
    for item in items {
        let root = root_of(item);
        if arena.data(root).first_child.get().is_invalid() {
            continue;
        }
        let counted = boxes_in_subtree_up_to(arena, root, BOXES_COUNTED_PER_JOB);
        boxes += counted;
        sizeable_jobs += usize::from(counted >= BOXES_COUNTED_PER_JOB);
        candidates.push(item);
    }
    if !on_style_layout_thread && sizeable_jobs >= 2 && boxes < MINIMUM_BOXES_FORKED_ONTO_STYLE_LAYOUT_THREADS {
        let limit = MINIMUM_BOXES_FORKED_ONTO_STYLE_LAYOUT_THREADS.div_ceil(candidates.len());
        boxes = candidates
            .iter()
            .map(|&item| boxes_in_subtree_up_to(arena, root_of(item), limit))
            .sum();
    }
    let worth_forking = forces_forks(pass)
        || (candidates.len() >= 2
            && if on_style_layout_thread {
                sizeable_jobs >= 2 || boxes >= MINIMUM_BOXES_FORKED_FROM_A_STYLE_LAYOUT_THREAD
            } else {
                boxes >= MINIMUM_BOXES_FORKED_ONTO_STYLE_LAYOUT_THREADS
            });
    if !worth_forking {
        candidates.clear();
    }
    candidates
}

pub(crate) fn lay_out_shapes_ahead(pass: &LayoutPass<'_>, shapes: Vec<formatting_context::ChildRunShape>) {
    let _ = lay_out_in_parallel(
        pass,
        shapes,
        |shape| shape.node,
        |pass, shape| {
            formatting_context::lay_out_shape_for_its_result(pass, shape);
        },
    );
}

pub(crate) struct ForkedTask<R> {
    pub(crate) result: R,
    pub(crate) host_findings: run_state::HostFindings,
}

pub(crate) fn lay_out_in_parallel<J: Send, R: Send>(
    pass: &LayoutPass<'_>,
    jobs: Vec<J>,
    root_of: impl Fn(&J) -> Node + Sync,
    run: impl Fn(LayoutPass<'_>, J) -> R + Sync,
) -> Result<Vec<ForkedTask<R>>, Vec<J>> {
    let settings = &*FORK_SETTINGS;
    if matches!(settings.mode, ForkMode::Sequential)
        || !pass.forks_runs()
        || jobs.is_empty()
        || (jobs.len() < 2 && !forces_forks(pass))
    {
        return Err(jobs);
    }
    let pass = *pass;
    let run_task = |job: J| {
        let host_findings = run_state::HostFindingsSink::default();
        let lease = SubtreeLease::of(pass.arena(), root_of(&job));
        let result = lease.while_held(settings.verifies_subtree_leases, || {
            run(pass.on_this_thread().noting_host_findings_in(&host_findings), job)
        });
        ForkedTask {
            result,
            host_findings: host_findings.take(),
        }
    };
    match settings.mode {
        ForkMode::Sequential => Err(jobs),
        ForkMode::OnStyleLayoutThreads => {
            if rayon::current_thread_index().is_some() {
                if !stack_has_room_for_forked_jobs(pass.arena()) {
                    return Err(jobs);
                }
                return Ok(jobs.into_par_iter().map(run_task).collect());
            }
            let Some(pool) = style_layout_pool(&pass) else {
                return Err(jobs);
            };
            Ok(pool.install(|| jobs.into_par_iter().map(run_task).collect()))
        }
        ForkMode::ReversedOnCallingThread => {
            if !stack_has_room_for_forked_jobs(pass.arena()) {
                return Err(jobs);
            }
            let mut tasks: Vec<_> = jobs.into_iter().rev().map(run_task).collect();
            tasks.reverse();
            Ok(tasks)
        }
    }
}

/// Whether a layout tree build may split its walk into jobs for the style layout threads: not when
/// `LIBWEB_LAYOUT_THREADS=1` asks for every job on the calling thread, nor when the threads are not running.
pub(crate) fn tree_build_forks_jobs(engine: &crate::css::style::RetainedState) -> bool {
    match FORK_SETTINGS.mode {
        ForkMode::Sequential => false,
        ForkMode::ReversedOnCallingThread => true,
        ForkMode::OnStyleLayoutThreads => {
            rayon::current_thread_index().is_some() || engine.started_style_layout_pool().is_some()
        }
    }
}

/// How many elements a layout tree build plans afresh at least before its jobs run on more than one thread.
// NB: Handing jobs to the threads of the pool and waiting for them costs a few hundred microseconds, more than a build
//     of fewer elements saves by running on several: a build of 3000 elements takes as long or longer on the pool as
//     on one thread, while one of 7000 to 10000 takes a tenth less. What counts is what the build plans, not how big
//     the document is: a small change of a big document plans few elements.
const MIN_ELEMENTS_FOR_PARALLEL_TREE_BUILD: usize = 4096;

/// How many threads the jobs of a layout tree build that plans `planned_elements` elements afresh may run on, at any
/// size for `forced`, where the document forces parallel layout. See [`planned_elements_up_to_parallel_threshold`].
pub(crate) fn tree_build_thread_count(
    engine: &crate::css::style::RetainedState,
    planned_elements: usize,
    forced: bool,
) -> usize {
    match FORK_SETTINGS.mode {
        ForkMode::Sequential => 1,
        ForkMode::ReversedOnCallingThread => 4,
        ForkMode::OnStyleLayoutThreads if !forced && !plans_enough_for_parallel_tree_build(planned_elements) => 1,
        ForkMode::OnStyleLayoutThreads => {
            if rayon::current_thread_index().is_some() {
                rayon::current_num_threads()
            } else {
                // NB: The thread that starts the build runs jobs too, once its walk is done.
                engine
                    .started_style_layout_pool()
                    .map_or(1, |pool| pool.current_num_threads() + 1)
            }
        }
    }
}

fn plans_enough_for_parallel_tree_build(planned_elements: usize) -> bool {
    planned_elements >= MIN_ELEMENTS_FOR_PARALLEL_TREE_BUILD
}

/// How many elements the subtrees of `roots` hold, themselves included, counting no further than the number a build has
/// to plan to run on several threads. `elements_below` adds the elements a node's subtree continues with.
// OPTIMIZATION: The count stops at the threshold, so a small change of a big document costs what it changes.
pub(crate) fn planned_elements_up_to_parallel_threshold<N: Copy>(
    roots: impl IntoIterator<Item = N>,
    mut elements_below: impl FnMut(N, &mut Vec<N>),
) -> usize {
    let mut pending: Vec<N> = roots.into_iter().collect();
    let mut count = 0;
    while let Some(node) = pending.pop() {
        count += 1;
        if plans_enough_for_parallel_tree_build(count) {
            break;
        }
        elements_below(node, &mut pending);
    }
    count
}

/// Runs the jobs of a layout tree build, on the style layout threads where it may, and answers their results in the
/// order of the jobs. Each job only reads the arena and the style engine.
pub(crate) fn run_tree_build_jobs<J: Send, R: Send>(
    arena: &LayoutNodeArena,
    engine: &crate::css::style::RetainedState,
    jobs: Vec<J>,
    run: impl Fn(J) -> R + Sync,
) -> Vec<R> {
    let in_order = |jobs: Vec<J>| jobs.into_iter().map(&run).collect::<Vec<_>>();
    match FORK_SETTINGS.mode {
        ForkMode::Sequential => in_order(jobs),
        ForkMode::ReversedOnCallingThread => {
            let mut results: Vec<R> = jobs.into_iter().rev().map(&run).collect();
            results.reverse();
            results
        }
        ForkMode::OnStyleLayoutThreads => {
            if jobs.len() < 2 || !stack_has_room_for_forked_jobs(arena) {
                return in_order(jobs);
            }
            if rayon::current_thread_index().is_some() {
                return jobs.into_par_iter().map(&run).collect();
            }
            match engine.started_style_layout_pool() {
                Some(pool) => pool.install(|| jobs.into_par_iter().map(&run).collect()),
                None => in_order(jobs),
            }
        }
    }
}

/// Runs `walk`, the walk of a layout tree build that may make jobs, with the scope that hands each job it makes to the
/// style layout threads as soon as it made it, where they run them: on the pool of the thread that runs the walk, or on
/// that of the style engine. There is none where every job runs on the thread that runs the walk.
pub(crate) fn in_tree_build_job_scope<'scope, R>(
    arena: &LayoutNodeArena,
    engine: &crate::css::style::RetainedState,
    makes_jobs: bool,
    walk: impl FnOnce(Option<&rayon::Scope<'scope>>) -> R,
) -> R {
    if !makes_jobs || FORK_SETTINGS.mode != ForkMode::OnStyleLayoutThreads || !stack_has_room_for_forked_jobs(arena) {
        return walk(None);
    }
    if rayon::current_thread_index().is_some() {
        return rayon::in_place_scope(|scope| walk(Some(scope)));
    }
    match engine.started_style_layout_pool() {
        Some(pool) => pool.in_place_scope(|scope| walk(Some(scope))),
        None => walk(None),
    }
}

/// Whether the jobs of a layout tree build that run on the thread of the walk that made them run in the reverse of the
/// order it made them in, as `LIBWEB_VERIFY_LAYOUT_FORKS=reversed` asks.
pub(crate) fn tree_build_jobs_run_reversed() -> bool {
    FORK_SETTINGS.mode == ForkMode::ReversedOnCallingThread
}

fn style_layout_pool(pass: &LayoutPass<'_>) -> Option<std::sync::Arc<rayon::ThreadPool>> {
    pass.arena()
        .with_style_store(crate::css::style::RetainedState::started_style_layout_pool)
}

fn boxes_in_subtree_up_to(arena: &LayoutNodeArena, root: Node, limit: usize) -> usize {
    let mut counted = 0;
    let mut current = Some(root);
    while let Some(node) = current {
        counted += 1;
        if counted >= limit {
            break;
        }
        current = arena.next_node_in_layout_subtree_in_pre_order(root, node);
    }
    counted
}

pub(crate) fn stack_has_room_for_forked_jobs(arena: &LayoutNodeArena) -> bool {
    let element_depth = arena.with_style_store(crate::css::style::RetainedState::deepest_element_depth);
    let box_depth = element_depth.saturating_mul(2).saturating_add(32);
    crate::render_state::stack_has_room_for_render_job(box_depth.saturating_mul(2))
}

#[derive(Clone, Copy)]
struct SubtreeLease {
    arena: *const LayoutNodeArena,
    root: Node,
    first_label: u64,
    label_past_subtree: u64,
}

thread_local! {
    static SUBTREE_LEASE_OF_THIS_THREAD: std::cell::Cell<Option<SubtreeLease>> = const { std::cell::Cell::new(None) };
}

impl SubtreeLease {
    fn of(arena: &LayoutNodeArena, root: Node) -> Self {
        Self {
            arena: std::ptr::from_ref(arena),
            root,
            first_label: arena.node_pre_order_label(root),
            label_past_subtree: arena.pre_order_label_of_subtree_successor(root),
        }
    }

    fn while_held<R>(self, verifies: bool, run: impl FnOnce() -> R) -> R {
        if !verifies {
            return run();
        }
        let enclosing = SUBTREE_LEASE_OF_THIS_THREAD.replace(Some(self));
        let result = run();
        SUBTREE_LEASE_OF_THIS_THREAD.set(enclosing);
        result
    }

    fn covers(&self, arena: &LayoutNodeArena, node: Node) -> bool {
        (self.first_label..self.label_past_subtree).contains(&arena.node_pre_order_label(node))
    }
}

#[inline]
pub(super) fn check_row_reached_by_forked_task(slot: usize, writes: bool) {
    if SUBTREE_LEASES_ARE_VERIFIED.load(std::sync::atomic::Ordering::Relaxed) {
        check_row_reached_under_the_lease_of_this_thread(slot, writes);
    }
}

#[cold]
#[inline(never)]
fn check_row_reached_under_the_lease_of_this_thread(slot: usize, writes: bool) {
    let Some(lease) = SUBTREE_LEASE_OF_THIS_THREAD.get() else {
        return;
    };
    // SAFETY: A lease is held only while the task it names runs, which borrows the arena.
    let arena = unsafe { &*lease.arena };
    let Some(node) = u32::try_from(slot).ok().and_then(|slot| arena.live_slot_at(slot)) else {
        panic!(
            "a forked layout task rooted at {:?} reached the row of slot {slot}, which holds no box",
            lease.root
        );
    };
    if lease.covers(arena, node) || (!writes && is_inclusive_ancestor(arena, node, lease.root)) {
        return;
    }
    panic!(
        "a forked layout task rooted at {:?} {} the row of {node:?}, outside its subtree",
        lease.root,
        if writes { "wrote" } else { "read" }
    );
}

fn is_inclusive_ancestor(arena: &LayoutNodeArena, ancestor: Node, node: Node) -> bool {
    let mut current = node;
    while !current.is_invalid() {
        if current == ancestor {
            return true;
        }
        current = arena.data(current).parent.get();
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A tree of `depth` levels below one root, each node with `fan_out` children, numbered breadth first.
    fn children(fan_out: usize, depth: u32) -> impl FnMut((usize, u32), &mut Vec<(usize, u32)>) {
        move |(index, level), pending| {
            if level < depth {
                pending.extend((0..fan_out).map(|child| (index * fan_out + child + 1, level + 1)));
            }
        }
    }

    #[test]
    fn a_build_of_a_few_elements_runs_on_one_thread_and_a_big_one_forks() {
        // 1 + 4 + 16 + 64 elements.
        let small = planned_elements_up_to_parallel_threshold([(0, 0)], children(4, 3));
        assert_eq!(small, 85);
        assert!(!plans_enough_for_parallel_tree_build(small));
        let big = planned_elements_up_to_parallel_threshold([(0, 0)], children(8, 5));
        assert!(plans_enough_for_parallel_tree_build(big));
    }

    #[test]
    fn the_count_stops_at_the_threshold() {
        let mut visited = 0;
        let mut below = children(16, 6);
        let count = planned_elements_up_to_parallel_threshold([(0, 0)], |node, pending| {
            visited += 1;
            below(node, pending);
        });
        assert_eq!(count, MIN_ELEMENTS_FOR_PARALLEL_TREE_BUILD);
        assert!(visited < MIN_ELEMENTS_FOR_PARALLEL_TREE_BUILD);
    }

    #[test]
    fn several_roots_count_together() {
        let roots = (0..3).map(|root| (root * 1000, 0));
        assert_eq!(planned_elements_up_to_parallel_threshold(roots, children(2, 2)), 3 * 7);
    }
}
