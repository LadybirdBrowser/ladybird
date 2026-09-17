/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout update: the style and layout stabilization loop a document runs before it can
//! answer geometry queries or paint. The steps that touch the DOM stay on the C++ host and
//! answer through a callback table registered once per arena.

use super::LayoutNodeArena;
use super::formatting_context::{compute_subtree_layout, run_root_layout};
use super::layout_node_arena::sync_enrolled_content_for_layout;
use super::node_data::NodeSlotId;
use super::node_facts;
use super::partial_relayout::FfiPartialRelayoutHostFacts;
use super::tree_builder::FfiLayoutTreeBuildOutcome;
use crate::abort_on_panic;
use crate::css::ffi_support::FfiUtf16View;
use std::ffi::c_void;
use std::time::Instant;

/// The document-side steps of a layout update. Each callback receives the registered
/// `context`, the owning document, first and answers synchronously; any of them may run the
/// layout update of another document, so the loop holds no arena borrow across a call.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutUpdateHostCallbacks {
    pub context: *mut c_void,
    pub connected_element_count: unsafe extern "C" fn(*mut c_void) -> u32,
    pub update_style: unsafe extern "C" fn(*mut c_void),
    pub process_pending_list_item_renumbers: unsafe extern "C" fn(*mut c_void),
    pub process_pending_top_layer_layout_changes: unsafe extern "C" fn(*mut c_void),
    pub document_facts: unsafe extern "C" fn(*mut c_void) -> FfiLayoutUpdateDocumentFacts,
    pub needs_style_update_after_layout: unsafe extern "C" fn(*mut c_void) -> bool,
    pub prepare_for_rendering: unsafe extern "C" fn(*mut c_void),
    /// Builds or updates the layout tree and installs its viewport as the document's layout root.
    pub build_layout_tree: unsafe extern "C" fn(*mut c_void) -> FfiLayoutTreeBuildOutcome,
    pub clear_needs_full_layout_tree_update: unsafe extern "C" fn(*mut c_void),
    /// True when stale list-item counters marked more of the tree for a rebuild.
    pub reconcile_stale_list_item_counters_after_tree_build: unsafe extern "C" fn(*mut c_void) -> bool,
    /// Refreshes what derives from committed layout; the flag says whether the tree changed.
    pub after_layout_commit: unsafe extern "C" fn(*mut c_void, bool),
    pub note_full_layout_performed: unsafe extern "C" fn(*mut c_void),
    pub evaluate_pending_container_queries: unsafe extern "C" fn(*mut c_void),
    pub record_stabilization_bound_failure: unsafe extern "C" fn(*mut c_void),
}

/// What the loop needs to know about the document at one point in time. Every host call can
/// change these, so the loop asks again after each one it depends on.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutUpdateDocumentFacts {
    /// The document is its navigable's active document; an inactive document counts as laid out.
    pub document_is_active: bool,
    /// The document's layout root, invalid when it has none. Mirrors the arena's record.
    pub layout_root: NodeSlotId,
    /// The document node or one of its descendants needs a layout tree update.
    pub document_needs_layout_tree_build: bool,
    pub needs_full_layout_tree_update: bool,
    pub container_query_evaluation_is_pending: bool,
    /// A top layer membership change or zone rebuild is waiting for the next pass.
    pub top_layer_work_pending: bool,
    pub should_collect_devtools_layout_data: bool,
    pub document_in_quirks_mode: bool,
    pub viewport_inline_size_raw: i32,
    pub viewport_block_size_raw: i32,
}

/// What one layout update was asked for.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutUpdateInputs {
    pub reason_is_inspect_devtools_layout_data: bool,
    /// A document hosting template contents never needs layout.
    pub is_template_contents_document: bool,
    /// The update reason's name, read only when tracing is enabled.
    pub reason_name: FfiUtf16View,
}

/// Confinement report of the most recent layout tree build, for tests observing whether a
/// partial rebuild stayed inside its rebuilt subtrees.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiLayoutTreeBuildStats {
    pub builds: u64,
    pub last_build_rebuilt_subtree_roots: u64,
    pub last_build_escaped_rebuild_roots: bool,
}

impl FfiLayoutUpdateHostCallbacks {
    // SAFETY (for every call below): The C++ host answers synchronously from its live document.
    fn connected_element_count(&self) -> u32 {
        unsafe { (self.connected_element_count)(self.context) }
    }

    fn update_style(&self) {
        unsafe { (self.update_style)(self.context) }
    }

    fn process_pending_list_item_renumbers(&self) {
        unsafe { (self.process_pending_list_item_renumbers)(self.context) }
    }

    fn process_pending_top_layer_layout_changes(&self) {
        unsafe { (self.process_pending_top_layer_layout_changes)(self.context) }
    }

    fn document_facts(&self, arena: &LayoutNodeArena) -> FfiLayoutUpdateDocumentFacts {
        let facts = unsafe { (self.document_facts)(self.context) };
        debug_assert_eq!(facts.layout_root, arena.layout_root());
        facts
    }

    fn needs_style_update_after_layout(&self) -> bool {
        unsafe { (self.needs_style_update_after_layout)(self.context) }
    }

    fn prepare_for_rendering(&self) {
        unsafe { (self.prepare_for_rendering)(self.context) }
    }

    fn build_layout_tree(&self) -> FfiLayoutTreeBuildOutcome {
        unsafe { (self.build_layout_tree)(self.context) }
    }

    fn clear_needs_full_layout_tree_update(&self) {
        unsafe { (self.clear_needs_full_layout_tree_update)(self.context) }
    }

    fn reconcile_stale_list_item_counters_after_tree_build(&self) -> bool {
        unsafe { (self.reconcile_stale_list_item_counters_after_tree_build)(self.context) }
    }

    fn after_layout_commit(&self, layout_tree_changed: bool) {
        unsafe { (self.after_layout_commit)(self.context, layout_tree_changed) }
    }

    fn note_full_layout_performed(&self) {
        unsafe { (self.note_full_layout_performed)(self.context) }
    }

    fn evaluate_pending_container_queries(&self) {
        unsafe { (self.evaluate_pending_container_queries)(self.context) }
    }

    fn record_stabilization_bound_failure(&self) {
        unsafe { (self.record_stabilization_bound_failure)(self.context) }
    }
}

/// Mirrors the document's own predicate: an inactive document is left alone, a document without
/// a tree needs one, and otherwise nothing may be pending on the root, the DOM, or a boundary.
fn layout_is_up_to_date(arena: &LayoutNodeArena, facts: &FfiLayoutUpdateDocumentFacts) -> bool {
    if !facts.document_is_active {
        return true;
    }
    let layout_root = arena.layout_root();
    if layout_root.is_invalid() {
        return false;
    }
    !arena.node_needs_layout_update(layout_root)
        && !facts.document_needs_layout_tree_build
        && !facts.needs_full_layout_tree_update
        && !arena.has_partial_relayout_boundary_roots()
}

/// The `TREEBUILD` and `LAYOUT` timing lines, off unless `LIBWEB_UPDATE_LAYOUT_TRACE` is set.
struct UpdateLayoutTrace {
    reason: Option<String>,
}

fn update_layout_trace_is_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var_os("LIBWEB_UPDATE_LAYOUT_TRACE").is_some())
}

impl UpdateLayoutTrace {
    /// # Safety
    ///
    /// `reason_name` must satisfy [`FfiUtf16View::to_utf16`]'s requirements.
    unsafe fn new(reason_name: FfiUtf16View) -> Self {
        if !update_layout_trace_is_enabled() {
            return Self { reason: None };
        }
        // SAFETY: Guaranteed by the caller.
        let reason = unsafe { reason_name.to_utf16() }
            .map(|units| String::from_utf16_lossy(&units))
            .unwrap_or_default();
        Self { reason: Some(reason) }
    }

    fn now(&self) -> Option<Instant> {
        self.reason.as_ref().map(|_| Instant::now())
    }

    fn tree_build(&self, started: Option<Instant>) {
        if let Some(started) = started {
            eprintln!("TREEBUILD {} µs", started.elapsed().as_micros());
        }
    }

    fn layout(&self, started: Option<Instant>) {
        if let (Some(reason), Some(started)) = (&self.reason, started) {
            eprintln!("LAYOUT {reason} {} µs", started.elapsed().as_micros());
        }
    }
}

enum PartialRelayout {
    NotEligible,
    Done,
    NeedsAnotherLayoutPass,
}

const ORDINARY_STABILIZATION_ROUND_LIMIT: u64 = 8;

/// # Safety
///
/// `arena_handle` must be a live handle with registered layout and layout update hosts, used on
/// the document thread between `layout_arena_begin_update_layout` and its end.
unsafe fn arena<'a>(arena_handle: *mut c_void) -> &'a LayoutNodeArena {
    // SAFETY: Guaranteed by the caller.
    unsafe { LayoutNodeArena::from_handle(arena_handle) }
}

/// Attempts to satisfy the pending layout update by re-laying out only the registered partial
/// relayout boundary subtrees. Runs the incremental layout tree build itself when tree updates
/// are pending (consuming `needs_layout_tree_rebuild`), so an ineligible update continues to
/// the full layout path without rebuilding again.
///
/// # Safety
///
/// As for [`arena`].
unsafe fn try_partial_relayout(
    arena_handle: *mut c_void,
    host: &FfiLayoutUpdateHostCallbacks,
    facts: &FfiLayoutUpdateDocumentFacts,
    registered_partial_relayout_roots: &mut Vec<NodeSlotId>,
    needs_layout_tree_rebuild: &mut bool,
    trace: &UpdateLayoutTrace,
) -> PartialRelayout {
    // SAFETY (for every derive below): Guaranteed by the caller; no borrow spans a host call.
    let partial_relayout_facts = FfiPartialRelayoutHostFacts {
        document_needs_full_layout_tree_update: facts.needs_full_layout_tree_update,
        container_query_evaluation_is_pending: facts.container_query_evaluation_is_pending,
        should_collect_devtools_layout_data: facts.should_collect_devtools_layout_data,
    };
    if !unsafe { arena(arena_handle) }.partial_relayout_may_be_attempted(
        unsafe { arena(arena_handle) }.layout_root(),
        registered_partial_relayout_roots,
        partial_relayout_facts,
    ) {
        return PartialRelayout::NotEligible;
    }

    let mut layout_tree_was_built_in_partial_branch = false;
    if *needs_layout_tree_rebuild {
        let tree_build_started = trace.now();
        let outcome = host.build_layout_tree();
        unsafe { arena(arena_handle) }.record_layout_tree_build(&outcome);
        *needs_layout_tree_rebuild = false;
        if host.reconcile_stale_list_item_counters_after_tree_build() || outcome.needs_another_build_pass {
            return PartialRelayout::NeedsAnotherLayoutPass;
        }
        layout_tree_was_built_in_partial_branch = true;

        // The build invalidates what deferred child list insertions reach, which can register
        // more boundaries.
        registered_partial_relayout_roots.extend(unsafe { arena(arena_handle) }.take_partial_relayout_boundary_roots());
        trace.tree_build(tree_build_started);
    }

    let layout_root = unsafe { arena(arena_handle) }.layout_root();
    let Some(partial_relayout_roots) = unsafe { arena(arena_handle) }
        .plan_partial_relayout_with_pending_rebuilt_roots(layout_root, registered_partial_relayout_roots)
    else {
        return PartialRelayout::NotEligible;
    };
    for &root in &partial_relayout_roots {
        debug_assert!(unsafe { arena(arena_handle) }.slot_is_live(root));
        debug_assert!(node_facts::kind_is_box(
            unsafe { arena(arena_handle) }.data(root).kind.get()
        ));
    }

    // The build may have resized this document's viewport through its embedding document.
    let facts = host.document_facts(unsafe { arena(arena_handle) });
    unsafe { sync_enrolled_content_for_layout(arena_handle) };
    for &root in &partial_relayout_roots {
        unsafe {
            compute_subtree_layout(
                arena_handle,
                root,
                layout_root,
                facts.viewport_inline_size_raw,
                facts.viewport_block_size_raw,
                facts.document_in_quirks_mode,
            );
        }
    }

    unsafe { arena(arena_handle) }.note_partial_layout();

    host.after_layout_commit(layout_tree_was_built_in_partial_branch);
    if host.needs_style_update_after_layout()
        || !layout_is_up_to_date(
            unsafe { arena(arena_handle) },
            &host.document_facts(unsafe { arena(arena_handle) }),
        )
    {
        return PartialRelayout::NeedsAnotherLayoutPass;
    }
    PartialRelayout::Done
}

/// # Safety
///
/// As for [`arena`], and `inputs` must satisfy [`UpdateLayoutTrace::new`]'s requirements.
unsafe fn update_layout(arena_handle: *mut c_void, inputs: &FfiLayoutUpdateInputs) {
    // SAFETY (for every derive below): Guaranteed by the caller; no borrow spans a host call.
    let host = unsafe { arena(arena_handle) }.layout_update_host();
    assert!(
        unsafe { arena(arena_handle) }.update_layout_is_running(),
        "the layout update runs between layout_arena_begin_update_layout and its end"
    );
    let trace = unsafe { UpdateLayoutTrace::new(inputs.reason_name) };

    // Size-query dependencies point from a descendant to an ancestor query container. They are
    // therefore acyclic, and a coherent style/layout pass can settle at least one more level of
    // a nested dependency chain. One pass per connected element is a conservative exact bound.
    // Recompute it after each pass because an initial style update can enroll the elements of a
    // freshly parsed document after the layout update has already started.
    let mut layout_pass: u64 = 0;
    while layout_pass < ORDINARY_STABILIZATION_ROUND_LIMIT + u64::from(host.connected_element_count()) + 1 {
        layout_pass += 1;

        host.update_style();
        host.process_pending_list_item_renumbers();
        host.process_pending_top_layer_layout_changes();

        let facts = host.document_facts(unsafe { arena(arena_handle) });
        let force_devtools_layout_data_collection =
            facts.should_collect_devtools_layout_data && inputs.reason_is_inspect_devtools_layout_data;

        if layout_is_up_to_date(unsafe { arena(arena_handle) }, &facts) && !force_devtools_layout_data_collection {
            host.prepare_for_rendering();
            return;
        }

        let mut registered_partial_relayout_roots =
            unsafe { arena(arena_handle) }.take_partial_relayout_boundary_roots();

        // NOTE: If this is a document hosting <template> contents, layout is unnecessary.
        if inputs.is_template_contents_document {
            return;
        }

        let mut needs_layout_tree_rebuild = unsafe { arena(arena_handle) }.layout_root().is_invalid()
            || facts.document_needs_layout_tree_build
            || facts.needs_full_layout_tree_update;

        match unsafe {
            try_partial_relayout(
                arena_handle,
                &host,
                &facts,
                &mut registered_partial_relayout_roots,
                &mut needs_layout_tree_rebuild,
                &trace,
            )
        } {
            PartialRelayout::Done => return,
            PartialRelayout::NeedsAnotherLayoutPass => continue,
            PartialRelayout::NotEligible => {}
        }
        drop(registered_partial_relayout_roots);

        // A build in the partial branch may have resized this document's viewport through its
        // embedding document.
        let facts = host.document_facts(unsafe { arena(arena_handle) });
        let layout_started = trace.now();

        if needs_layout_tree_rebuild {
            let outcome = host.build_layout_tree();
            unsafe { arena(arena_handle) }.record_layout_tree_build(&outcome);

            if outcome.needs_another_build_pass {
                continue;
            }

            // The full layout below covers every boundary the build's invalidation registered.
            drop(unsafe { arena(arena_handle) }.take_partial_relayout_boundary_roots());

            host.clear_needs_full_layout_tree_update();
            trace.tree_build(layout_started);

            if host.reconcile_stale_list_item_counters_after_tree_build() {
                continue;
            }
        }

        let layout_root = unsafe { arena(arena_handle) }.layout_root();
        assert!(!layout_root.is_invalid(), "a full layout pass needs a layout root");
        unsafe {
            run_root_layout(
                arena_handle,
                layout_root,
                facts.viewport_inline_size_raw,
                facts.viewport_block_size_raw,
                facts.document_in_quirks_mode,
                facts.should_collect_devtools_layout_data,
            );
        }

        host.note_full_layout_performed();
        unsafe { arena(arena_handle) }.note_full_layout();

        host.after_layout_commit(true);
        trace.layout(layout_started);

        host.evaluate_pending_container_queries();

        if host.needs_style_update_after_layout() {
            continue;
        }

        let facts = host.document_facts(unsafe { arena(arena_handle) });
        // A zone rebuild requested during layout tree construction runs as another pass.
        if facts.top_layer_work_pending {
            continue;
        }

        // Layout-only invalidations still need to be flushed before we can exit.
        if layout_is_up_to_date(unsafe { arena(arena_handle) }, &facts) {
            break;
        }
    }

    if host.needs_style_update_after_layout()
        || !layout_is_up_to_date(
            unsafe { arena(arena_handle) },
            &host.document_facts(unsafe { arena(arena_handle) }),
        )
    {
        host.record_stabilization_bound_failure();
        unreachable!("the layout update did not stabilize within its exact bound");
    }
}

/// # Safety
///
/// `arena` must be a live handle on the document thread. The callbacks must remain valid until
/// they are cleared or the arena is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_layout_update_host_callbacks(
    arena: *mut c_void,
    callbacks: FfiLayoutUpdateHostCallbacks,
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.set_layout_update_host(Some(callbacks));
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_layout_update_host_callbacks(arena: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.set_layout_update_host(None);
}

/// # Safety
///
/// `arena` must be a live handle on the document thread, with no layout update running.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_begin_update_layout(arena: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.begin_update_layout();
}

/// # Safety
///
/// `arena` must be a live handle on the document thread, with a layout update running.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_end_update_layout(arena: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.end_update_layout();
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_layout_is_running(arena: *mut c_void) -> bool {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.update_layout_is_running()
}

/// Runs the document's layout update to a fixed point: style, then the layout tree build, then
/// either a partial relayout of the registered boundaries or a full pass, until nothing is
/// pending. The document-side steps run through the registered layout update host.
///
/// # Safety
///
/// `arena` must be a live handle with registered layout and layout update hosts, used on the
/// document thread between `layout_arena_begin_update_layout` and its end, and `inputs` must
/// remain valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_layout(arena: *mut c_void, inputs: *const FfiLayoutUpdateInputs) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    assert!(!inputs.is_null());
    abort_on_panic(|| {
        // SAFETY: Guaranteed by the entry point's contract.
        unsafe { update_layout(arena, &*inputs) };
    });
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_partial_layout_count(arena: *mut c_void) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.partial_layout_count()
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_full_layout_count(arena: *mut c_void) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.full_layout_count()
}

/// # Safety
///
/// `arena` must be a live handle on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_layout_tree_build_stats(arena: *mut c_void) -> FfiLayoutTreeBuildStats {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { LayoutNodeArena::from_handle(arena) }.layout_tree_build_stats()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts_for(arena: &LayoutNodeArena) -> FfiLayoutUpdateDocumentFacts {
        FfiLayoutUpdateDocumentFacts {
            document_is_active: true,
            layout_root: arena.layout_root(),
            document_needs_layout_tree_build: false,
            needs_full_layout_tree_update: false,
            container_query_evaluation_is_pending: false,
            top_layer_work_pending: false,
            should_collect_devtools_layout_data: false,
            document_in_quirks_mode: false,
            viewport_inline_size_raw: 0,
            viewport_block_size_raw: 0,
        }
    }

    #[test]
    fn an_inactive_document_counts_as_laid_out_and_a_rootless_active_one_does_not() {
        let arena = LayoutNodeArena::new();
        let mut facts = facts_for(&arena);
        assert!(!layout_is_up_to_date(&arena, &facts));
        facts.document_is_active = false;
        assert!(layout_is_up_to_date(&arena, &facts));
    }

    #[test]
    fn dom_side_pending_work_keeps_a_rooted_document_from_being_laid_out() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_unbound(std::ptr::null_mut());
        arena.set_layout_root(viewport);
        arena.reset_layout_update_flags_in_subtree(viewport);
        let facts = facts_for(&arena);
        assert!(layout_is_up_to_date(&arena, &facts));

        let mut tree_build_pending = facts;
        tree_build_pending.document_needs_layout_tree_build = true;
        assert!(!layout_is_up_to_date(&arena, &tree_build_pending));

        let mut full_rebuild_pending = facts;
        full_rebuild_pending.needs_full_layout_tree_update = true;
        assert!(!layout_is_up_to_date(&arena, &full_rebuild_pending));

        arena.free_subtree(viewport).destroy_shells_and_invoke_callbacks();
        assert!(!layout_is_up_to_date(&arena, &facts));
    }

    #[test]
    fn the_layout_counters_start_at_zero_and_count_each_pass() {
        let arena = LayoutNodeArena::new();
        assert_eq!(arena.partial_layout_count(), 0);
        assert_eq!(arena.full_layout_count(), 0);
        assert_eq!(arena.layout_tree_build_stats().builds, 0);
        arena.note_partial_layout();
        arena.note_full_layout();
        arena.note_full_layout();
        arena.record_layout_tree_build(&FfiLayoutTreeBuildOutcome {
            viewport: NodeSlotId::INVALID,
            rebuilt_subtree_root_count: 3,
            layout_tree_update_escaped_rebuild_roots: true,
            needs_another_build_pass: false,
        });
        assert_eq!(arena.partial_layout_count(), 1);
        assert_eq!(arena.full_layout_count(), 2);
        let stats = arena.layout_tree_build_stats();
        assert_eq!(stats.builds, 1);
        assert_eq!(stats.last_build_rebuilt_subtree_roots, 3);
        assert!(stats.last_build_escaped_rebuild_roots);
    }
}
