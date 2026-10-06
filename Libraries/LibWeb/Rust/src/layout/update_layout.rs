/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout update: the style and layout stabilization loop a document runs before it can
//! answer geometry queries or paint. The steps that touch the DOM stay on the C++ host and
//! answer through a callback table registered once per arena.

use super::commit::CommitNotifications;
use super::formatting_context::{FfiLayoutHostCallbacks, LayoutStageFacts, lay_out_boundary, lay_out_root};
use super::layout_node_arena::{OwedImageResources, sync_enrolled_content_for_layout};
use super::node_data::NodeSlotId;
use super::node_facts;
use super::partial_relayout::FfiPartialRelayoutHostFacts;
use super::tree_builder::{FfiGeneratedImage, FfiPseudoElement, TreeBuildAnswer, TreeBuildJob};
use super::tree_mutation::{HostWorkDue, OwedHostWork};
use super::{ArenaHandle, LayoutNodeArena};
use crate::abort_on_panic;
use crate::css::style::tree::StyleNodeID;
use crate::painting::recording_slot::FlightLicense;
use crate::render_state::{BegunRead, DocumentHost};
use crate::stage::MainThread;
use std::ffi::c_void;

mod main_thread_entries;

pub(crate) use main_thread_entries::MainThreadFfiEntry;

/// Runs `job`, a layout round of `host`'s document, on the document's render state, and answers what the round owes the
/// host, in `read`.
fn run_layout_round_job(host: &DocumentHost, read: &BegunRead, job: LayoutRoundJob) -> LayoutRoundAnswer {
    host.let_go_of_rows();
    // The host keeps what the job's inputs name until it has the answer.
    host.run(read, true, move |state| job.run(state.arena_handle_mut()))
}

/// Answers `answer` from the render state of `host`'s document and `args`, in `read`: the host's layout update reads it
/// between rounds, for whether the layout is up to date and what the next round's build needs.
fn read_arena<A, R>(host: &DocumentHost, read: &BegunRead, args: A, answer: fn(&mut LayoutNodeArena, A) -> R) -> R {
    host.ask(read, |state| answer(state.arena_mut(), args))
}

/// The document-side steps of a layout update. Each callback receives the registered
/// `context`, the owning document, first and answers synchronously; any of them may run the
/// layout update of another document, so the loop holds no arena borrow across a call. The
/// methods below, which take the main thread token, are the only way to call them.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutUpdateHostCallbacks {
    context: *mut c_void,
    connected_element_count: unsafe extern "C" fn(*mut c_void, &BegunRead) -> u32,
    update_style: unsafe extern "C" fn(*mut c_void, &BegunRead),
    process_pending_list_item_renumbers: unsafe extern "C" fn(*mut c_void, &BegunRead),
    process_pending_top_layer_layout_changes: unsafe extern "C" fn(*mut c_void, &BegunRead),
    document_facts: unsafe extern "C" fn(*mut c_void, &BegunRead) -> FfiLayoutUpdateDocumentFacts,
    /// True when a size container waits for its queries to be evaluated after the next full layout. Only an update
    /// that may take the partial relayout path asks.
    container_query_evaluation_is_pending: unsafe extern "C" fn(*mut c_void, &BegunRead) -> bool,
    needs_style_update_after_layout: unsafe extern "C" fn(*mut c_void, &BegunRead) -> bool,
    prepare_for_rendering: unsafe extern "C" fn(*mut c_void, &BegunRead),
    /// Readies the document for a layout tree build, and answers the document's style, interned as a
    /// record, where the flag says the build may build the viewport, and 0 otherwise.
    prepare_layout_tree_build: unsafe extern "C" fn(*mut c_void, &BegunRead, bool) -> u64,
    /// Retires the tree a build replaced once the host has been paid for the build: the viewport
    /// before the build, then the one it placed.
    finish_layout_tree_build: unsafe extern "C" fn(*mut c_void, &BegunRead, NodeSlotId, NodeSlotId),
    /// True when stale list-item counters marked more of the tree for a rebuild.
    reconcile_stale_list_item_counters_after_tree_build: unsafe extern "C" fn(*mut c_void, &BegunRead) -> bool,
    /// Refreshes what derives from committed layout; the flag says whether the tree changed.
    after_layout_commit: unsafe extern "C" fn(*mut c_void, &BegunRead, bool),
    note_full_layout_performed: unsafe extern "C" fn(*mut c_void),
    evaluate_pending_container_queries: unsafe extern "C" fn(*mut c_void, &BegunRead),
    record_stabilization_bound_failure: unsafe extern "C" fn(*mut c_void),
    /// Attaches the image resources a box's style asks for. The flag says the box replaces its
    /// element's contents with a single image, whose provider it owns.
    attach_style_resources: unsafe extern "C" fn(*mut c_void, &BegunRead, NodeSlotId, bool, FfiStyleImageFacts),
    /// Gives a generated image box the provider of the image it shows, which the box owns, and
    /// attaches the box's style resources. The image is the `<image>` at the given index of the
    /// pseudo-element's `content`, or the given marker's `list-style-image`.
    attach_generated_image: unsafe extern "C" fn(
        *mut c_void,
        &BegunRead,
        NodeSlotId,
        u32,
        FfiPseudoElement,
        FfiGeneratedImage,
        FfiStyleImageFacts,
    ),
}

/// What the loop needs to know about the document at one point in time. Every host call can
/// change these, so the loop asks again after each one it depends on.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutUpdateDocumentFacts {
    /// The document is its navigable's active document; an inactive document counts as laid out.
    pub document_is_active: bool,
    /// The document node or one of its descendants needs a layout tree update.
    pub document_needs_layout_tree_build: bool,
    /// A top layer membership change or zone rebuild is waiting for the next pass.
    pub top_layer_work_pending: bool,
    pub should_collect_devtools_layout_data: bool,
    pub document_in_quirks_mode: bool,
    pub viewport_inline_size_raw: i32,
    pub viewport_block_size_raw: i32,
    /// The document's style node, which a layout tree build walks from.
    pub document_style_node: u32,
    /// The document holds list owners whose item counters went stale without changing what they
    /// render, which it reconciles with what a layout tree build rebuilt before the tree lays out.
    pub has_stale_list_item_counters: bool,
}

/// What one layout update was asked for.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutUpdateInputs {
    pub reason_is_inspect_devtools_layout_data: bool,
    /// A document hosting template contents never needs layout.
    pub is_template_contents_document: bool,
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
    fn connected_element_count(&self, _: &MainThread, read: &BegunRead) -> u32 {
        unsafe { (self.connected_element_count)(self.context, read) }
    }

    fn update_style(&self, _: &MainThread, read: &BegunRead) {
        unsafe { (self.update_style)(self.context, read) }
    }

    fn process_pending_list_item_renumbers(&self, _: &MainThread, read: &BegunRead) {
        unsafe { (self.process_pending_list_item_renumbers)(self.context, read) }
    }

    fn process_pending_top_layer_layout_changes(&self, _: &MainThread, read: &BegunRead) {
        unsafe { (self.process_pending_top_layer_layout_changes)(self.context, read) }
    }

    fn document_facts(&self, _: &MainThread, read: &BegunRead) -> FfiLayoutUpdateDocumentFacts {
        unsafe { (self.document_facts)(self.context, read) }
    }

    fn container_query_evaluation_is_pending(&self, _: &MainThread, read: &BegunRead) -> bool {
        unsafe { (self.container_query_evaluation_is_pending)(self.context, read) }
    }

    fn needs_style_update_after_layout(&self, _: &MainThread, read: &BegunRead) -> bool {
        unsafe { (self.needs_style_update_after_layout)(self.context, read) }
    }

    fn prepare_for_rendering(&self, _: &MainThread, read: &BegunRead) {
        unsafe { (self.prepare_for_rendering)(self.context, read) }
    }

    fn prepare_layout_tree_build(&self, _: &MainThread, read: &BegunRead, may_create_viewport: bool) -> Option<u64> {
        let record = unsafe { (self.prepare_layout_tree_build)(self.context, read, may_create_viewport) };
        (record != 0).then_some(record)
    }

    fn finish_layout_tree_build(
        &self,
        _: &MainThread,
        read: &BegunRead,
        replaced_viewport: NodeSlotId,
        viewport: NodeSlotId,
    ) {
        unsafe { (self.finish_layout_tree_build)(self.context, read, replaced_viewport, viewport) }
    }

    fn reconcile_stale_list_item_counters_after_tree_build(&self, _: &MainThread, read: &BegunRead) -> bool {
        unsafe { (self.reconcile_stale_list_item_counters_after_tree_build)(self.context, read) }
    }

    fn after_layout_commit(&self, _: &MainThread, read: &BegunRead, layout_tree_changed: bool) {
        unsafe { (self.after_layout_commit)(self.context, read, layout_tree_changed) }
    }

    fn note_full_layout_performed(&self, _: &MainThread) {
        unsafe { (self.note_full_layout_performed)(self.context) }
    }

    fn evaluate_pending_container_queries(&self, _: &MainThread, read: &BegunRead) {
        unsafe { (self.evaluate_pending_container_queries)(self.context, read) }
    }

    fn record_stabilization_bound_failure(&self, _: &MainThread) {
        unsafe { (self.record_stabilization_bound_failure)(self.context) }
    }

    /// Hands the boxes the tree builds of the update stamped the image resources they are owed,
    /// in the order the builds came to owe them. Until now a box that owns its image's provider
    /// had no image; one handed a provider whose image is already there lays out again.
    fn attach_owed_image_resources(&self, _: &MainThread, host: &DocumentHost, read: &BegunRead) {
        if host.known_owed_image_resources() == Some(false) {
            return;
        }
        // Each box reads whether its style holds image values, which the engine answers with the rows rather than
        // once per box, and what its row is, which the rows published with them answer.
        let (owed, rows) = read_arena(host, read, (), |arena, ()| {
            let owed = arena.take_image_resources_owed_to_host();
            let owed = arena.with_style_engine(|engine| {
                owed.into_iter()
                    // A later build of the update may have freed the row.
                    .filter(|&(row, _)| arena.slot_is_live(row))
                    .map(|(row, owed)| {
                        arena.note_owned_provider_handed_over(row);
                        (row, owed, FfiStyleImageFacts::of(engine, arena.node_style_record(row)))
                    })
                    .collect::<Vec<_>>()
            });
            let rows = (!owed.is_empty()).then(|| arena.publish_row_snapshot(false));
            (owed, rows)
        });
        if let Some(rows) = rows {
            host.keep_rows(rows);
        }
        for (row, owed, images) in owed {
            match owed {
                OwedImageResources::StyleResources {
                    owns_content_replacement_image,
                } => unsafe {
                    (self.attach_style_resources)(self.context, read, row, owns_content_replacement_image, images);
                },
                OwedImageResources::GeneratedImage {
                    generator,
                    pseudo_element,
                    image,
                } => unsafe {
                    (self.attach_generated_image)(
                        self.context,
                        read,
                        row,
                        generator.raw(),
                        pseudo_element,
                        image,
                        images,
                    );
                },
            }
        }
    }
}

/// Whether `style_record`, the style record a box had when a tree build came to owe it its image resources, holds image
/// values, which the box reads as it attaches them.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleImageFacts {
    pub style_record: u64,
    pub holds_image_values: bool,
}

impl FfiStyleImageFacts {
    fn of(engine: &crate::css::style::StyleEngine, style_record: u64) -> Self {
        Self {
            style_record,
            holds_image_values: engine.style_record_holds_image_values(style_record),
        }
    }
}

/// The same predicate the document exposes: an inactive document is left alone, and the arena
/// answers for the tree it holds.
fn layout_is_up_to_date(arena: &LayoutNodeArena, facts: &FfiLayoutUpdateDocumentFacts) -> bool {
    if !facts.document_is_active {
        return true;
    }
    arena.layout_is_up_to_date(facts.document_needs_layout_tree_build)
}

/// Whether the layout of `host`'s document is up to date, as [`layout_is_up_to_date`] answers.
fn host_layout_is_up_to_date(host: &DocumentHost, read: &BegunRead, facts: &FfiLayoutUpdateDocumentFacts) -> bool {
    if !facts.document_is_active {
        return true;
    }
    if let Some(state) = host.known_arena_facts() {
        return state.layout_is_up_to_date_unless_built && !facts.document_needs_layout_tree_build;
    }
    read_arena(host, read, *facts, |arena, facts| layout_is_up_to_date(arena, &facts))
}

const ORDINARY_STABILIZATION_ROUND_LIMIT: u64 = 8;

/// One round of a layout update past its style, which the host sends its document's render state: the layout tree
/// build, where the tree needs one, then a partial relayout of the boundaries the round plans or else a full layout,
/// each committed. The round reads the document's facts as they were before it, and nothing of the host.
///
/// The registered boundaries stay in the arena until the round's layout takes them, so a round that stops after its
/// build leaves them to the job that goes on with it, or to the next round.
pub(crate) struct LayoutRoundJob {
    facts: FfiLayoutUpdateDocumentFacts,
    build: Option<TreeBuildJob>,
    /// The layout the round goes on with, which a round that stopped after its build had chosen.
    layout: RoundLayout,
    /// Whether a size container waits for its queries to be evaluated after the next full layout, which only a round
    /// that may lay out partially reads.
    container_query_evaluation_is_pending: bool,
    container_length_bases: super::layout_pass::ContainerLengthBasesQuery,
}

/// Which layout a round lays its tree out with.
#[derive(Clone, Copy, PartialEq, Eq)]
enum RoundLayout {
    /// A partial relayout of the registered boundaries where the plan allows it, and a full layout otherwise.
    PartialIfPlanned,
    /// A full layout.
    Full,
}

/// Where a layout round ended.
enum LayoutRoundEnd {
    /// The round built the tree and stopped before laying it out: the build asks for another pass,
    /// or the document reconciles stale list item counters with what it rebuilt first.
    Built {
        needs_another_build_pass: bool,
        layout: RoundLayout,
    },
    /// The registered boundaries were laid out again.
    PartialLayout,
    /// The whole document was laid out.
    FullLayout,
}

/// What a layout round owes the host, which it pays in the order of the fields.
pub(crate) struct LayoutRoundAnswer {
    /// The viewport before the round's build, and what the build owes, where the round built the
    /// tree.
    build: Option<(NodeSlotId, TreeBuildAnswer)>,
    /// The host calls the round's tree build and the styles the viewport took over owe, after what
    /// the host hears of the boxes nodes gained and lost in the round.
    work: HostWorkDue,
    /// What committing each layout stage of the round owes.
    commits: Vec<CommitNotifications>,
    end: LayoutRoundEnd,
}

/// The first round of a rendering update's layout, sealed on the host's thread with the facts it read there, to run in
/// the frame the update lets fly.
pub(crate) struct SealedRound {
    job: LayoutRoundJob,
    /// Whether the round builds the tree.
    rebuilds_tree: bool,
}

impl SealedRound {
    /// Runs the round over `state` in the frame's flight, where the frame's style left its layout to do, owing the host
    /// `work` besides what the round owes it. The flight reaches nothing of the host, so a length only the host
    /// resolves leaves the layout for the host to lay out again.
    pub(crate) fn run(self, state: &mut ArenaHandle, work: OwedHostWork) -> Option<FlownRound> {
        if state
            .arena()
            .layout_is_up_to_date(self.job.facts.document_needs_layout_tree_build)
        {
            return None;
        }
        let answer = self.job.run_owing(state, work);
        state.arena().mark_unresolved_container_lengths_for_layout();
        Some(FlownRound {
            answer,
            rebuilds_tree: self.rebuilds_tree,
        })
    }
}

/// What a round that flew owes the host, which the layout update after the frame's landing pays first.
pub(crate) struct FlownRound {
    answer: LayoutRoundAnswer,
    rebuilds_tree: bool,
}

impl FlownRound {
    /// Pays what the round owes `host` in `read`, where no layout update goes on from it.
    ///
    /// # Safety
    ///
    /// The host's layout update callbacks must answer synchronously from its live document.
    pub(crate) unsafe fn pay(mut self, main_thread: &MainThread, host: &DocumentHost, read: &BegunRead) {
        let callbacks = host
            .host_tables()
            .layout_update_host
            .get()
            .expect("the document has no layout update host");
        // SAFETY: Guaranteed by the caller.
        unsafe { self.answer.pay(main_thread, &callbacks, read) };
    }
}

/// The layout round each tick of a clock lease runs once it has shown the samples of the document's animations: the
/// document's facts, and a container length query that answers nothing, sealed where the clock's plan was, at the end of
/// a rendering update that left the layout up to date.
pub(crate) struct ClockRound {
    facts: FfiLayoutUpdateDocumentFacts,
    container_length_bases: super::layout_pass::ContainerLengthBasesQuery,
}

impl ClockRound {
    /// Builds again what the clock frame marked and lays out what it moved, and pushes what the round owes the host to
    /// `owed`, answering it, or nothing where the round laid nothing out. A round declines a build that needs the host
    /// and a length only the host resolves, after it owes the host what it ran.
    pub(crate) fn run<'a>(
        &self,
        state: &mut ArenaHandle,
        owed: &'a mut Vec<LayoutRoundAnswer>,
    ) -> Result<Option<&'a LayoutRoundAnswer>, ClockRoundDeclined> {
        let arena = state.arena();
        if arena.layout_root().is_invalid() || arena.needs_full_layout_tree_update() {
            return Err(ClockRoundDeclined);
        }
        let build = arena
            .document_style_node()
            .filter(|&document| arena.layout_tree_update_marks().borrow().child_needs(document))
            .map(|document| TreeBuildJob::new(document, None));
        if build.is_none() && arena.layout_is_up_to_date(false) {
            return Ok(None);
        }
        let answer = LayoutRoundJob {
            facts: self.facts,
            build,
            layout: RoundLayout::PartialIfPlanned,
            container_query_evaluation_is_pending: false,
            container_length_bases: self.container_length_bases,
        }
        .run(state);
        // A box the build gave an image has none until the host attaches it.
        let declined =
            matches!(answer.end, LayoutRoundEnd::Built { .. }) || state.arena().owes_shown_image_resources_to_host();
        owed.push(answer);
        match state.arena().mark_unresolved_container_lengths_for_layout() || declined {
            true => Err(ClockRoundDeclined),
            false => Ok(owed.last()),
        }
    }
}

/// A clock round whose tree build or layout needs the host: a build that asks for another pass, reconciles list item
/// counters or owes the host image resources, or a length only the host resolves.
pub(crate) struct ClockRoundDeclined;

/// Seals the round the ticks of a clock lease of `document_host`'s document run, where its layout is up to date in
/// `read`.
fn seal_clock_round(main_thread: &MainThread, document_host: &DocumentHost, read: &BegunRead) -> Option<ClockRound> {
    let host = document_host.host_tables().layout_update_host.get()?;
    let facts = host.document_facts(main_thread, read);
    if !facts.document_is_active || !host_layout_is_up_to_date(document_host, read, &facts) {
        return None;
    }
    Some(ClockRound {
        facts,
        container_length_bases: FfiLayoutHostCallbacks::of(main_thread)
            .container_length_bases_query(main_thread)
            .sealed(),
    })
}

impl LayoutRoundJob {
    /// Runs the round over `state`, and resolves what it owes the host as it ends.
    pub(crate) fn run(self, state: &mut ArenaHandle) -> LayoutRoundAnswer {
        self.run_owing(state, OwedHostWork::default())
    }

    /// Runs the round over `state`, and resolves what it owes the host as it ends, `work` among it.
    fn run_owing(self, state: &mut ArenaHandle, work: OwedHostWork) -> LayoutRoundAnswer {
        let mut answer = self.run_with(state, &work);
        answer.work = work.resolve(state.arena());
        answer
    }

    fn run_with(mut self, state: &mut ArenaHandle, work: &OwedHostWork) -> LayoutRoundAnswer {
        let facts = self.facts;
        let mut answer = LayoutRoundAnswer {
            build: None,
            work: HostWorkDue::default(),
            commits: Vec::new(),
            end: LayoutRoundEnd::FullLayout,
        };
        let stage = LayoutStageFacts {
            container_length_bases: self.container_length_bases,
            viewport_inline_size_raw: facts.viewport_inline_size_raw,
            viewport_block_size_raw: facts.viewport_block_size_raw,
            document_in_quirks_mode: facts.document_in_quirks_mode,
        };
        // The round cannot call the host, which hears of the boxes nodes gain and lose once it is over.
        state.arena().queue_box_presence();
        // Attempts to satisfy the round by laying out only the registered partial relayout boundary subtrees. The
        // build runs here when the tree needs one, so a round that is not eligible goes on to the full layout without
        // building again.
        let partial_relayout_facts = FfiPartialRelayoutHostFacts {
            container_query_evaluation_is_pending: self.container_query_evaluation_is_pending,
            should_collect_devtools_layout_data: facts.should_collect_devtools_layout_data,
        };
        let arena = state.arena();
        if self.layout == RoundLayout::PartialIfPlanned
            && arena.partial_relayout_may_be_attempted(
                arena.layout_root(),
                &arena.partial_relayout_boundary_roots.borrow(),
                partial_relayout_facts,
            )
        {
            if let Some(build) = self.build.take() {
                let needs_another_build_pass = Self::build(state, build, work, &mut answer);
                if needs_another_build_pass || facts.has_stale_list_item_counters {
                    answer.end = LayoutRoundEnd::Built {
                        needs_another_build_pass,
                        layout: RoundLayout::PartialIfPlanned,
                    };
                    return answer;
                }
            }

            let arena = state.arena();
            // Among the boundaries are those the build registered where it invalidated what deferred child list
            // insertions reach.
            let boundaries = arena.take_partial_relayout_boundary_roots();
            if let Some(planned) =
                arena.plan_partial_relayout_with_pending_rebuilt_roots(arena.layout_root(), &boundaries)
            {
                for boundary in &planned {
                    debug_assert!(node_facts::kind_is_box(arena.data(boundary.root()).kind.get()));
                }
                sync_enrolled_content_for_layout(state.arena_mut());
                for boundary in planned {
                    answer.commits.push(lay_out_boundary(state, boundary, stage));
                }
                state.arena().note_partial_layout();
                answer.end = LayoutRoundEnd::PartialLayout;
                return answer;
            }
        }

        if let Some(build) = self.build.take() {
            if Self::build(state, build, work, &mut answer) {
                answer.end = LayoutRoundEnd::Built {
                    needs_another_build_pass: true,
                    layout: RoundLayout::Full,
                };
                return answer;
            }
            state.arena().set_needs_full_layout_tree_update(false);
            if facts.has_stale_list_item_counters {
                answer.end = LayoutRoundEnd::Built {
                    needs_another_build_pass: false,
                    layout: RoundLayout::Full,
                };
                return answer;
            }
        }
        // The full layout below covers every registered boundary, the build's among them.
        drop(state.arena().take_partial_relayout_boundary_roots());

        let layout_root = state.arena().layout_root();
        assert!(!layout_root.is_invalid(), "a full layout pass needs a layout root");
        let commit = lay_out_root(
            state,
            work,
            layout_root,
            stage,
            facts.should_collect_devtools_layout_data,
        );
        answer.commits.push(commit);
        state.arena().note_full_layout();
        answer
    }

    /// Runs `build`, records it, and answers whether it asks for another pass.
    fn build(
        state: &mut ArenaHandle,
        build: TreeBuildJob,
        work: &OwedHostWork,
        answer: &mut LayoutRoundAnswer,
    ) -> bool {
        let replaced_viewport = state.arena().layout_root();
        let built = build.run(state, work);
        state.arena().record_layout_tree_build(&built.outcome);
        let needs_another_build_pass = built.outcome.needs_another_build_pass;
        answer.build = Some((replaced_viewport, built));
        needs_another_build_pass
    }
}

impl LayoutRoundAnswer {
    /// The size query containers whose content size the round's layout changed.
    pub(crate) fn resized_size_containers(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.commits
            .iter()
            .flat_map(CommitNotifications::resized_size_containers)
    }

    /// Pays what the round owes the host, in the order it came to owe it. The boxes nodes gained
    /// and lost and the host calls the round owes come first, then a build's commit messages and
    /// styled scroll containers, after which the document retires the tree the build replaced; then
    /// what the layout stages owe.
    ///
    /// # Safety
    ///
    /// The host's callbacks must answer synchronously from its live document.
    pub(crate) unsafe fn pay(
        &mut self,
        main_thread: &MainThread,
        host: &FfiLayoutUpdateHostCallbacks,
        read: &BegunRead,
    ) {
        let layout_host = FfiLayoutHostCallbacks::of(main_thread);
        // SAFETY (for every call below): Guaranteed by the caller.
        std::mem::take(&mut self.work).pay(main_thread);
        if let Some((replaced_viewport, built)) = self.build.take() {
            unsafe {
                layout_host.deliver_commit_messages(main_thread, read, &built.reports);
                layout_host.take_built_scroll_containers(main_thread, read, &built.built_scroll_containers);
            }
            host.finish_layout_tree_build(main_thread, read, replaced_viewport, built.outcome.viewport);
        }
        if self.commits.is_empty() {
            return;
        }
        for commit in self.commits.drain(..) {
            unsafe { commit.notify_host(main_thread, read, &layout_host) };
        }
        super::trace::name_layout_trace_owners(main_thread, read);
    }
}

/// The job the layout update goes on with: one it sends, or what the round that flew answered.
enum NextRound {
    Job(LayoutRoundJob),
    Flown(LayoutRoundAnswer),
}

/// Readies the next round of the layout update of `document_host`'s document, which lays it out as of `facts`, with a
/// tree build where the tree needs one.
fn next_round(
    main_thread: &MainThread,
    document_host: &DocumentHost,
    read: &BegunRead,
    host: &FfiLayoutUpdateHostCallbacks,
    facts: FfiLayoutUpdateDocumentFacts,
) -> SealedRound {
    let layout_host = FfiLayoutHostCallbacks::of(main_thread);
    let rebuilds_tree = facts.document_needs_layout_tree_build
        || read_arena(document_host, read, (), |arena, ()| {
            arena.layout_root().is_invalid() || arena.needs_full_layout_tree_update()
        });
    let build = rebuilds_tree.then(|| {
        let document_style_node = StyleNodeID::from_raw(facts.document_style_node)
            .expect("a document that lays out is named in the style mirror");
        let may_create_viewport = read_arena(
            document_host,
            read,
            document_style_node,
            |arena, document_style_node| arena.tree_build_may_create_viewport(Some(document_style_node)),
        );
        TreeBuildJob::new(
            document_style_node,
            host.prepare_layout_tree_build(main_thread, read, may_create_viewport),
        )
    });
    SealedRound {
        job: LayoutRoundJob {
            facts,
            build,
            layout: RoundLayout::PartialIfPlanned,
            container_query_evaluation_is_pending: host.container_query_evaluation_is_pending(main_thread, read),
            container_length_bases: layout_host.container_length_bases_query(main_thread),
        },
        rebuilds_tree,
    }
}

/// The first round of a rendering update's layout of `document_host`'s document, sealed for a frame to run beside the
/// host, where the document is active and `needs_round` says the round has something to do as of its facts.
///
/// # Safety
///
/// As for [`update_layout`].
unsafe fn sealed_first_round(
    main_thread: &MainThread,
    document_host: &DocumentHost,
    read: &BegunRead,
    inputs: &FfiLayoutUpdateInputs,
    needs_round: impl FnOnce(&FfiLayoutUpdateDocumentFacts) -> bool,
) -> Option<SealedRound> {
    let host = document_host
        .host_tables()
        .layout_update_host
        .get()
        .expect("the document has no layout update host");
    let facts = host.document_facts(main_thread, read);
    if !facts.document_is_active || inputs.is_template_contents_document || !needs_round(&facts) {
        return None;
    }
    let mut round = next_round(main_thread, document_host, read, &host, facts);
    // The round runs beside the host, which it cannot ask.
    round.job.container_length_bases = round.job.container_length_bases.sealed();
    Some(round)
}

/// Seals the first round of a rendering update's layout of `document_host`'s document, which the frame the update lets
/// fly next runs after its style.
///
/// # Safety
///
/// As for [`update_layout`].
unsafe fn seal_first_round(
    main_thread: &MainThread,
    document_host: &DocumentHost,
    read: &BegunRead,
    inputs: &FfiLayoutUpdateInputs,
) {
    // A round is sealed for a document whose layout is up to date as well: the frame's style may leave it layout to
    // do, which the round finds once the frame has applied the style.
    // SAFETY: Guaranteed by the caller.
    if let Some(round) = unsafe { sealed_first_round(main_thread, document_host, read, inputs, |_| true) } {
        document_host.seal_round(round);
    }
}

/// Lets the first round of a rendering update's layout of `document_host`'s document fly beside the host, once the
/// host's style is up to date, where the layout is not and no round that flew with the style waits to be taken in. The
/// host's next layout update pays it first. Answers whether it flies.
///
/// # Safety
///
/// As for [`update_layout`].
unsafe fn fly_first_round(
    main_thread: &MainThread,
    document_host: &DocumentHost,
    read: &BegunRead,
    inputs: &FfiLayoutUpdateInputs,
    license: &FlightLicense,
) -> bool {
    // A round that flew with the style is the layout update's first: the update takes it in and goes on from it here.
    if document_host.has_flown_round() {
        return false;
    }
    // SAFETY: Guaranteed by the caller.
    let round = unsafe {
        sealed_first_round(main_thread, document_host, read, inputs, |facts| {
            !host_layout_is_up_to_date(document_host, read, facts)
        })
    };
    let Some(round) = round else {
        return false;
    };
    crate::render_state::fly(document_host, None, Some(round), license);
    true
}

/// Takes in the layout round that flew in the frame of `document_host`'s document, waiting for it to land, and pays it as
/// a layout update pays its last round, where the round left the document laid out as of the frame. Answers whether it
/// did: no other round runs, so what the host wrote since the frame flew stays the next layout update's.
///
/// # Safety
///
/// As for [`update_layout`].
unsafe fn take_flown_layout_in(main_thread: &MainThread, document_host: &DocumentHost, read: &BegunRead) -> bool {
    let host = document_host
        .host_tables()
        .layout_update_host
        .get()
        .expect("the document has no layout update host");
    document_host.take_frame_in_with(read);
    // SAFETY (for every pay below): Guaranteed by the caller.
    document_host.pay_clock_rounds(|mut answer| unsafe { answer.pay(main_thread, &host, read) });
    let Some(FlownRound {
        mut answer,
        rebuilds_tree,
    }) = document_host.take_flown_round()
    else {
        return false;
    };
    unsafe { answer.pay(main_thread, &host, read) };
    if rebuilds_tree && host.reconcile_stale_list_item_counters_after_tree_build(main_thread, read) {
        return false;
    }
    match answer.end {
        LayoutRoundEnd::Built { .. } => return false,
        LayoutRoundEnd::PartialLayout => host.after_layout_commit(main_thread, read, rebuilds_tree),
        LayoutRoundEnd::FullLayout => {
            host.note_full_layout_performed(main_thread);
            host.after_layout_commit(main_thread, read, true);
            host.evaluate_pending_container_queries(main_thread, read);
        }
    }
    // The style written since the round flew is the next layout update's; only container queries the layout decides
    // restyle what the round laid out.
    !host.container_query_evaluation_is_pending(main_thread, read)
}

/// # Safety
///
/// The layout update host's callbacks must answer synchronously from the live document.
unsafe fn update_layout(
    main_thread: &MainThread,
    document_host: &DocumentHost,
    read: &BegunRead,
    inputs: &FfiLayoutUpdateInputs,
) {
    let host = document_host
        .host_tables()
        .layout_update_host
        .get()
        .expect("the document has no layout update host");
    let layout_host = FfiLayoutHostCallbacks::of(main_thread);
    assert!(
        document_host.host_tables().update_layout_running.get(),
        "the layout update runs between document_host_begin_update_layout and its end"
    );

    // Size-query dependencies point from a descendant to an ancestor query container. They are
    // therefore acyclic, and a coherent style/layout pass can settle at least one more level of
    // a nested dependency chain. One pass per connected element is a conservative exact bound.
    // Recompute it after each pass because an initial style update can enroll the elements of a
    // freshly parsed document after the layout update has already started. The bound is at least
    // the ordinary limit, so the passes within it do not ask for the count.
    let mut layout_pass: u64 = 0;
    while layout_pass <= ORDINARY_STABILIZATION_ROUND_LIMIT
        || layout_pass
            < ORDINARY_STABILIZATION_ROUND_LIMIT + u64::from(host.connected_element_count(main_thread, read)) + 1
    {
        layout_pass += 1;

        host.update_style(main_thread, read);
        // What the ticks of a clock lease the update ended laid out, the host pays before anything else of the update.
        document_host.pay_clock_rounds(|mut answer| unsafe { answer.pay(main_thread, &host, read) });
        // A round that flew in the frame the update took in is the update's first, which the host pays before
        // anything else of the update reads the layout.
        document_host.take_frame_in_with(read);
        let (rebuilds_tree, mut next) = match document_host.take_flown_round() {
            Some(FlownRound { answer, rebuilds_tree }) => (rebuilds_tree, NextRound::Flown(answer)),
            None => {
                host.process_pending_list_item_renumbers(main_thread, read);
                host.process_pending_top_layer_layout_changes(main_thread, read);

                let facts = host.document_facts(main_thread, read);
                let force_devtools_layout_data_collection =
                    facts.should_collect_devtools_layout_data && inputs.reason_is_inspect_devtools_layout_data;
                if host_layout_is_up_to_date(document_host, read, &facts) && !force_devtools_layout_data_collection {
                    host.prepare_for_rendering(main_thread, read);
                    return;
                }

                // NOTE: If this is a document hosting <template> contents, layout is unnecessary.
                if inputs.is_template_contents_document {
                    return;
                }

                let round = next_round(main_thread, document_host, read, &host, facts);
                (round.rebuilds_tree, NextRound::Job(round.job))
            }
        };
        let end = loop {
            let mut answer = match next {
                NextRound::Flown(answer) => answer,
                NextRound::Job(job) => run_layout_round_job(document_host, read, job),
            };
            unsafe { answer.pay(main_thread, &host, read) };
            let built = matches!(answer.end, LayoutRoundEnd::Built { .. });
            if rebuilds_tree && host.reconcile_stale_list_item_counters_after_tree_build(main_thread, read) {
                // The stale counters marked more of the tree for a rebuild, which the next round runs.
                debug_assert!(
                    built,
                    "only a round that stops after its build reconciles counters that matter"
                );
                break None;
            }
            let LayoutRoundEnd::Built {
                needs_another_build_pass: false,
                layout,
            } = answer.end
            else {
                break Some(answer.end);
            };
            // The round goes on to lay out what it built, as of the document's facts now: paying for the build may
            // have resized this document's viewport through its embedding document.
            next = NextRound::Job(LayoutRoundJob {
                facts: host.document_facts(main_thread, read),
                build: None,
                layout,
                container_query_evaluation_is_pending: layout == RoundLayout::PartialIfPlanned
                    && host.container_query_evaluation_is_pending(main_thread, read),
                container_length_bases: layout_host.container_length_bases_query(main_thread),
            });
        };

        match end {
            None | Some(LayoutRoundEnd::Built { .. }) => continue,
            Some(LayoutRoundEnd::PartialLayout) => {
                // A round that built the tree changed it, whichever of its jobs ran the build.
                host.after_layout_commit(main_thread, read, rebuilds_tree);
                if host.needs_style_update_after_layout(main_thread, read)
                    || !host_layout_is_up_to_date(document_host, read, &host.document_facts(main_thread, read))
                {
                    continue;
                }
                return;
            }
            Some(LayoutRoundEnd::FullLayout) => {}
        }

        host.note_full_layout_performed(main_thread);
        host.after_layout_commit(main_thread, read, true);

        host.evaluate_pending_container_queries(main_thread, read);

        if host.needs_style_update_after_layout(main_thread, read) {
            continue;
        }

        let facts = host.document_facts(main_thread, read);
        // A zone rebuild requested during layout tree construction runs as another pass.
        if facts.top_layer_work_pending {
            continue;
        }

        // Layout-only invalidations still need to be flushed before we can exit.
        if host_layout_is_up_to_date(document_host, read, &facts) {
            break;
        }
    }

    if host.needs_style_update_after_layout(main_thread, read)
        || !host_layout_is_up_to_date(document_host, read, &host.document_facts(main_thread, read))
    {
        host.record_stabilization_bound_failure(main_thread);
        unreachable!("the layout update did not stabilize within its exact bound");
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The callbacks must remain valid until they are
/// cleared or the host is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_layout_update_host_callbacks(
    host: &DocumentHost,
    callbacks: FfiLayoutUpdateHostCallbacks,
) {
    host.host_tables().layout_update_host.set(Some(callbacks));
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, with no layout update running. A document runs
/// one layout update at a time; a nested request is a caller bug.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_begin_update_layout(host: &DocumentHost) {
    let running = &host.host_tables().update_layout_running;
    assert!(!running.replace(true), "a layout update is already running");
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, with a layout update running.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_end_update_layout(host: &DocumentHost) {
    let running = &host.host_tables().update_layout_running;
    assert!(running.replace(false), "no layout update is running");
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_update_layout_is_running(host: &DocumentHost) -> bool {
    host.host_tables().update_layout_running.get()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::tree_builder::FfiLayoutTreeBuildOutcome;

    fn facts_for() -> FfiLayoutUpdateDocumentFacts {
        FfiLayoutUpdateDocumentFacts {
            document_is_active: true,
            document_needs_layout_tree_build: false,
            top_layer_work_pending: false,
            should_collect_devtools_layout_data: false,
            document_in_quirks_mode: false,
            viewport_inline_size_raw: 0,
            viewport_block_size_raw: 0,
            document_style_node: 0,
            has_stale_list_item_counters: false,
        }
    }

    #[test]
    fn an_inactive_document_counts_as_laid_out_and_a_rootless_active_one_does_not() {
        let arena = LayoutNodeArena::new();
        let mut facts = facts_for();
        assert!(!layout_is_up_to_date(&arena, &facts));
        facts.document_is_active = false;
        assert!(layout_is_up_to_date(&arena, &facts));
    }

    #[test]
    fn dom_side_pending_work_keeps_a_rooted_document_from_being_laid_out() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_unbound();
        arena.set_layout_root(viewport);
        arena.reset_layout_update_flags_in_subtree(viewport);
        let facts = facts_for();
        assert!(layout_is_up_to_date(&arena, &facts));

        let mut tree_build_pending = facts;
        tree_build_pending.document_needs_layout_tree_build = true;
        assert!(!layout_is_up_to_date(&arena, &tree_build_pending));

        arena.set_needs_full_layout_tree_update(true);
        assert!(!layout_is_up_to_date(&arena, &facts));
        arena.set_needs_full_layout_tree_update(false);
        assert!(layout_is_up_to_date(&arena, &facts));

        arena
            .free_subtree(viewport)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
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
