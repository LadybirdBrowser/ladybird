/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A document's style transaction as a job its render state runs: the host hands it the transaction's root and the
//! document's computation inputs, and the job answers with the reactions the host applies, which the host keeps until
//! it ends the transaction.

use super::StyleEngine;
use super::bridge::{
    FfiCustomFunctionEntry, FfiDocumentStyleComputationInputs, FfiHostHandle, FfiStyleSheetResourceContextEntry,
    FfiStyleTransactionOutput, FfiStyleTransactionView, take_style_transaction,
};
use super::engine_calls::{StyleAnswer, StyleQuery, ask_engine};
use super::identities::StyleNodeIdAllocator;
use super::tree::StyleNodeID;
use crate::css::custom_properties::{CustomPropertyRegistry, retain_custom_property_registry};
use crate::css::parser::query_parser::FfiMediaFeatureValue;
use crate::css::rule::CompiledFunction;
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::painting::ffi::FfiFlightBlocker;
use crate::painting::recording_slot::FlightLicense;
use crate::render_state::{
    BegunRead, DocumentHost, ReadRight, RenderJob, RenderState, RenderWait, StyleJobPermit, TaskBoundary, fly,
    force_read, run_job,
};
use std::sync::Arc;

/// Takes the pending style transaction under `root`, with the document computation inputs the host sealed for it.
pub(crate) struct StyleJob {
    root: StyleNodeID,
    computation_inputs: SealedStyleInputs,
    /// Whether the transaction flies beside the host, which leaves its atom sweep to a later transaction: the host may
    /// name an atom meanwhile that the sweep would reclaim before the host hears of it.
    flies: bool,
}

/// A style transaction's document computation inputs, sealed on the host's thread: the job owns a copy of every buffer
/// the host lent with them, and a reference to each custom function and to the custom-property registry they name, so
/// it reads nothing the host goes on writing.
pub(crate) struct SealedStyleInputs {
    /// The inputs, whose borrowed fields name the buffers below.
    inputs: FfiDocumentStyleComputationInputs,
    _document_base_url: Box<[u8]>,
    _resource_context_urls: Box<[u8]>,
    _resource_contexts: Box<[FfiStyleSheetResourceContextEntry]>,
    _media_feature_values: Box<[FfiMediaFeatureValue]>,
    _media_length_resolution_context: Option<Box<FfiLengthResolutionContext>>,
    _custom_functions: Box<[FfiCustomFunctionEntry]>,
    _retained_functions: Box<[Arc<CompiledFunction>]>,
    _custom_property_registry: Option<Arc<CustomPropertyRegistry>>,
}

// SAFETY: Every pointer the sealed inputs hold names one of their own buffers, a custom function they hold a
// reference to, or the registry they hold one to, none of which the host writes.
unsafe impl Send for SealedStyleInputs {}

/// The `count` values at `values`, or none.
///
/// # Safety
///
/// `values` must be null or point at `count` readable values.
unsafe fn lent_values<'a, T>(values: *const T, count: usize) -> &'a [T] {
    if values.is_null() || count == 0 {
        return &[];
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { std::slice::from_raw_parts(values, count) }
}

impl SealedStyleInputs {
    /// Seals the inputs `lent` and what their borrowed fields name.
    ///
    /// # Safety
    ///
    /// The borrowed fields of `lent` must name live buffers of their stated lengths, live custom functions and the
    /// document's live custom-property registry, or nothing, for this call.
    unsafe fn seal(lent: FfiDocumentStyleComputationInputs) -> Self {
        // SAFETY: Guaranteed by the caller.
        let (document_base_url, lent_contexts, media_feature_values, lent_functions) = unsafe {
            (
                Box::<[u8]>::from(lent_bytes(lent.document_base_url, lent.document_base_url_length)),
                lent_values(
                    lent.style_sheet_resource_contexts
                        .as_pointer()
                        .cast::<FfiStyleSheetResourceContextEntry>(),
                    lent.style_sheet_resource_context_count,
                ),
                Box::<[FfiMediaFeatureValue]>::from(lent_values(
                    lent.media_feature_values.as_pointer().cast::<FfiMediaFeatureValue>(),
                    lent.media_feature_value_count,
                )),
                lent_values(
                    lent.custom_functions.as_pointer().cast::<FfiCustomFunctionEntry>(),
                    lent.custom_function_count,
                ),
            )
        };
        // Every context's base URL goes into one buffer.
        let resource_context_urls: Box<[u8]> = lent_contexts
            .iter()
            // SAFETY: Guaranteed by the caller.
            .flat_map(|entry| unsafe { lent_values(entry.base_url, entry.base_url_length) })
            .copied()
            .collect();
        let mut offset = 0;
        let resource_contexts: Box<[_]> = lent_contexts
            .iter()
            .map(|entry| {
                let base_url = resource_context_urls[offset..].as_ptr();
                offset += entry.base_url_length;
                FfiStyleSheetResourceContextEntry {
                    source_identity: entry.source_identity,
                    base_url,
                    base_url_length: entry.base_url_length,
                    has_base_url: entry.has_base_url,
                    origin_clean: entry.origin_clean,
                }
            })
            .collect();
        // The output flag of the length context is the host's, and the engine keeps none.
        // SAFETY: Guaranteed by the caller.
        let media_length_resolution_context = unsafe {
            lent.media_length_resolution_context
                .as_pointer()
                .cast::<FfiLengthResolutionContext>()
                .as_ref()
        }
        .map(|&context| {
            Box::new(FfiLengthResolutionContext {
                resolved_viewport_relative_length: std::ptr::null_mut(),
                ..context
            })
        });
        let custom_functions: Box<[_]> = lent_functions
            .iter()
            .map(|entry| FfiCustomFunctionEntry {
                function: entry.function,
                caller_scope: entry.caller_scope,
                definition_scope: entry.definition_scope,
                tree_scope: entry.tree_scope,
            })
            .collect();
        let retained_functions = lent_functions
            .iter()
            .filter(|entry| !entry.function.is_null())
            .map(|entry| {
                let function = entry.function.cast::<CompiledFunction>();
                // SAFETY: Guaranteed by the caller: the host lends a live compiled function, shared by reference count.
                unsafe {
                    Arc::increment_strong_count(function);
                    Arc::from_raw(function)
                }
            })
            .collect();
        // SAFETY: Guaranteed by the caller.
        let custom_property_registry =
            unsafe { retain_custom_property_registry(lent.custom_property_registry.as_pointer()) };
        let inputs = FfiDocumentStyleComputationInputs {
            document_base_url: FfiHostHandle::from_pointer(document_base_url.as_ptr().cast()),
            style_sheet_resource_contexts: FfiHostHandle::from_pointer(resource_contexts.as_ptr().cast()),
            media_feature_values: FfiHostHandle::from_pointer(media_feature_values.as_ptr().cast()),
            media_length_resolution_context: media_length_resolution_context
                .as_deref()
                .map_or_else(FfiHostHandle::default, |context| {
                    FfiHostHandle::from_pointer(std::ptr::from_ref(context).cast())
                }),
            custom_functions: FfiHostHandle::from_pointer(custom_functions.as_ptr().cast()),
            ..lent
        };
        Self {
            inputs,
            _document_base_url: document_base_url,
            _resource_context_urls: resource_context_urls,
            _resource_contexts: resource_contexts,
            _media_feature_values: media_feature_values,
            _media_length_resolution_context: media_length_resolution_context,
            _custom_functions: custom_functions,
            _retained_functions: retained_functions,
            _custom_property_registry: custom_property_registry,
        }
    }
}

/// The `length` bytes at the host's `bytes`, or none.
///
/// # Safety
///
/// `bytes` must be null or name `length` readable bytes.
unsafe fn lent_bytes<'a>(bytes: FfiHostHandle, length: usize) -> &'a [u8] {
    // SAFETY: Guaranteed by the caller.
    unsafe { lent_values(bytes.as_pointer().cast::<u8>(), length) }
}

/// What a style job answers: the reactions of the transaction, owned, which the host keeps until it ends the
/// transaction.
pub(crate) struct StyleJobAnswer {
    output: FfiStyleTransactionOutput,
    records: NamedRecords,
    /// The synthetic pseudo-elements each element whose row the host composes has rules for, as a C++ record's
    /// pseudo-style mask, and those among them that no settle generates or changes, by element, which the host reads as
    /// it settles the element's pseudo-elements over the composition.
    composed_pseudo_styles: Box<[(u32, ComposedPseudoStyles)]>,
    /// The transition steps the transaction decided beside the rows that owe them, by element, which the host's steps
    /// read rather than ask.
    decided_transition_steps: Box<[crate::css::transition::DecidedTransitionStep]>,
}

/// What a style transaction answers of the pseudo-elements of an element whose row the host composes.
#[derive(Clone, Copy)]
pub(crate) struct ComposedPseudoStyles {
    /// The synthetic pseudo-elements with rules, as a C++ record's pseudo-style mask.
    pub(crate) mask: u64,
    /// The pseudo-elements in `mask` that a settle over any composition of the element leaves alone.
    pub(crate) inert: u64,
}

/// The view of each record a transaction's rows name and the custom-property environment it was computed in, by record,
/// which the host's drain reads without asking.
struct NamedRecords(Box<[(u64, super::bridge::FfiStyleRecordView, Option<u64>)]>);

// SAFETY: A view points into a published record, which never changes while it is live, and the host reads one only for
// a record it installs from the transaction, which keeps it live, or one a row replaces, which the engine reclaims no
// sooner than its next transaction.
unsafe impl Send for NamedRecords {}

impl StyleJobAnswer {
    /// The rows the transaction answered.
    pub(crate) fn rows(&self) -> &[super::bridge::FfiStyleDelta] {
        self.output.answers()
    }

    /// The view of `record`, one the rows name, and the custom-property environment it was computed in.
    pub(crate) fn record(&self, record: u64) -> Option<(super::bridge::FfiStyleRecordView, Option<u64>)> {
        let records = &self.records.0;
        let index = records.binary_search_by_key(&record, |&(record, ..)| record).ok()?;
        let (_, view, environment) = records[index];
        Some((view, environment))
    }

    /// What the transaction answered of the pseudo-elements of `node`, where the host composes its row.
    pub(crate) fn composed_pseudo_styles(&self, node: u32) -> Option<ComposedPseudoStyles> {
        let styles = &self.composed_pseudo_styles;
        let index = styles.binary_search_by_key(&node, |&(node, _)| node).ok()?;
        Some(styles[index].1)
    }

    /// The transition step the transaction decided beside the row of `node`, if it decided one.
    pub(crate) fn decided_transition_step(&self, node: u32) -> Option<&crate::css::transition::DecidedTransitionStep> {
        let steps = &self.decided_transition_steps;
        let index = steps.binary_search_by_key(&node, |step| step.node()).ok()?;
        Some(&steps[index])
    }
}

impl RenderJob for StyleJob {
    type Answer = StyleJobAnswer;
    type Permit = StyleJobPermit;
    const IS_STYLE: bool = true;

    fn run_on(self, state: &mut RenderState) -> StyleJobAnswer {
        self.run(state.engine_mut())
    }
}

impl StyleJob {
    /// Runs the job on `engine`, the engine of the document it was sent for.
    pub(crate) fn run(self, engine: &mut StyleEngine) -> StyleJobAnswer {
        engine.defer_atom_sweep(self.flies);
        // SAFETY: The sealed inputs name only what they own, and live until the transaction has taken them in.
        let output = unsafe { take_style_transaction(engine, self.root, self.computation_inputs.inputs) };
        engine.defer_atom_sweep(false);
        // The host reads the record each row replaces as well, as it compares a box's old style with its new one. Only a
        // live base record comes along: the host asks about a replaced overlay record itself.
        let mut records: Vec<u64> = output
            .answers()
            .iter()
            .flat_map(|row| {
                let old = Some(row.old_style_record)
                    .filter(|&old| engine.computed_group_sets.final_style_record_is_live(old));
                [old, Some(row.new_style_record)]
            })
            .flatten()
            .filter(|&record| record != 0)
            .collect();
        records.sort_unstable();
        records.dedup();
        let records = records
            .into_iter()
            .filter_map(|record| {
                // SAFETY: The view points into the record, which stays live while the rows name it.
                let view = unsafe { super::bridge::style_record_view(engine, record) };
                view.present.then(|| {
                    let environment = engine
                        .computed_group_sets
                        .style_record_custom_property_environment(record);
                    (record, view, environment)
                })
            })
            .collect();
        // The host settles the pseudo-elements of a row it composes only once it has composed the row, which asks
        // which of them have rules, and which of those the settle leaves alone: the transaction that matched the
        // element answers it beside the row.
        let mut composed_pseudo_styles: Vec<_> = output
            .answers()
            .iter()
            .filter(|row| row.composed_by_the_host)
            .filter_map(|row| {
                let node = StyleNodeID::from_raw(row.style_node)?;
                let styles = ComposedPseudoStyles {
                    mask: engine.published_pseudo_style_mask(node),
                    inert: engine.inert_pseudo_kinds(node),
                };
                Some((row.style_node, styles))
            })
            .collect();
        composed_pseudo_styles.sort_unstable_by_key(|&(node, _)| node);
        // The host's transition step for a row that owes one asks how the row's move starts the element's transitions,
        // which the transaction answers beside the row where nothing but the engine's records decides it.
        let mut decided_transition_steps: Vec<_> = output
            .answers()
            .iter()
            .filter_map(|row| crate::css::transition::DecidedTransitionStep::of_row(&mut *engine, row))
            .collect();
        decided_transition_steps.sort_unstable_by_key(crate::css::transition::DecidedTransitionStep::node);
        StyleJobAnswer {
            output,
            records: NamedRecords(records),
            composed_pseudo_styles: composed_pseudo_styles.into_boxed_slice(),
            decided_transition_steps: decided_transition_steps.into_boxed_slice(),
        }
    }
}

/// Runs a style job for the transaction under `root`, and answers what the host reads of its reactions, which stays
/// valid until the host ends the transaction.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the buffers `computation_inputs` names must be
/// live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_take_style_transaction(
    host: *mut DocumentHost,
    read: &BegunRead,
    root: u32,
    computation_inputs: FfiDocumentStyleComputationInputs,
) -> FfiStyleTransactionView {
    let Some(root) = StyleNodeID::from_raw(root) else {
        return FfiStyleTransactionView::default();
    };
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let job = StyleJob {
        root,
        // SAFETY: Guaranteed by the caller.
        computation_inputs: unsafe { SealedStyleInputs::seal(computation_inputs) },
        flies: false,
    };
    // The first style transaction of a read the host waits for is the read's first job; any other, a rendering
    // update's or a later wave's, is a style update's.
    let answer = match host.take_unstyled_read() {
        Some(read) => force_read(read, host, job),
        None => run_job(StyleJobPermit::of_style_update(read), host, job),
    };
    host.keep_style_transaction(answer).output.view()
}

/// Lets the pending style transaction under `root` fly beside the host, with the document computation inputs the host
/// gathered, where `blocker` is none and no transaction the host let fly before waits to be drained. The host's next
/// style update drains its reactions first. Answers whether the transaction flies.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the buffers `computation_inputs` names must be
/// live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_let_style_transaction_fly(
    host: *mut DocumentHost,
    root: u32,
    computation_inputs: FfiDocumentStyleComputationInputs,
    blocker: FfiFlightBlocker,
) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    // The round the host sealed for the frame flies with it, or not at all.
    let round = host.take_sealed_round();
    let (Some(root), Some(license)) = (StyleNodeID::from_raw(root), FlightLicense::for_blocker(blocker)) else {
        return false;
    };
    if host.has_flown_style() {
        return false;
    }
    let job = StyleJob {
        root,
        // SAFETY: Guaranteed by the caller.
        computation_inputs: unsafe { SealedStyleInputs::seal(computation_inputs) },
        flies: true,
    };
    fly(host, job, round, &license);
    true
}

/// Takes in the style transaction of `host`'s document that the host let fly, waiting for it to land, for the host to
/// drain its reactions against the inputs it was sealed with. What the host wrote beside the transaction reaches the
/// render state only once style_engine_end_flown_style_drain() ends the drain: it is the next transaction's.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, that let a style transaction fly.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_take_flown_style_transaction(
    host: *const DocumentHost,
    read: &BegunRead,
) -> FfiStyleTransactionView {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    // The read spends itself on the transaction where no job took it yet, as on the first job it would have sent,
    // taking the frame in where it still flies.
    let read = host
        .take_unstyled_read()
        .map_or_else(|| read.into_read_right(), ReadRight::Forced);
    let answer = host.begin_style_drain(read);
    host.keep_style_transaction(answer).output.view()
}

/// Ends the drain of the style transaction of `host`'s document that flew, behind which the writes the host made beside
/// it reach the render state.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, that drains a style transaction that flew.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_end_flown_style_drain(host: *const DocumentHost) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.end_style_drain();
}

/// Whether the frame whose style transaction `host`'s document drains applied the element `style_node` names its record
/// `style_record` ahead of the host, and marked the relayout the move asks for: the host's install of the row marks none
/// then.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_frame_marked_relayout(
    host: *const DocumentHost,
    style_node: u32,
    style_record: u64,
) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    StyleNodeID::from_raw(style_node)
        .is_some_and(|style_node| unsafe { &*host }.frame_marked_relayout(style_node, style_record))
}

/// Whether the style transaction of `host`'s document the host let fly still flies; one that has landed is taken in.
/// The event loop asks between two tasks, so this never waits.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the event loop must call this between two tasks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_transaction_flies(host: *const DocumentHost) -> bool {
    assert!(!host.is_null(), "document host is null");
    let boundary = TaskBoundary::at_event_loop_entry(&TAKES_FINISHED_STYLE_IN);
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.frame_still_flies(&boundary)
}

/// The entry the event loop calls between two tasks to take a document's style transaction in where it has landed.
pub(crate) struct TakesFinishedStyleIn {
    _private: (),
}

const TAKES_FINISHED_STYLE_IN: TakesFinishedStyleIn = TakesFinishedStyleIn { _private: () };

/// Ends the transaction the host took last, and has `allocator` mint the identities its end released again first.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `allocator` its document's allocator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_end_style_transaction(
    host: *mut DocumentHost,
    read: &crate::render_state::BegunRead,
    allocator: *mut StyleNodeIdAllocator,
) {
    assert!(!host.is_null() && !allocator.is_null());
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.end_style_transaction();
    // SAFETY: As above.
    let StyleAnswer::Nodes(released) = (unsafe { ask_engine(host, read, StyleQuery::EndTransaction) }) else {
        unreachable!("the end of a transaction is answered with the identities it released");
    };
    // SAFETY: As above.
    let memo = unsafe { &*host }.engine_memo();
    memo.held.borrow_mut().forget(&released);
    memo.described.borrow_mut().forget(&released);
    memo.baselines.borrow_mut().forget(&released);
    // SAFETY: As above.
    unsafe { &mut *allocator }.release(&released);
}
