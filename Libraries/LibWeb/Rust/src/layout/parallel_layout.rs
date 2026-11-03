/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use rayon::prelude::*;
use std::sync::LazyLock;

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
    let worth_forking = settings.forces_forks
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
        |callbacks, shape| {
            formatting_context::lay_out_shape_for_its_result(callbacks, shape);
        },
    );
}

pub(crate) fn prepare_inline_content_ahead(pass: &LayoutPass<'_>, block_containers: Vec<Node>) {
    let _ = lay_out_in_parallel(
        pass,
        block_containers,
        |&block_container| block_container,
        |callbacks, block_container| {
            prepared_inline_content::PreparedInlineContent::for_block_container(callbacks, block_container);
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
        || (jobs.len() < 2 && !settings.forces_forks)
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
            let mut tasks: Vec<_> = jobs.into_iter().rev().map(run_task).collect();
            tasks.reverse();
            Ok(tasks)
        }
    }
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

fn stack_has_room_for_forked_jobs(arena: &LayoutNodeArena) -> bool {
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
