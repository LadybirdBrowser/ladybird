/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A document's style update as a job its render state runs: the host hands it the transaction's root and the
//! document's computation inputs, and the job answers with the reactions the host applies, which the host keeps until
//! it ends the transaction.

use super::StyleEngine;
use super::bridge::{
    FfiCustomFunctionEntry, FfiDocumentStyleComputationInputs, FfiHostHandle, FfiStyleSheetResourceContextEntry,
    FfiStyleUpdateOutput, run_style_update,
};
use super::tree::StyleNodeID;
use crate::css::custom_properties::{CustomPropertyRegistry, retain_custom_property_registry};
use crate::css::parser::query_parser::FfiMediaFeatureValue;
use crate::css::rule::CompiledFunction;
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::painting::ffi::FfiFlightBlocker;
use crate::painting::recording_slot::FlightLicense;
use crate::render_state::{BegunRead, DocumentHost, FrameJob, TaskBoundary, fly};
use std::sync::Arc;

/// Takes the pending style update under `root`, with the document computation inputs the host sealed for it.
pub(crate) struct StyleJob {
    root: StyleNodeID,
    computation_inputs: Arc<SealedStyleInputs>,
    /// Whether the transaction flies beside the host, which leaves its atom sweep to a later transaction: the host may
    /// name an atom meanwhile that the sweep would reclaim before the host hears of it.
    flies: bool,
}

/// A style update's document computation inputs, sealed on the host's thread: the job owns a copy of every buffer
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
// SAFETY: As above; nothing writes them once sealed, so threads that share them only read.
unsafe impl Sync for SealedStyleInputs {}

impl SealedStyleInputs {
    /// The inputs, which name the buffers they own, for as long as they live.
    pub(crate) fn inputs(&self) -> FfiDocumentStyleComputationInputs {
        self.inputs
    }
}

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
    unsafe { ak::slice_from_raw_parts(values, count) }
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
        let mut resource_context_urls =
            Vec::with_capacity(lent_contexts.iter().map(|entry| entry.base_url_length).sum());
        for entry in lent_contexts {
            // SAFETY: Guaranteed by the caller.
            resource_context_urls.extend_from_slice(unsafe { lent_values(entry.base_url, entry.base_url_length) });
        }
        let resource_context_urls = resource_context_urls.into_boxed_slice();
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
        let custom_functions = Box::<[FfiCustomFunctionEntry]>::from(lent_functions);
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
    output: FfiStyleUpdateOutput,
    records: NamedRecords,
    /// The transition steps the transaction decided beside the rows that owe them, by element, which the host's steps
    /// read rather than ask.
    decided_transition_steps: Box<[crate::css::transition::DecidedTransitionStep]>,
    /// What the main thread does to take the transaction's style installation, until it takes it.
    installation_for_host: super::pipeline::install::StyleInstallation,
    /// What the render side installed of the style rows in the boxes of their elements itself, which the host adopts
    /// as it installs the rows, without publishing them again.
    styled_rows: crate::layout::StyledRows,
    /// The styles installed for every element and pseudo-element once the transaction installed its rows.
    installed_styles: super::computed::InstalledStyles,
}

/// A record a transaction's rows name, with the record the host holds for it.
type NamedRecord = (u64, std::sync::Arc<super::pipeline::Record>);

/// The record the host holds for each record a transaction's rows name, by record, which the host's drain reads without
/// asking. Holding the records keeps them live for the host until the next transaction.
struct NamedRecords {
    records: Box<[NamedRecord]>,
    /// Where the last lookup found its record. The host installs a transaction's rows in tree order, the order in
    /// which their new records were minted, so the next lookup mostly finds the record at or just past it.
    cursor: std::cell::Cell<usize>,
}

impl StyleJobAnswer {
    /// The styles installed for every element and pseudo-element once the transaction installed its rows: the records
    /// the rows install, and those of every element the transaction left as it was.
    pub(crate) fn installed_styles(&self) -> &super::computed::InstalledStyles {
        &self.installed_styles
    }

    /// The rows the transaction answered.
    pub(crate) fn rows(&self) -> &[super::bridge::FfiStyleDelta] {
        self.output.answers()
    }

    /// The atoms the engine reclaimed as it took the transaction.
    pub(crate) fn reclaimed_style_atoms(&self) -> &[super::atoms::ReclaimedStyleAtom] {
        self.output.reclaimed_style_atoms()
    }

    /// The record the host holds for `record`, one the rows name.
    pub(crate) fn record(&self, record: u64) -> Option<*const std::ffi::c_void> {
        let NamedRecords { records, cursor } = &self.records;
        let near = cursor.get();
        let index = (near..records.len().min(near + 4))
            .find(|&index| records[index].0 == record)
            .or_else(|| records.binary_search_by_key(&record, |&(record, _)| record).ok())?;
        cursor.set(index);
        Some(std::sync::Arc::as_ptr(&records[index].1).cast())
    }

    /// Takes what the main thread does to take the transaction's style installation.
    pub(crate) fn take_installation(&mut self) -> super::pipeline::install::StyleInstallation {
        self.installation_for_host.take_for_the_host()
    }

    /// Notes, for each element whose style record the transaction installed, whether its box is one only the host
    /// styles in `arena`, the layout arena the host installs the records into.
    pub(crate) fn note_host_styled_boxes(&mut self, arena: &crate::layout::LayoutNodeArena) {
        for row in &mut self.installation_for_host.effects {
            let slot = arena.bound_row(row.node());
            if !slot.is_invalid() && arena.box_is_styled_by_host(slot) {
                row.effects |= super::bridge::MainEffects::HOST_BOX_FACTS;
            }
        }
    }

    /// How the boxes of `node`, an element whose style record the transaction installed, take it: they are built
    /// again from the root the move names, or the host installs it where they take the composition of its animations, or
    /// appear or go.
    pub(crate) fn element_install(&self, node: StyleNodeID) -> crate::layout::ElementInstall {
        use super::bridge::MainEffects;
        use crate::layout::ElementInstall;
        let Some(row) = self.installation_for_host.effects_of(node) else {
            return ElementInstall::Boxes;
        };
        if row
            .effects
            .intersects(MainEffects::ANIMATED | MainEffects::DISPLAY_NONE_FLIP)
        {
            ElementInstall::Host
        } else if row.effects.intersects(MainEffects::REBUILD) {
            ElementInstall::Rebuild(row.rebuild_root)
        } else {
            ElementInstall::Boxes
        }
    }

    /// Notes `styled`, what the render side installed of the transaction's style rows.
    pub(crate) fn note_styled(&mut self, styled: crate::layout::StyledRows) {
        debug_assert!(
            self.styled_rows.pinned_records.is_empty() && self.styled_rows.rebuilt.is_empty(),
            "the render side installs a transaction once"
        );
        use super::bridge::MainEffects;
        for row in &mut self.installation_for_host.effects {
            let node = row.node();
            // The render side marked the rebuilds of these elements, which leaves the host none to mark.
            if styled.rebuilt.binary_search(&node).is_ok() {
                row.effects = row.effects.without(MainEffects::REBUILD);
            }
            // The host installs the records in the boxes of the elements the render side left to it, and of those the
            // main thread held no style for.
            let installed = styled
                .elements
                .binary_search_by_key(&node, |element| element.node)
                .is_ok();
            if !installed || !row.moves_from_a_record() {
                row.effects |= MainEffects::HOST_BOX_FACTS;
            }
        }
        self.styled_rows = styled;
        self.installation_for_host.keep_effects_with_host_work();
    }

    /// Whether the render side installed `style_record`, the style record of the element `node`, in its boxes, and
    /// those of its pseudo-elements, with the relayout and repaint the move asks for.
    pub(crate) fn render_state_installed(&self, node: StyleNodeID, style_record: u64) -> bool {
        let elements = &self.styled_rows.elements;
        elements
            .binary_search_by_key(&node, |element| element.node)
            .is_ok_and(|index| elements[index].style_record == style_record)
    }

    /// The record the render side installed in the box `slot`, if it styled it.
    pub(crate) fn render_state_installed_box(&self, slot: crate::layout::node_data::NodeSlotId) -> Option<u64> {
        let boxes = &self.styled_rows.boxes;
        let index = boxes
            .binary_search_by_key(&slot.slot_index(), |styled| styled.slot.slot_index())
            .ok()?;
        (boxes[index].slot == slot).then_some(boxes[index].style_record)
    }

    /// Takes the records the render side pinned for the host as it installed the transaction's rows, which the host
    /// unpins as it ends the transaction.
    pub(crate) fn take_pinned_records(&mut self) -> Vec<u64> {
        let mut pinned = std::mem::take(&mut self.styled_rows.pinned_records);
        pinned.append(&mut self.installation_for_host.pinned_records);
        pinned
    }

    /// The transition step the transaction decided beside the row of `node`, if it decided one.
    pub(crate) fn decided_transition_step(&self, node: u32) -> Option<&crate::css::transition::DecidedTransitionStep> {
        let steps = &self.decided_transition_steps;
        let index = steps.binary_search_by_key(&node, |step| step.node()).ok()?;
        Some(&steps[index])
    }
}

impl StyleJob {
    /// Runs the job on `engine`, the engine of the document it was sent for.
    pub(crate) fn run(self, engine: &mut StyleEngine) -> StyleJobAnswer {
        // What an installation the host never took, as one a flush threw away made, has the main thread do is not this
        // transaction's.
        for record in engine.take_installation().pinned_records {
            engine.unpin_layout_style_record(record);
        }
        engine.defer_atom_sweep(self.flies);
        // SAFETY: The sealed inputs name only what they own, and live until the transaction has taken them in.
        let output = unsafe { run_style_update(engine, self.root, self.computation_inputs.inputs) };
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
            .filter_map(|record| Some((record, engine.computed_group_sets.host_record(record)?.clone())))
            .collect();
        // The host's transition step for a row that owes one asks how the row's move starts the element's transitions,
        // which the transaction answers beside the row where nothing but the engine's records decides it.
        let mut decided_transition_steps: Vec<_> = output
            .answers()
            .iter()
            .filter_map(|row| crate::css::transition::DecidedTransitionStep::of_row(&mut *engine, row))
            .collect();
        decided_transition_steps.sort_unstable_by_key(crate::css::transition::DecidedTransitionStep::node);
        let installation_for_host = engine.take_installation();
        // The render side knows what it publishes: the host swaps the installation's records in without echoing them.
        installation_for_host.note_held_by_the_host(engine);
        let installed_styles = engine.computed_group_sets.publish_installed_styles();
        let answer = StyleJobAnswer {
            output,
            records: NamedRecords {
                records,
                cursor: std::cell::Cell::new(0),
            },
            decided_transition_steps: decided_transition_steps.into_boxed_slice(),
            installation_for_host,
            styled_rows: Default::default(),
            installed_styles,
        };
        if engine.process_style.verifies_reads {
            verify_installed_styles(answer.installed_styles(), answer.rows());
        }
        answer
    }
}

/// Checks that `styles` install what `rows` do.
fn verify_installed_styles(styles: &super::computed::InstalledStyles, rows: &[super::bridge::FfiStyleDelta]) {
    for row in rows {
        let Some(node) = StyleNodeID::from_raw(row.style_node) else {
            continue;
        };
        let installed = match row.pseudo_kind {
            u8::MAX => styles.element(node),
            kind => styles.pseudo_element(node, kind),
        };
        let installed = installed.map_or(0, |style| style.final_style_record().raw());
        if installed != row.new_style_record {
            eprintln!(
                "the installed styles hold record {installed} for node {} (pseudo-element {}), the row installs {}",
                row.style_node, row.pseudo_kind, row.new_style_record
            );
            std::process::abort();
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
pub unsafe extern "C" fn style_engine_run_style_update(
    host: &DocumentHost,
    read: &BegunRead,
    root: u32,
    computation_inputs: FfiDocumentStyleComputationInputs,
) {
    let Some(root) = StyleNodeID::from_raw(root) else {
        return;
    };
    let job = StyleJob {
        root,
        // SAFETY: Guaranteed by the caller.
        computation_inputs: Arc::new(unsafe { SealedStyleInputs::seal(computation_inputs) }),
        flies: false,
    };
    host.keep_style_inputs(root, Arc::clone(&job.computation_inputs));
    // A style update takes the transactions its installations feed back one after another, and ends only the last.
    // The host installed the one it took before this one, so it is done with the records the render side pinned for
    // it to compare with, which go as the next one is taken.
    let pinned = host.take_style_update_pinned_records();
    let answer = host.run(read, true, move |state| {
        let engine = state.engine_mut();
        for record in pinned {
            engine.unpin_layout_style_record(record);
        }
        let mut answer = job.run(engine);
        state.install_styles(&mut answer);
        answer
    });
    host.keep_style_update(answer);
}

/// Lets the pending style update under `root` fly beside the host, with the document computation inputs the host
/// gathered, where `blocker` is none and no transaction the host let fly before waits to be drained. The host's next
/// style update drains its reactions first. Answers whether the transaction flies.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the buffers `computation_inputs` names must be
/// live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_let_style_update_fly(
    host: &DocumentHost,
    root: u32,
    computation_inputs: FfiDocumentStyleComputationInputs,
    blocker: FfiFlightBlocker,
) -> bool {
    let (Some(root), Some(license)) = (StyleNodeID::from_raw(root), FlightLicense::for_blocker(blocker)) else {
        return false;
    };
    if host.has_flown_style() {
        return false;
    }
    let job = StyleJob {
        root,
        // SAFETY: Guaranteed by the caller.
        computation_inputs: Arc::new(unsafe { SealedStyleInputs::seal(computation_inputs) }),
        flies: true,
    };
    host.keep_style_inputs(root, Arc::clone(&job.computation_inputs));
    fly(host, FrameJob::Style(job), &license);
    true
}

/// Takes in the style update of `host`'s document that the host let fly, waiting for it to land, for the host to
/// drain its reactions against the inputs it was sealed with. What the host wrote beside the transaction reaches the
/// render state only once style_engine_end_flown_style_drain() ends the drain: it is the next transaction's.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_drain_flown_style_update(host: &DocumentHost, read: &BegunRead) {
    // The read takes the frame in where it still flies.
    let answer = host.begin_style_drain(read);
    host.keep_style_update(answer);
}

/// Ends the drain of the style update of `host`'s document that flew, behind which the writes the host made beside
/// it reach the render state.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_end_flown_style_drain(host: &DocumentHost) {
    host.end_style_drain();
}

/// Whether the render state installed the record `style_record` of the element `style_node` names in its box ahead of
/// the host, and marked the relayout the move asks for: the host's install of the row marks none then.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_render_state_installed(host: &DocumentHost, style_node: u32, style_record: u64) -> bool {
    StyleNodeID::from_raw(style_node).is_some_and(|style_node| host.render_state_installed(style_node, style_record))
}

/// Whether the style update of `host`'s document the host let fly still flies; one that has landed is taken in.
/// The event loop asks between two tasks, so this never waits.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_style_update_flies(host: &DocumentHost) -> bool {
    let boundary = TaskBoundary::at_event_loop_entry(&TAKES_FINISHED_STYLE_IN);
    host.frame_still_flies(&boundary)
}

/// The entry the event loop calls between two tasks to take a document's style update in where it has landed.
pub(crate) struct TakesFinishedStyleIn {
    _private: (),
}

const TAKES_FINISHED_STYLE_IN: TakesFinishedStyleIn = TakesFinishedStyleIn { _private: () };

/// Ends the transaction the host took last. The engine releases the identities its end released once it applies the
/// write, which the host takes back to mint again as it has that job back.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_end_style_update(host: &DocumentHost) {
    let pinned = host.end_style_update();
    host.queue_change(crate::render_state::ArenaChange::Engine(
        super::engine_calls::EngineWrite::EndStyleUpdate { pinned: pinned.into() },
    ));
}
