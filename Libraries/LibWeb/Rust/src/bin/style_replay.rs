/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::alloc::GlobalAlloc;
use std::alloc::Layout;
use std::alloc::System;
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::collections::HashSet;
use std::collections::VecDeque;
use std::collections::hash_map::DefaultHasher;
use std::env;
use std::ffi::c_void;
use std::hash::Hash;
use std::hash::Hasher;
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Duration;
use std::time::Instant;

use libweb_rust::css::style::bridge;
use libweb_rust::css::style::bridge::FfiCascadeOrigin;
use libweb_rust::css::style::bridge::FfiElementArrival;
use libweb_rust::css::style::bridge::FfiElementDeclarationDelta;
use libweb_rust::css::style::bridge::FfiElementDeclarationKind;
use libweb_rust::css::style::bridge::FfiElementStyleInput;
use libweb_rust::css::style::bridge::FfiExactCascadePublication;
use libweb_rust::css::style::bridge::FfiLocalFeatureDelta;
use libweb_rust::css::style::bridge::FfiMemoryPressureSnapshot;
use libweb_rust::css::style::bridge::FfiRuleMatch;
use libweb_rust::css::style::bridge::FfiStateDelta;
use libweb_rust::css::style::bridge::FfiStyleDelta;
use libweb_rust::css::style::bridge::FfiStyleDeltaDamage;
use libweb_rust::css::style::bridge::FfiStyleDeltaGap;
use libweb_rust::css::style::bridge::FfiStyleInputTransaction;
use libweb_rust::css::style::bridge::FfiTreeDelta;
use libweb_rust::css::style::bridge::RecordedExactCascadeWinner;
use libweb_rust::css::style::cascade::CascadeOperator;
use libweb_rust::css::style::cascade::SpecifiedValueID;
use libweb_rust::css::style::fast_hash::FastMap;
use libweb_rust::css::style::index::StyleAtomID;
use libweb_rust::css::style::memory::MEMORY_CATEGORIES;
#[cfg(test)]
use libweb_rust::css::style::memory::MemoryCategory;
use libweb_rust::css::style::memory::TIER_COUNT;
use libweb_rust::css::style::memory::TIER3_REFUSAL_CATEGORIES;
use libweb_rust::css::style::memory::Tier;
use libweb_rust::css::style::program::CustomDeclaration;
use libweb_rust::css::style::program::DeclaredProperty;
use libweb_rust::css::style::record_replay::EventKind;
use libweb_rust::css::style::record_replay::LogReader;
use libweb_rust::css::style::record_replay::PayloadReader;
use libweb_rust::css::style::record_replay::PayloadWriter;
use libweb_rust::css::style::selector::SelectorProgram;

struct ReplayCustomPropertyRegistry(*mut c_void);

impl ReplayCustomPropertyRegistry {
    fn new() -> Self {
        Self(libweb_rust::css::custom_properties::rust_custom_property_registry_create())
    }

    fn pointer(&self) -> *const c_void {
        self.0
    }
}

impl Drop for ReplayCustomPropertyRegistry {
    fn drop(&mut self) {
        unsafe { libweb_rust::css::custom_properties::rust_custom_property_registry_destroy(self.0) };
    }
}

include!(concat!(env!("OUT_DIR"), "/style_engine_replay_generated.rs"));

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("style-replay: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let options = Options::parse()?;

    // SAFETY: replay is single-threaded, and no StyleEngine has observed the environment yet.
    unsafe { env::remove_var("LIBWEB_STYLE_RECORD") };
    libweb_rust::css::computed_values::enable_style_group_replay();

    let mut encountered_subtests = BTreeSet::new();
    let mut reports = Vec::new();
    for path in &options.paths {
        let mapping = mapped_log::MappedLog::open(path)?;
        let mut reader = LogReader::new(mapping.bytes())?;
        let format_version = reader.version();
        let replay_custom_property_registry = ReplayCustomPropertyRegistry::new();
        reader.set_verify_checksums(options.assert_digests);
        // Engine IDs and style-record tokens are sequential counters in the recorder, so lookups
        // that run once per event index dense arrays instead of hashing. Replay is short-lived;
        // the arrays are sized by the largest token seen and never shrink.
        let mut live_engines = Vec::<Option<*mut c_void>>::new();
        let mut match_answer_identity_mappings = Vec::<MatchAnswerIdentityMapping>::new();
        let mut engine_count = 0_u64;
        let mut event_count = 0_u64;
        let mut intern_atom_boundary_calls = 0_u64;
        let mut element_arrivals = 0_u64;
        let mut element_fact_calls = 0_u64;
        let mut element_declaration_calls = 0_u64;
        let mut element_animation_name_calls = 0_u64;
        let mut attribute_value_text_queries = 0_u64;
        let mut attribute_value_text_publications = 0_u64;
        let mut selector_program_sharing = SelectorProgramSharing::default();
        let mut flush_count = 0_u64;
        let mut presence_degraded_publication_comparisons = 0_u64;
        let mut boundary_time = Duration::ZERO;
        let mut selected_flush_count = 0_u64;
        let mut selected_boundary_time = Duration::ZERO;
        let mut active_phase = None;
        let mut last_benchmark_marker = String::new();
        let mut phase_times = BTreeMap::<String, Duration>::new();
        let mut pending_changed_rows = FastMap::<usize, u64>::default();
        let mut amplification = AmplificationLedger::default();
        let mut amplification_counter_reader = None;
        let mut detailed_counters = DetailedCounterLedger::default();
        let mut detailed_counter_reader = None;
        let mut recorded_style_record_payloads = Vec::<Vec<Option<Option<EncodedU64Slice<'_>>>>>::new();
        let mut recorded_style_record_views = Vec::<Vec<Option<Option<RecordedStyleRecordView<'_>>>>>::new();
        let mut computed_longhand_tables =
            Vec::<Vec<Option<*mut libweb_rust::css::computed_longhand_table::ComputedLonghandTable>>>::new();
        let mut computed_group_payloads = Vec::new();
        let mut animation_overlay_payloads = Vec::new();
        let mut inheritance_dependent_properties = Vec::new();
        let mut inheritance_dependent_values = Vec::new();
        let mut memory_pressure = FfiMemoryPressureSnapshot::default();
        loop {
            let (skipped, event) = match options.assert_digests {
                true => (0, reader.read_event_borrowed()?),
                false => reader.read_event_borrowed_skipping_redundant_style_reads()?,
            };
            event_count += skipped;
            let Some(mut event) = event else {
                break;
            };
            event_count += 1;
            match event.kind {
                EventKind::SetElementNamespace
                | EventKind::SetElementLanguage
                | EventKind::SetElementDirectionality
                | EventKind::SetElementCustomStates
                | EventKind::SetElementIsSlot
                | EventKind::SetElementHeadingLevel => element_fact_calls += 1,
                EventKind::SetElementDeclaredProperties => element_declaration_calls += 1,
                EventKind::SetElementAnimationNames => element_animation_name_calls += 1,
                EventKind::HasAttributeValueText => attribute_value_text_queries += 1,
                EventKind::SetAttributeValueText => attribute_value_text_publications += 1,
                _ => {}
            }
            match event.kind {
                kind if replay_generated_boundary_event(kind, &mut event.payload, &live_engines)? => {}
                EventKind::CreateGraph => {
                    let engine_id = event.payload.read_u64()?;
                    let device_class = event.payload.read_u8()?;
                    if device_class != 0 {
                        return Err(format!("engine {engine_id} has unknown device class {device_class}").into());
                    }
                    let recorded_verification_gates = match event.payload.remaining_bytes() {
                        0 => 0,
                        _ => event.payload.read_u8()?,
                    };
                    require_matching_verification_gates(
                        recorded_verification_gates,
                        bridge::style_engine_verification_gate_bits(),
                    )
                    .map_err(|error| format!("engine {engine_id} {error}"))?;
                    let index = usize::try_from(engine_id)?;
                    if engine_id == 0 || live_engines.get(index).is_some_and(Option::is_some) {
                        return Err(format!("engine {engine_id} was created more than once").into());
                    }
                    let engine = bridge::style_engine_create_for_replay(bridge::FfiDeviceClass::ForegroundDesktop);
                    unsafe { bridge::style_engine_use_recording_memory_policy(engine) };
                    if live_engines.len() <= index {
                        live_engines.resize(index + 1, None);
                        selector_program_sharing.resize_engines(index + 1);
                        match_answer_identity_mappings.resize_with(index + 1, MatchAnswerIdentityMapping::default);
                        computed_longhand_tables.resize_with(index + 1, Vec::new);
                    }
                    live_engines[index] = Some(engine);
                    match_answer_identity_mappings[index] = MatchAnswerIdentityMapping::default();
                    computed_longhand_tables[index].clear();
                    engine_count += 1;
                }
                EventKind::SetComputedGroupDependencyMasks => {
                    let _engine = read_engine(&mut event.payload, &live_engines)?;
                    if event.payload.read_bool()? {
                        let first_property = event.payload.read_u16()?;
                        let masks = event.payload.read_u32_vec()?;
                        let output_masks = event.payload.read_u32_vec()?;
                        libweb_rust::css::computed_values::register_replay_property_dependency_masks(
                            first_property,
                            &masks,
                            &output_masks,
                        );
                    }
                }
                EventKind::DestroyGraph => {
                    let engine_id = event.payload.read_u64()?;
                    let engine_index = usize::try_from(engine_id)?;
                    let engine = live_engines
                        .get_mut(engine_index)
                        .and_then(Option::take)
                        .ok_or_else(|| format!("engine {engine_id} was destroyed without being live"))?;
                    release_computed_longhand_tables(&mut computed_longhand_tables[engine_index]);
                    accumulate_memory_pressure(&mut memory_pressure, unsafe {
                        bridge::replay_memory_pressure_snapshot(engine)
                    });
                    unsafe { bridge::style_engine_destroy(engine) };
                }
                EventKind::AllocateStyleNodes => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let expected = event.payload.read_u32_vec()?;
                    let mut actual = vec![0; expected.len()];
                    unsafe { bridge::style_engine_allocate_style_nodes(engine, actual.as_mut_ptr(), actual.len()) };
                    if actual != expected {
                        return Err(
                            format!("style-node allocation diverged: expected {expected:?}, got {actual:?}").into(),
                        );
                    }
                }
                EventKind::ApplyTransaction => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    // The row arrays are consumed in place from the mapped log; nothing is copied.
                    let tree = event.payload.read_raw_slice::<FfiTreeDelta>()?;
                    element_arrivals += tree
                        .iter()
                        .filter(|delta| !delta.old_connected && delta.new_connected)
                        .count() as u64;
                    let arrivals = event.payload.read_raw_slice::<FfiElementArrival>()?;
                    let arrival_custom_state_atoms = event.payload.read_u32_vec()?;
                    let features = event.payload.read_raw_slice::<FfiLocalFeatureDelta>()?;
                    let states = event.payload.read_raw_slice::<FfiStateDelta>()?;
                    let declarations = event.payload.read_raw_slice::<FfiElementDeclarationDelta>()?;
                    let element_style_inputs = event.payload.read_raw_slice::<FfiElementStyleInput>()?;
                    let row_count =
                        (tree.len() + features.len() + states.len() + declarations.len() + element_style_inputs.len())
                            as u64;
                    *pending_changed_rows.entry(engine as usize).or_default() += row_count;
                    let transaction = FfiStyleInputTransaction {
                        tree_deltas: tree.as_ptr(),
                        tree_delta_count: tree.len(),
                        element_arrivals: arrivals.as_ptr(),
                        element_arrival_count: arrivals.len(),
                        arrival_custom_state_atoms: arrival_custom_state_atoms.as_ptr(),
                        arrival_custom_state_atom_count: arrival_custom_state_atoms.len(),
                        local_feature_deltas: features.as_ptr(),
                        local_feature_delta_count: features.len(),
                        state_deltas: states.as_ptr(),
                        state_delta_count: states.len(),
                        element_declaration_deltas: declarations.as_ptr(),
                        element_declaration_delta_count: declarations.len(),
                        element_style_inputs: element_style_inputs.as_ptr(),
                        element_style_input_count: element_style_inputs.len(),
                    };
                    unsafe { bridge::style_engine_apply_transaction(engine, &transaction) };
                }
                EventKind::Flush => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let counter_reader =
                        amplification_counter_reader.get_or_insert_with(|| AmplificationCounterReader::new(engine));
                    let before = counter_reader.read(engine);
                    let detailed_before = options.detailed_counters.then(|| {
                        detailed_counter_reader
                            .get_or_insert_with(|| DetailedCounterReader::new(engine))
                            .read(engine)
                    });
                    let start = Instant::now();
                    unsafe { bridge::style_engine_flush(engine) };
                    let elapsed = start.elapsed();
                    let after = counter_reader.read(engine);
                    let detailed_after = detailed_before
                        .as_ref()
                        .map(|_| detailed_counter_reader.as_ref().unwrap().read(engine));
                    boundary_time += elapsed;
                    flush_count += 1;
                    let selected = record_timing(
                        &options.selection,
                        active_phase.as_ref(),
                        elapsed,
                        &mut selected_flush_count,
                        &mut selected_boundary_time,
                        &mut phase_times,
                    );
                    let changed_rows = pending_changed_rows.remove(&(engine as usize)).unwrap_or(0);
                    if selected {
                        amplification.record(changed_rows, &before, &after);
                        if let (Some(before), Some(after)) = (&detailed_before, &detailed_after) {
                            detailed_counters.record(before, after);
                        }
                    }
                }
                EventKind::AddStyleRule => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let sheet = event.payload.read_u32()?;
                    let before_rule = event.payload.read_u32()?;
                    let expected = event.payload.read_u32()?;
                    let actual = match expected {
                        0 => 0,
                        _ => {
                            replay_atom_mappings(engine, &mut event.payload)?;
                            let program = libweb_rust::css::style::selector::replay::read(&mut event.payload)?;
                            selector_program_sharing.record(engine_index, &program);
                            unsafe { bridge::replay_add_style_rule(engine, sheet, before_rule, program) }
                        }
                    };
                    if actual != expected {
                        return Err(format!("rule allocation diverged: expected {expected}, got {actual}").into());
                    }
                }
                EventKind::InternAtom => {
                    intern_atom_boundary_calls += 1;
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let token = usize::try_from(event.payload.read_u64()?)?;
                    let expected = event.payload.read_u32()?;
                    let actual = unsafe { bridge::style_engine_intern_atom(engine, token) };
                    assert_identity("atom", expected, actual)?;
                }
                EventKind::SelectorQueryAtomMappings => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    replay_atom_mappings(engine, &mut event.payload)?;
                }
                EventKind::ReplaceStyleRuleSelectors => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let rule = event.payload.read_u32()?;
                    replay_atom_mappings(engine, &mut event.payload)?;
                    let program = libweb_rust::css::style::selector::replay::read(&mut event.payload)?;
                    selector_program_sharing.record(engine_index, &program);
                    unsafe { bridge::replay_replace_style_rule_selectors(engine, rule, program) };
                }
                EventKind::SetElementDeclaredProperties => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let kind = read_declaration_kind(&mut event.payload)?;
                    let declarations_are_complete = event.payload.read_bool()?;
                    let declared = read_declared_properties(&mut event.payload)?;
                    let custom_declarations = read_custom_declarations(&mut event.payload)?;
                    unsafe {
                        bridge::replay_set_element_declared_properties(
                            engine,
                            node,
                            kind,
                            &declared,
                            &custom_declarations,
                            declarations_are_complete,
                        );
                    };
                }
                EventKind::RepublishRecordEnvironment => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let environment = event.payload.read_u64()?;
                    let expected = event.payload.read_u64()?;
                    let actual = unsafe { bridge::replay_republish_record_environment(engine, node, environment) };
                    if actual != expected {
                        return Err(format!("republished record environment diverged: {actual} != {expected}").into());
                    }
                }
                EventKind::NoteCustomPropertyName => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let name = event.payload.read_u32()?;
                    let text = event.payload.read_u16_vec()?;
                    unsafe { bridge::replay_note_custom_property_name(engine, name, &text) };
                }
                EventKind::SetRuleDeclaredProperties => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let rule = event.payload.read_u32()?;
                    let declarations_are_complete = event.payload.read_bool()?;
                    let declared = read_declared_properties(&mut event.payload)?;
                    let custom_declarations = read_custom_declarations(&mut event.payload)?;
                    unsafe {
                        bridge::replay_set_rule_declared_properties(
                            engine,
                            rule,
                            &declared,
                            &custom_declarations,
                            declarations_are_complete,
                        );
                    };
                }
                EventKind::TakeStyleTransaction => {
                    return Err("recording predates typed style delta batches".into());
                }
                EventKind::StyleDeltaBatch => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let root = event.payload.read_u32()?;
                    let computation_inputs = read_document_style_computation_inputs(
                        &mut event.payload,
                        format_version,
                        replay_custom_property_registry.pointer(),
                    )?;
                    let output_bytes = event.payload.read_bytes()?;
                    let expected_digest = event.payload.read_u64()?;
                    let expected = read_style_transaction_outputs(PayloadReader::new(output_bytes))?;
                    if !expected.filter_calls.is_empty() {
                        return Err("style transaction recording predates engine-owned liveness filtering".into());
                    }
                    let mut context = ReplayStyleTransactionContext {
                        expected_emissions: expected.emissions.into(),
                        actual_emissions: Vec::new(),
                        match_answer_identity_mapping: &mut match_answer_identity_mappings[engine_index],
                        error: None,
                    };
                    let counter_reader =
                        amplification_counter_reader.get_or_insert_with(|| AmplificationCounterReader::new(engine));
                    let before = counter_reader.read(engine);
                    let detailed_before = options.detailed_counters.then(|| {
                        detailed_counter_reader
                            .get_or_insert_with(|| DetailedCounterReader::new(engine))
                            .read(engine)
                    });
                    if expected.style_atoms_swept {
                        unsafe {
                            bridge::style_engine_set_replay_reclaimed_style_atoms(
                                engine,
                                expected.reclaimed_atoms.as_ptr(),
                                expected.reclaimed_atoms.len(),
                            );
                        }
                    }
                    let start = Instant::now();
                    let actual_view =
                        unsafe { bridge::style_engine_take_style_transaction(engine, root, computation_inputs) };
                    let actual_reclaimed_atoms = if actual_view.reclaimed_style_atom_count == 0 {
                        Vec::new()
                    } else {
                        unsafe {
                            std::slice::from_raw_parts(
                                actual_view.reclaimed_style_atoms,
                                actual_view.reclaimed_style_atom_count,
                            )
                        }
                        .iter()
                        .map(|reclaimed| reclaimed.atom)
                        .collect::<Vec<_>>()
                    };
                    if actual_view.style_atoms_swept != expected.style_atoms_swept {
                        return Err(format!(
                            "style atom sweep diverged: expected {}, got {}",
                            expected.style_atoms_swept, actual_view.style_atoms_swept
                        )
                        .into());
                    }
                    if actual_reclaimed_atoms != expected.reclaimed_atoms {
                        return Err(format!(
                            "style atom reclamation diverged: expected {:?}, got {:?}",
                            expected.reclaimed_atoms, actual_reclaimed_atoms
                        )
                        .into());
                    }
                    if actual_view.count != 0 {
                        let answers = unsafe { std::slice::from_raw_parts(actual_view.answers, actual_view.count) };
                        replay_style_transaction_output(
                            &mut context,
                            actual_view.transaction_version,
                            actual_view.program_version,
                            answers,
                        );
                    }
                    let elapsed = start.elapsed();
                    let after = counter_reader.read(engine);
                    let detailed_after = detailed_before
                        .as_ref()
                        .map(|_| detailed_counter_reader.as_ref().unwrap().read(engine));
                    boundary_time += elapsed;
                    flush_count += 1;
                    let selected = record_timing(
                        &options.selection,
                        active_phase.as_ref(),
                        elapsed,
                        &mut selected_flush_count,
                        &mut selected_boundary_time,
                        &mut phase_times,
                    );
                    let changed_rows = pending_changed_rows.remove(&(engine as usize)).unwrap_or(0);
                    if selected {
                        amplification.record(changed_rows, &before, &after);
                        if let (Some(before), Some(after)) = (&detailed_before, &detailed_after) {
                            detailed_counters.record(before, after);
                        }
                    }
                    if let Some(error) = context.error {
                        return Err(error.into());
                    }
                    if !context.expected_emissions.is_empty() {
                        return Err("style transaction omitted recorded output".into());
                    }
                    if options.assert_digests {
                        let actual = StyleTransactionOutputs {
                            result: actual_view.scoped,
                            filter_calls: Vec::new(),
                            emissions: context.actual_emissions,
                            style_atoms_swept: actual_view.style_atoms_swept,
                            reclaimed_atoms: actual_reclaimed_atoms,
                        };
                        let mut actual_payload = PayloadWriter::default();
                        write_style_transaction_outputs(&actual, &mut actual_payload);
                        if actual_payload.stable_digest() != expected_digest {
                            return Err(format!(
                                "style transaction digest diverged: expected {expected_digest:#018x}, got {:#018x}",
                                actual_payload.stable_digest()
                            )
                            .into());
                        }
                    }
                }
                EventKind::SetElementParts => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let count = event.payload.read_length()?;
                    let mut names = Vec::with_capacity(count);
                    let mut hosts = Vec::with_capacity(count);
                    for _ in 0..count {
                        names.push(event.payload.read_u32()?);
                        hosts.push(event.payload.read_u32()?);
                    }
                    unsafe {
                        bridge::style_engine_set_element_parts(engine, node, names.as_ptr(), hosts.as_ptr(), count)
                    };
                }
                EventKind::SetElementLanguage => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let language = event.payload.read_u32()?;
                    let text = event.payload.read_u16_vec()?;
                    let text_pointer = match text.is_empty() {
                        true => std::ptr::null(),
                        false => text.as_ptr(),
                    };
                    unsafe {
                        bridge::style_engine_set_element_language(engine, node, language, text_pointer, text.len())
                    };
                }
                EventKind::MatchDocument => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let root = event.payload.read_u32()?;
                    let expected = usize::try_from(event.payload.read_u64()?)?;
                    let actual = unsafe { bridge::style_engine_match_document(engine, root) };
                    if actual != expected {
                        return Err(format!("document match count diverged: expected {expected}, got {actual}").into());
                    }
                }
                EventKind::MatchElement => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let capacity = usize::try_from(event.payload.read_u64()?)?;
                    let compact_for_cascade = event.payload.read_bool()?;
                    let expected_result = usize::try_from(event.payload.read_u64()?)?;
                    let has_output = event.payload.read_bool()?;
                    let expected = match has_output {
                        true => read_rule_matches(&mut event.payload)?,
                        false => Vec::new(),
                    };
                    let mut actual = vec![
                        FfiRuleMatch {
                            node: 0,
                            rule: 0,
                            semantic_declaration: 0,
                            pseudo_element: 0,
                            scope_host: 0,
                            scope_proximity: 0,
                        };
                        capacity
                    ];
                    let actual_result = unsafe {
                        bridge::style_engine_match_element(
                            engine,
                            node,
                            actual.as_mut_ptr(),
                            capacity,
                            compact_for_cascade,
                        )
                    };
                    if actual_result != expected_result {
                        return Err(format!(
                            "element match count diverged: expected {expected_result}, got {actual_result}"
                        )
                        .into());
                    }
                    if has_output && !rule_matches_equal(&actual[..actual_result], &expected) {
                        return Err(format!(
                            "element matches diverged for node {node}: expected {expected:?}, got {:?}",
                            &actual[..actual_result]
                        )
                        .into());
                    }
                }
                EventKind::MatchElementSignature => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let expected = event.payload.read_u32()?;
                    let actual = unsafe { bridge::style_engine_match_element_signature(engine, node) };
                    match_answer_identity_mappings[engine_index]
                        .record(expected, actual)
                        .map_err(|error| format!("element match signature {error}"))?;
                }
                EventKind::PublishedMatchAnswerSignature => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let expected = event.payload.read_u32()?;
                    let actual = unsafe { bridge::style_engine_published_match_answer_signature(engine, node) };
                    match_answer_identity_mappings[engine_index]
                        .record(expected, actual)
                        .map_err(|error| format!("published match answer signature {error}"))?;
                }
                EventKind::BenchmarkMarker => {
                    let _engine = read_engine(&mut event.payload, &live_engines)?;
                    let name = String::from_utf16(&event.payload.read_u16_vec()?)?;
                    last_benchmark_marker.clone_from(&name);
                    if let Some(marker) = BenchmarkMarker::parse(&name) {
                        encountered_subtests.insert(format!("{}/{}", marker.suite, marker.test));
                        active_phase = marker.next_phase();
                    }
                }
                EventKind::ConsumePublishedMatchAnswer => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let capacity = usize::try_from(event.payload.read_u64()?)?;
                    let expected_result = usize::try_from(event.payload.read_u64()?)?;
                    let has_output = event.payload.read_bool()?;
                    let expected = match has_output {
                        true => read_rule_matches(&mut event.payload)?,
                        false => Vec::new(),
                    };
                    let mut actual = vec![
                        FfiRuleMatch {
                            node: 0,
                            rule: 0,
                            semantic_declaration: 0,
                            pseudo_element: 0,
                            scope_host: 0,
                            scope_proximity: 0,
                        };
                        capacity
                    ];
                    let actual_result = unsafe {
                        bridge::style_engine_consume_published_match_answer(engine, node, actual.as_mut_ptr(), capacity)
                    };
                    if actual_result != expected_result {
                        return Err(format!(
                        "published match consumption diverged for node {node}: expected count {expected_result}, got {actual_result}"
                    )
                    .into());
                    }
                    if has_output && !rule_matches_equal(&actual[..actual_result], &expected) {
                        return Err(format!(
                            "published matches diverged for node {node}: expected {expected:?}, got {:?}",
                            &actual[..actual_result]
                        )
                        .into());
                    }
                }
                EventKind::CompletePublishedMatchAnswersForClosure => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let nodes = event.payload.read_u32_vec()?;
                    let expected = event.payload.read_bool()?;
                    let actual = unsafe {
                        bridge::style_engine_complete_published_match_answers_for_closure(
                            engine,
                            nodes.as_ptr(),
                            nodes.len(),
                        )
                    };
                    if actual != expected {
                        return Err(format!(
                            "published match closure completion diverged: expected {expected}, got {actual}"
                        )
                        .into());
                    }
                }
                EventKind::ForEachFlatTreeDescendant => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let root = event.payload.read_u32()?;
                    let expected = event.payload.read_u32_vec()?;
                    let view = unsafe { bridge::style_engine_flat_tree_descendants(engine, root) };
                    let actual = if view.count == 0 {
                        Vec::new()
                    } else {
                        unsafe { std::slice::from_raw_parts(view.nodes, view.count) }.to_vec()
                    };
                    unsafe { bridge::style_engine_discard_flat_tree_descendants(engine) };
                    if actual != expected {
                        return Err(format!(
                            "flat-tree descendants diverged for node {root}: expected {expected:?}, got {actual:?}"
                        )
                        .into());
                    }
                }
                EventKind::BeginColdMatchingBatch => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let root = event.payload.read_u32()?;
                    let expected = event.payload.read_bool()?;
                    let start = Instant::now();
                    let actual = unsafe { bridge::style_engine_begin_cold_matching_batch(engine, root) };
                    let elapsed = start.elapsed();
                    boundary_time += elapsed;
                    record_boundary_timing(
                        &options.selection,
                        active_phase.as_ref(),
                        elapsed,
                        &mut selected_boundary_time,
                        &mut phase_times,
                    );
                    if actual != expected {
                        return Err(
                            format!("cold matching batch result diverged: expected {expected}, got {actual}").into(),
                        );
                    }
                }
                EventKind::BeginAdaptiveColdMatchingBatch => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let root = event.payload.read_u32()?;
                    let start = Instant::now();
                    unsafe { bridge::style_engine_begin_adaptive_cold_matching_batch(engine, root) };
                    let elapsed = start.elapsed();
                    boundary_time += elapsed;
                    record_boundary_timing(
                        &options.selection,
                        active_phase.as_ref(),
                        elapsed,
                        &mut selected_boundary_time,
                        &mut phase_times,
                    );
                }
                EventKind::EndColdMatchingBatch => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let start = Instant::now();
                    unsafe { bridge::style_engine_end_cold_matching_batch(engine) };
                    let elapsed = start.elapsed();
                    boundary_time += elapsed;
                    record_boundary_timing(
                        &options.selection,
                        active_phase.as_ref(),
                        elapsed,
                        &mut selected_boundary_time,
                        &mut phase_times,
                    );
                }
                EventKind::RetryEngineRecordAfterAncestor => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let expected = event.payload.read_u64()?;
                    let actual = unsafe { bridge::style_engine_retry_engine_record_after_ancestor(engine, node) };
                    if actual != expected {
                        return Err(format!(
                            "retried cold style record diverged for node {node}: expected {expected}, got {actual}"
                        )
                        .into());
                    }
                }
                EventKind::RemoveComputedPseudo => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let pseudo_kind = event.payload.read_u8()?;
                    let expected_old = event.payload.read_u64()?;
                    let expected_new = event.payload.read_u64()?;
                    let actual = unsafe { bridge::style_engine_remove_computed_pseudo(engine, node, pseudo_kind) };
                    if (actual.old_style_record, actual.new_style_record) != (expected_old, expected_new) {
                        return Err(format!(
                            "removed pseudo style records diverged: expected ({expected_old}, {expected_new}), got ({}, {})",
                            actual.old_style_record, actual.new_style_record
                        )
                        .into());
                    }
                }
                EventKind::Counter => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let index = usize::try_from(event.payload.read_u64()?)?;
                    let expected_present = event.payload.read_bool()?;
                    let expected = match expected_present {
                        true => Some((event.payload.read_bytes()?.to_vec(), event.payload.read_u64()?)),
                        false => None,
                    };
                    let mut actual_value = 0;
                    let mut actual_name_length = 0;
                    let actual_name = unsafe {
                        bridge::style_engine_counter(engine, index, &mut actual_value, &mut actual_name_length)
                    };
                    let actual = match actual_name.is_null() {
                        true => None,
                        false => Some((
                            unsafe { std::slice::from_raw_parts(actual_name, actual_name_length) }.to_vec(),
                            actual_value,
                        )),
                    };
                    if actual != expected {
                        return Err(format!("counter {index} diverged: expected {expected:?}, got {actual:?}").into());
                    }
                }
                EventKind::PublishExactCascadeState => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let pseudo_kind = event.payload.read_u8()?;
                    let inherited_style_groups = event.payload.read_u8()?;
                    let expected_generation = event.payload.read_u64()?;
                    let expected_bound_generation = match event.payload.read_bool()? {
                        true => Some(event.payload.read_u64()?),
                        false => None,
                    };
                    let actual_generations =
                        unsafe { bridge::replay_exact_cascade_generation_snapshot(engine, node, pseudo_kind) };
                    if actual_generations != (expected_generation, expected_bound_generation) {
                        print_memory_pressure("generation divergence", unsafe {
                            bridge::replay_memory_pressure_snapshot(engine)
                        });
                        return Err(format!(
                            "exact cascade generations diverged at event {event_count} for node {node}: expected ({expected_generation}, {expected_bound_generation:?}), got {actual_generations:?}"
                        )
                        .into());
                    }
                    let count = event.payload.read_length()?;
                    let mut winners = Vec::with_capacity(count);
                    for _ in 0..count {
                        winners.push(RecordedExactCascadeWinner {
                            property: event.payload.read_u16()?,
                            value: event.payload.read_u64()?,
                            operator: read_cascade_operator(&mut event.payload)?,
                            animation_relevance: event.payload.read_u32()?,
                            important: event.payload.read_bool()?,
                        });
                    }
                    let expected = read_exact_cascade_publication(&mut event.payload)?;
                    let recorded_had_previous = match event.payload.remaining_bytes() {
                        0 => None,
                        _ => Some(event.payload.read_bool()?),
                    };
                    let (donor_node, donor_style_record) = match event.payload.remaining_bytes() {
                        0 => (0, 0),
                        _ => (event.payload.read_u32()?, event.payload.read_u64()?),
                    };
                    let (actual, actual_had_previous) = unsafe {
                        bridge::replay_publish_exact_cascade_state(
                            engine,
                            node,
                            pseudo_kind,
                            &winners,
                            inherited_style_groups,
                            donor_node,
                            donor_style_record,
                        )
                    };
                    // Retention differs legitimately between the recording session and replay, and
                    // the publication's mask follows it. Every other field remains comparable.
                    let comparable = recorded_had_previous.is_none_or(|recorded| recorded == actual_had_previous);
                    if !comparable {
                        presence_degraded_publication_comparisons += 1;
                    }
                    if !exact_cascade_publications_match(expected, actual, comparable) {
                        print_memory_pressure("publication divergence", unsafe {
                            bridge::replay_memory_pressure_snapshot(engine)
                        });
                        return Err(format!(
                            "exact cascade publication diverged at event {event_count} for node {node}: expected {expected:?}, got {actual:?}"
                        )
                        .into());
                    }
                }
                EventKind::PublishComputedGroups => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let node = event.payload.read_u32()?;
                    let pseudo_kind = event.payload.read_u8()?;
                    let group_count = event.payload.read_length()?;
                    computed_group_payloads.clear();
                    computed_group_payloads.reserve(group_count);
                    for _ in 0..group_count {
                        let identity = event.payload.read_u32()?;
                        let retained_bytes = usize::try_from(event.payload.read_u64()?)?;
                        let pointer =
                            libweb_rust::css::computed_values::register_replay_style_group(identity, retained_bytes);
                        computed_group_payloads.push(pointer);
                    }
                    let inherited_group_count = event.payload.read_length()?;
                    let custom_property_environment = event.payload.read_u64()?;
                    let pseudo_element_styles = event.payload.read_u64()?;
                    let dependency_flags = event.payload.read_u8()?;
                    let counter_style_environment_identity = event.payload.read_u64()?;
                    let animation_overlay_identity = event.payload.read_u64()?;
                    let animated_overlay = match event.payload.read_u64()? {
                        0 => std::ptr::null_mut(),
                        _ => libweb_rust::css::animated_overlay::rust_animated_overlay_create(),
                    };
                    let overlay_count = event.payload.read_length()?;
                    animation_overlay_payloads.clear();
                    animation_overlay_payloads.reserve(overlay_count);
                    for _ in 0..overlay_count {
                        animation_overlay_payloads.push(replay_pointer(event.payload.read_u64()?));
                    }
                    let property_importance = event.payload.read_bytes()?;
                    let property_inheritance = event.payload.read_bytes()?;
                    let inheritance_dependent_count = event.payload.read_length()?;
                    inheritance_dependent_properties.clear();
                    inheritance_dependent_properties.reserve(inheritance_dependent_count);
                    inheritance_dependent_values.clear();
                    inheritance_dependent_values.reserve(inheritance_dependent_count);
                    for _ in 0..inheritance_dependent_count {
                        inheritance_dependent_properties.push(event.payload.read_u16()?);
                        let token = event.payload.read_u64()?;
                        let flags = event.payload.read_u8()?;
                        inheritance_dependent_values.push(bridge::replay_style_value(token, flags));
                    }
                    let raw_cascaded_font_size_token = event.payload.read_u64()?;
                    let raw_cascaded_font_size_flags = event.payload.read_u8()?;
                    let raw_cascaded_font_size = match raw_cascaded_font_size_token {
                        0 => std::ptr::null(),
                        token => bridge::replay_style_value(token, raw_cascaded_font_size_flags),
                    };
                    let longhand_table = match event.payload.read_bool()? {
                        false => std::ptr::null_mut(),
                        true => {
                            let identity = usize::try_from(event.payload.read_u32()?)?;
                            let record_definition = event.payload.read_bool()?;
                            let tables = &mut computed_longhand_tables[engine_index];
                            if record_definition {
                                if tables.len() <= identity {
                                    tables.resize(identity + 1, None);
                                }
                                if tables[identity].is_some() {
                                    return Err(format!("computed longhand table {identity} was defined twice").into());
                                }
                                let table =
                                    libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_create();
                                let stored_value_count = event.payload.read_length()?;
                                for _ in 0..stored_value_count {
                                    let property = event.payload.read_u16()?;
                                    let token = event.payload.read_u64()?;
                                    let flags = event.payload.read_u8()?;
                                    let value = bridge::replay_style_value(token, flags);
                                    unsafe {
                                        libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_set(
                                            table,
                                            property,
                                            value.cast(),
                                            -1,
                                        );
                                    }
                                }
                                unsafe {
                                    libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_freeze(
                                        table,
                                    );
                                }
                                tables[identity] = Some(table);
                            }
                            tables
                                .get(identity)
                                .copied()
                                .flatten()
                                .ok_or_else(|| format!("computed longhand table {identity} was not defined"))?
                        }
                    };
                    let expected = bridge::FfiStyleRecordDelta {
                        old_style_record: event.payload.read_u64()?,
                        new_style_record: event.payload.read_u64()?,
                    };
                    let publication_longhand_table = if longhand_table.is_null() {
                        if !inheritance_dependent_values.is_empty() {
                            return Err("inheritance-dependent values require a computed longhand table".into());
                        }
                        std::ptr::null_mut()
                    } else {
                        let table = libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_create();
                        unsafe {
                            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_copy_from(
                                table,
                                longhand_table,
                            );
                            for (&property, &value) in inheritance_dependent_properties
                                .iter()
                                .zip(&inheritance_dependent_values)
                            {
                                libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_add_inheritance_dependent_value(
                                    table,
                                    property,
                                    value,
                                );
                            }
                            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_load_flag_bitmaps(
                                table,
                                property_importance.as_ptr(),
                                property_importance.len(),
                                property_inheritance.as_ptr(),
                                property_inheritance.len(),
                            );
                            let metadata =
                                &mut *libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_metadata(
                                    table,
                                );
                            metadata.pseudo_element_styles = pseudo_element_styles;
                            metadata.dependency_flags = dependency_flags & 3;
                            metadata.in_display_none_subtree = dependency_flags & (1 << 2) != 0;
                            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_set_raw_cascaded_font_size(
                                table,
                                raw_cascaded_font_size,
                            );
                            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_freeze(table);
                        }
                        table
                    };
                    let actual = unsafe {
                        bridge::style_engine_publish_computed_groups(
                            engine,
                            node,
                            pseudo_kind,
                            computed_group_payloads.as_ptr(),
                            computed_group_payloads.len(),
                            inherited_group_count,
                            custom_property_environment,
                            dependency_flags & (1 << 3) != 0,
                            counter_style_environment_identity,
                            animation_overlay_identity,
                            animated_overlay.cast(),
                            animation_overlay_payloads.as_ptr(),
                            animation_overlay_payloads.len(),
                            publication_longhand_table.cast_const().cast(),
                            std::ptr::null(),
                        )
                    };
                    if !publication_longhand_table.is_null() {
                        unsafe {
                            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_release(
                                publication_longhand_table,
                            );
                        }
                    }
                    if !animated_overlay.is_null() {
                        unsafe {
                            libweb_rust::css::animated_overlay::rust_animated_overlay_free(animated_overlay);
                        }
                    }
                    if actual != expected {
                        return Err(format!(
                            "computed style publication diverged at event {event_count} after {last_benchmark_marker:?} in {active_phase:?} for node {node}: expected {expected:?}, got {actual:?}"
                        )
                        .into());
                    }
                }
                EventKind::StyleRecordPayloads => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let style_record = event.payload.read_u64()?;
                    let record_response = event.payload.read_bool()?;
                    let index = style_record_replay_index(style_record)?;
                    if recorded_style_record_payloads.len() <= engine_index {
                        recorded_style_record_payloads.resize_with(engine_index + 1, Vec::new);
                    }
                    let records = &mut recorded_style_record_payloads[engine_index];
                    if record_response {
                        let response = match event.payload.read_bool()? {
                            true => Some(read_u64_slice(&mut event.payload)?),
                            false => None,
                        };
                        if records.len() <= index {
                            records.resize(index + 1, None);
                        }
                        records[index] = Some(response);
                    } else if !options.assert_digests {
                        // The recorder omits repeat responses because style records are immutable.
                        // Current captures omit this event entirely. Old captures keep the marker;
                        // fast mode trusts the first comparison, while assert mode checks repeats.
                        continue;
                    }
                    let expected = records
                        .get(index)
                        .and_then(Option::as_ref)
                        .ok_or_else(|| format!("style record {style_record} payload response was not defined"))?;
                    let actual = unsafe { bridge::style_engine_style_record_payloads(engine, style_record) };
                    if actual.is_null() == expected.is_some() {
                        return Err(format!("style record {style_record} payload presence diverged").into());
                    }
                    if let Some(expected) = expected {
                        let pointers =
                            unsafe { std::slice::from_raw_parts(actual.cast::<*const c_void>(), expected.len()) };
                        for (position, (&pointer, want)) in pointers.iter().zip(expected.iter()).enumerate() {
                            if pointer_value(pointer)? != want {
                                return Err(format!(
                                    "style record {style_record} payload {position} diverged from {want}"
                                )
                                .into());
                            }
                        }
                    }
                }
                EventKind::StyleRecordView => {
                    let (engine_index, engine) = read_engine_indexed(&mut event.payload, &live_engines)?;
                    let style_record = event.payload.read_u64()?;
                    let record_response = event.payload.read_bool()?;
                    let index = style_record_replay_index(style_record)?;
                    if recorded_style_record_views.len() <= engine_index {
                        recorded_style_record_views.resize_with(engine_index + 1, Vec::new);
                    }
                    let records = &mut recorded_style_record_views[engine_index];
                    if record_response {
                        let response = match event.payload.read_bool()? {
                            true => Some(read_style_record_view(&mut event.payload)?),
                            false => None,
                        };
                        if records.len() <= index {
                            records.resize(index + 1, None);
                        }
                        records[index] = Some(response);
                    } else if !options.assert_digests {
                        // The recorder omits repeat responses because style records are immutable.
                        // Current captures omit this event entirely. Old captures keep the marker;
                        // fast mode trusts the first comparison, while assert mode checks repeats.
                        continue;
                    }
                    let expected = records
                        .get(index)
                        .and_then(Option::as_ref)
                        .ok_or_else(|| format!("style record {style_record} view response was not defined"))?;
                    let actual = unsafe { bridge::style_engine_style_record_view(engine, style_record) };
                    if actual.present != expected.is_some() {
                        return Err(format!("style record {style_record} view presence diverged").into());
                    }
                    if let Some(expected) = expected {
                        if !style_record_view_matches(expected, actual)? {
                            let actual = semantic_style_record_view(actual)?;
                            return Err(format!(
                                "style record {style_record} view diverged: expected {expected:?}, got {actual:?}"
                            )
                            .into());
                        }
                    }
                }
                EventKind::AttributeValueTextRequirementsVersion => {
                    let _engine = read_engine(&mut event.payload, &live_engines)?;
                    let _recorded_version = event.payload.read_u64()?;
                }
                EventKind::AttributeNameRequiresValueText => {
                    let _engine = read_engine(&mut event.payload, &live_engines)?;
                    let _name = event.payload.read_u32()?;
                    let _recorded_result = event.payload.read_bool()?;
                }
                EventKind::HasDeferredGeometryTransaction => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let expected = event.payload.read_bool()?;
                    let actual = unsafe { bridge::style_engine_has_deferred_geometry_transaction(engine) };
                    if actual != expected {
                        return Err(format!(
                            "deferred geometry transaction presence diverged: expected {expected}, got {actual}"
                        )
                        .into());
                    }
                }
                EventKind::PendingTransactionMayAffectLayoutGeometry => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let expected = event.payload.read_bool()?;
                    let actual = unsafe { bridge::style_engine_pending_transaction_may_affect_layout_geometry(engine) };
                    if actual != expected {
                        return Err(
                            format!("pending geometry effect diverged: expected {expected}, got {actual}").into(),
                        );
                    }
                }
                EventKind::DeferPendingTransactionForGeometryRead => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let expected = event.payload.read_bool()?;
                    let actual = unsafe { bridge::style_engine_defer_pending_transaction_for_geometry_read(engine) };
                    if actual != expected {
                        return Err(format!(
                            "geometry transaction deferral diverged: expected {expected}, got {actual}"
                        )
                        .into());
                    }
                }
                EventKind::BeginDeferredGeometryTransactionFlush => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    let expected = event.payload.read_bool()?;
                    let actual = unsafe { bridge::style_engine_begin_deferred_geometry_transaction_flush(engine) };
                    if actual != expected {
                        return Err(format!(
                            "deferred geometry flush start diverged: expected {expected}, got {actual}"
                        )
                        .into());
                    }
                }
                EventKind::EndDeferredGeometryTransactionFlush => {
                    let engine = read_engine(&mut event.payload, &live_engines)?;
                    unsafe { bridge::style_engine_end_deferred_geometry_transaction_flush(engine) };
                }
                kind => return Err(format!("unhandled StyleEngine replay event {kind:?}").into()),
            }
            event.payload.finish()?;
        }
        let live_at_process_exit = live_engines.iter().flatten().count();
        for tables in &mut computed_longhand_tables {
            release_computed_longhand_tables(tables);
        }
        for engine in live_engines.into_iter().flatten() {
            accumulate_memory_pressure(&mut memory_pressure, unsafe {
                bridge::replay_memory_pressure_snapshot(engine)
            });
            unsafe { bridge::style_engine_destroy(engine) };
        }
        println!(
            "replayed {} events and {} flushes for {} document engines in {:.3} ms ({} live at capture exit) from {}",
            event_count,
            flush_count,
            engine_count,
            boundary_time.as_secs_f64() * 1000.0,
            live_at_process_exit,
            path.to_string_lossy()
        );
        println!(
            "presence-degraded exact cascade publication comparisons: {presence_degraded_publication_comparisons}"
        );
        print_memory_residency(engine_count, &memory_pressure);
        if memory_pressure.tier3_refusals != 0
            || memory_pressure.tier4_refusals != 0
            || memory_pressure.tier3_evictions != 0
        {
            println!(
                "memory pressure: tier3 limit={} bytes, resident={} bytes; tier4 limit={} bytes, resident={} bytes; refusals={{tier3: {}, tier4: {}}}; tier3 evictions={}",
                memory_pressure.tier3_limit,
                memory_pressure.tier3_bytes,
                memory_pressure.tier4_limit,
                memory_pressure.tier4_bytes,
                memory_pressure.tier3_refusals,
                memory_pressure.tier4_refusals,
                memory_pressure.tier3_evictions,
            );
        }
        if !matches!(options.selection, Selection::Full) {
            println!(
                "selected {} flushes in {:.3} ms for {}",
                selected_flush_count,
                selected_boundary_time.as_secs_f64() * 1000.0,
                options.selection
            );
        }
        for (phase, elapsed) in &phase_times {
            println!("  {phase}: {:.3} ms", elapsed.as_secs_f64() * 1000.0);
        }
        amplification.print();
        if options.detailed_counters {
            println!("boundary counters:");
            println!("  internAtomCalls: {intern_atom_boundary_calls}");
            println!("  elementArrivals: {element_arrivals}");
            println!(
                "  elementFactCalls: {} ({:.2}/arrival)",
                element_fact_calls,
                element_fact_calls as f64 / element_arrivals.max(1) as f64
            );
            println!("  elementDeclarationCalls: {element_declaration_calls}");
            println!("  elementAnimationNameCalls: {element_animation_name_calls}");
            println!("  attributeValueTextQueries: {attribute_value_text_queries}");
            println!("  attributeValueTextPublications: {attribute_value_text_publications}");
            println!(
                "  selectorProgramCompilations: {}",
                selector_program_sharing.compilations
            );
            println!(
                "  selectorProgramsDistinct: documents={}, process={}",
                selector_program_sharing.document_distinct_count(),
                selector_program_sharing.process_hashes.len()
            );
            println!(
                "  selectorProgramBytesDistinct: documents={}, process={}",
                selector_program_sharing.document_distinct_bytes, selector_program_sharing.process_distinct_bytes
            );
        }
        detailed_counters.print(detailed_counter_reader.as_ref());
        reports.push(serde_json::json!({
            "path": path,
            "events": event_count,
            "flushes": flush_count,
            "document_engines": engine_count,
            "live_at_capture_exit": live_at_process_exit,
            "presence_degraded_exact_cascade_publication_comparisons": presence_degraded_publication_comparisons,
            "boundary_counters": {
                "intern_atom_calls": intern_atom_boundary_calls,
                "element_arrivals": element_arrivals,
                "element_fact_calls": element_fact_calls,
                "element_fact_calls_per_arrival": element_fact_calls as f64 / element_arrivals.max(1) as f64,
                "element_declaration_calls": element_declaration_calls,
                "element_animation_name_calls": element_animation_name_calls,
                "attribute_value_text_queries": attribute_value_text_queries,
                "attribute_value_text_publications": attribute_value_text_publications,
                "selector_program_compilations": selector_program_sharing.compilations,
                "selector_programs_distinct_documents": selector_program_sharing.document_distinct_count(),
                "selector_programs_distinct_process": selector_program_sharing.process_hashes.len(),
                "selector_program_bytes_distinct_documents": selector_program_sharing.document_distinct_bytes,
                "selector_program_bytes_distinct_process": selector_program_sharing.process_distinct_bytes,
            },
            "timing": {
                "boundary_ms": duration_ms(boundary_time),
                "selected_flushes": selected_flush_count,
                "selected_boundary_ms": duration_ms(selected_boundary_time),
                "phases_ms": phase_times
                    .iter()
                    .map(|(phase, elapsed)| (phase, duration_ms(*elapsed)))
                    .collect::<BTreeMap<_, _>>(),
            },
            "memory": memory_report(engine_count, &memory_pressure),
            "amplification": amplification.report(),
            "detailed_counters": detailed_counters.report(detailed_counter_reader.as_ref()),
        }));
    }
    if options.list {
        for subtest in &encountered_subtests {
            println!("{subtest}");
        }
    }
    if let Some(path) = &options.json_output {
        let output = serde_json::json!({
            "schema_version": 1,
            "selection": options.selection.as_json(),
            "assert_digests": options.assert_digests,
            "subtests": encountered_subtests,
            "captures": reports,
        });
        let file = std::fs::File::create(path)?;
        serde_json::to_writer_pretty(file, &output)?;
    }
    Ok(())
}

#[derive(Default)]
struct SelectorProgramSharing {
    compilations: u64,
    document_hashes: Vec<HashSet<u64>>,
    process_hashes: HashSet<u64>,
    document_distinct_bytes: u64,
    process_distinct_bytes: u64,
}

impl SelectorProgramSharing {
    fn resize_engines(&mut self, length: usize) {
        self.document_hashes.resize_with(length, HashSet::new);
    }

    fn record(&mut self, engine: usize, program: &SelectorProgram) {
        let mut hasher = DefaultHasher::new();
        program.hash(&mut hasher);
        let hash = hasher.finish();
        let bytes = program.capacity_bytes();
        self.compilations += 1;
        if self.document_hashes[engine].insert(hash) {
            self.document_distinct_bytes += bytes;
        }
        if self.process_hashes.insert(hash) {
            self.process_distinct_bytes += bytes;
        }
    }

    fn document_distinct_count(&self) -> usize {
        self.document_hashes.iter().map(HashSet::len).sum()
    }
}

#[derive(Clone)]
enum Selection {
    Full,
    Suite(String),
    Subtest { suite: String, test: String },
}

impl std::fmt::Display for Selection {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Full => formatter.write_str("the full capture"),
            Self::Suite(suite) => write!(formatter, "suite {suite}"),
            Self::Subtest { suite, test } => write!(formatter, "subtest {suite}/{test}"),
        }
    }
}

impl Selection {
    fn as_json(&self) -> serde_json::Value {
        match self {
            Self::Full => serde_json::json!({ "kind": "full" }),
            Self::Suite(suite) => serde_json::json!({ "kind": "suite", "suite": suite }),
            Self::Subtest { suite, test } => {
                serde_json::json!({ "kind": "subtest", "suite": suite, "test": test })
            }
        }
    }
}

struct Options {
    paths: Vec<PathBuf>,
    selection: Selection,
    assert_digests: bool,
    detailed_counters: bool,
    list: bool,
    json_output: Option<PathBuf>,
}

impl Options {
    fn parse() -> Result<Self, Box<dyn std::error::Error>> {
        Self::parse_from(env::args_os())
    }

    fn parse_from(arguments: impl IntoIterator<Item = std::ffi::OsString>) -> Result<Self, Box<dyn std::error::Error>> {
        let mut arguments = arguments.into_iter();
        let program = arguments.next().unwrap_or_default();
        let usage = || {
            format!(
                "usage: {} [--assert-digests] [--detailed-counters] [--list] [--json-output PATH] [--suite NAME | --subtest SUITE/TEST] <capture>...",
                program.to_string_lossy()
            )
        };
        let mut paths = Vec::new();
        let mut selection = Selection::Full;
        let mut assert_digests = false;
        let mut detailed_counters = false;
        let mut list = false;
        let mut json_output = None;
        while let Some(argument) = arguments.next() {
            match argument.to_str() {
                Some("--assert-digests") => assert_digests = true,
                Some("--detailed-counters") => detailed_counters = true,
                Some("--list") => list = true,
                Some("--json-output") => json_output = Some(PathBuf::from(arguments.next().ok_or_else(usage)?)),
                Some("--suite") => {
                    if !matches!(selection, Selection::Full) {
                        return Err(usage().into());
                    }
                    let suite = arguments.next().ok_or_else(usage)?.into_string().map_err(|_| usage())?;
                    selection = Selection::Suite(suite);
                }
                Some("--subtest") => {
                    if !matches!(selection, Selection::Full) {
                        return Err(usage().into());
                    }
                    let subtest = arguments.next().ok_or_else(usage)?.into_string().map_err(|_| usage())?;
                    let (suite, test) = subtest
                        .split_once('/')
                        .ok_or_else(|| "--subtest must be written as SUITE/TEST".to_owned())?;
                    selection = Selection::Subtest {
                        suite: suite.to_owned(),
                        test: test.to_owned(),
                    };
                }
                Some(value) if value.starts_with('-') => return Err(usage().into()),
                _ => paths.push(PathBuf::from(argument)),
            }
        }
        if paths.is_empty() {
            return Err(usage().into());
        }
        Ok(Self {
            paths,
            selection,
            assert_digests,
            detailed_counters,
            list,
            json_output,
        })
    }
}

#[derive(Clone, Debug)]
struct ActivePhase {
    suite: String,
    test: String,
    phase: &'static str,
}

struct BenchmarkMarker {
    suite: String,
    test: String,
    boundary: &'static str,
}

impl BenchmarkMarker {
    fn parse(name: &str) -> Option<Self> {
        let (base, boundary) = ["start", "sync-end", "async-end"]
            .into_iter()
            .find_map(|boundary| name.strip_suffix(&format!("-{boundary}")).map(|base| (base, boundary)))?;
        let (suite, test) = base.split_once('.')?;
        Some(Self {
            suite: suite.to_owned(),
            test: test.to_owned(),
            boundary,
        })
    }

    fn next_phase(&self) -> Option<ActivePhase> {
        match self.boundary {
            "start" => Some(ActivePhase {
                suite: self.suite.clone(),
                test: self.test.clone(),
                phase: "sync",
            }),
            "sync-end" => Some(ActivePhase {
                suite: self.suite.clone(),
                test: self.test.clone(),
                phase: "async",
            }),
            "async-end" => None,
            _ => unreachable!(),
        }
    }
}

fn record_timing(
    selection: &Selection,
    active_phase: Option<&ActivePhase>,
    elapsed: Duration,
    selected_flush_count: &mut u64,
    selected_flush_time: &mut Duration,
    phase_times: &mut BTreeMap<String, Duration>,
) -> bool {
    let selected = phase_is_selected(selection, active_phase);
    if !selected {
        return false;
    }
    *selected_flush_count += 1;
    record_boundary_timing(selection, active_phase, elapsed, selected_flush_time, phase_times);
    true
}

fn phase_is_selected(selection: &Selection, active_phase: Option<&ActivePhase>) -> bool {
    match selection {
        Selection::Full => true,
        Selection::Suite(suite) => active_phase.is_some_and(|phase| phase.suite == *suite),
        Selection::Subtest { suite, test } => {
            active_phase.is_some_and(|phase| phase.suite == *suite && phase.test == *test)
        }
    }
}

fn record_boundary_timing(
    selection: &Selection,
    active_phase: Option<&ActivePhase>,
    elapsed: Duration,
    selected_flush_time: &mut Duration,
    phase_times: &mut BTreeMap<String, Duration>,
) {
    if !phase_is_selected(selection, active_phase) {
        return;
    }
    *selected_flush_time += elapsed;
    let phase = active_phase.map_or_else(
        || "setup".to_owned(),
        |phase| format!("{}/{}", phase.suite, phase.phase),
    );
    *phase_times.entry(phase).or_default() += elapsed;
}

#[derive(Default)]
struct AmplificationLedger {
    flushes: u64,
    input_changed: u64,
    ingress_touched: u64,
    selector_changed: u64,
    selector_touched: u64,
    match_changed: u64,
    match_touched: u64,
    cascade_changed: u64,
    cascade_touched: u64,
}

#[derive(Clone, Copy, Default)]
struct AmplificationCounters {
    ingress_touched: u64,
    selector_changed: u64,
    selector_touched: u64,
    match_changed: u64,
    match_touched: u64,
    cascade_changed: u64,
    cascade_touched: u64,
}

struct AmplificationCounterReader {
    ingress_touched: usize,
    selector_changed: Vec<usize>,
    selector_touched: Vec<usize>,
    match_changed: usize,
    match_touched: Vec<usize>,
    cascade_changed: usize,
    cascade_touched: usize,
}

struct DetailedCounterReader {
    names: Vec<String>,
}

#[derive(Default)]
struct DetailedCounterLedger {
    values: Vec<u64>,
}

impl DetailedCounterReader {
    fn new(engine: *mut c_void) -> Self {
        let mut names = Vec::new();
        for index in 0.. {
            let mut value = 0_u64;
            let mut name_length = 0_usize;
            let name = unsafe { bridge::style_engine_counter(engine, index, &mut value, &mut name_length) };
            if name.is_null() {
                break;
            }
            let name = unsafe { std::slice::from_raw_parts(name, name_length) };
            names.push(String::from_utf8(name.to_vec()).unwrap());
        }
        Self { names }
    }

    fn read(&self, engine: *mut c_void) -> Vec<u64> {
        (0..self.names.len())
            .map(|index| read_counter_value(engine, index))
            .collect()
    }
}

impl DetailedCounterLedger {
    fn record(&mut self, before: &[u64], after: &[u64]) {
        self.values.resize(before.len(), 0);
        for (value, (&before, &after)) in self.values.iter_mut().zip(before.iter().zip(after)) {
            *value += after - before;
        }
    }

    fn print(&self, reader: Option<&DetailedCounterReader>) {
        let Some(reader) = reader else {
            return;
        };
        println!("detailed counters:");
        for (name, &value) in reader.names.iter().zip(&self.values) {
            if value != 0 {
                println!("  {name}: {value}");
            }
        }
    }

    fn report(&self, reader: Option<&DetailedCounterReader>) -> BTreeMap<String, u64> {
        let Some(reader) = reader else {
            return BTreeMap::new();
        };
        reader
            .names
            .iter()
            .cloned()
            .zip(self.values.iter().copied())
            .filter(|(_, value)| *value != 0)
            .collect()
    }
}

impl AmplificationCounterReader {
    fn new(engine: *mut c_void) -> Self {
        let mut reader = Self {
            ingress_touched: usize::MAX,
            selector_changed: Vec::new(),
            selector_touched: Vec::new(),
            match_changed: usize::MAX,
            match_touched: Vec::new(),
            cascade_changed: usize::MAX,
            cascade_touched: usize::MAX,
        };
        for index in 0.. {
            let mut value = 0_u64;
            let mut name_length = 0_usize;
            let name = unsafe { bridge::style_engine_counter(engine, index, &mut value, &mut name_length) };
            if name.is_null() {
                break;
            }
            let name = unsafe { std::slice::from_raw_parts(name, name_length) };
            match name {
                b"normalizedUniqueKeys" => reader.ingress_touched = index,
                b"selectorTruthAdditions" | b"selectorTruthRemovals" => reader.selector_changed.push(index),
                b"candidateChecks"
                | b"localFeatureTests"
                | b"stateTests"
                | b"combinatorSteps"
                | b"structuralTests"
                | b"relationalTests"
                | b"prefixCompoundsEvaluated"
                | b"prefixConvergenceNodes"
                | b"remainingPostingRowsInspected"
                | b"preparedMatchingBatchCompletenessRowsInspected" => reader.selector_touched.push(index),
                b"matchAnswerChanges" => reader.match_changed = index,
                b"coldNodesEvaluated"
                | b"ruleMatchesEmitted"
                | b"retainedMatchAnswerDeltaEntries"
                | b"preparedMatchingBatchRowsCloned" => reader.match_touched.push(index),
                b"cascadeNodeHandlesPublished" => reader.cascade_changed = index,
                b"cascadeMatchesBeforeCompaction" => reader.cascade_touched = index,
                _ => {}
            }
        }
        assert_ne!(reader.ingress_touched, usize::MAX);
        assert_eq!(reader.selector_changed.len(), 2);
        assert_eq!(reader.selector_touched.len(), 10);
        assert_ne!(reader.match_changed, usize::MAX);
        assert_eq!(reader.match_touched.len(), 4);
        assert_ne!(reader.cascade_changed, usize::MAX);
        assert_ne!(reader.cascade_touched, usize::MAX);
        reader
    }

    fn read(&self, engine: *mut c_void) -> AmplificationCounters {
        let sum = |indices: &[usize]| indices.iter().map(|&index| read_counter_value(engine, index)).sum();
        AmplificationCounters {
            ingress_touched: read_counter_value(engine, self.ingress_touched),
            selector_changed: sum(&self.selector_changed),
            selector_touched: sum(&self.selector_touched),
            match_changed: read_counter_value(engine, self.match_changed),
            match_touched: sum(&self.match_touched),
            cascade_changed: read_counter_value(engine, self.cascade_changed),
            cascade_touched: read_counter_value(engine, self.cascade_touched),
        }
    }
}

impl AmplificationLedger {
    fn record(&mut self, input_changed: u64, before: &AmplificationCounters, after: &AmplificationCounters) {
        self.flushes += 1;
        self.input_changed += input_changed;
        self.ingress_touched += after.ingress_touched - before.ingress_touched;
        self.selector_changed += after.selector_changed - before.selector_changed;
        self.selector_touched += after.selector_touched - before.selector_touched;
        self.match_changed += after.match_changed - before.match_changed;
        self.match_touched += after.match_touched - before.match_touched;
        self.cascade_changed += after.cascade_changed - before.cascade_changed;
        self.cascade_touched += after.cascade_touched - before.cascade_touched;
    }

    fn print(&self) {
        if self.flushes == 0 {
            return;
        }
        println!("amplification ledger ({} selected flushes):", self.flushes);
        self.print_row("ingress", self.input_changed, self.ingress_touched);
        self.print_row("selector", self.selector_changed, self.selector_touched);
        self.print_row("match", self.match_changed, self.match_touched);
        self.print_row("cascade", self.cascade_changed, self.cascade_touched);
    }

    fn print_row(&self, stage: &str, changed: u64, touched: u64) {
        let amplification = match changed {
            0 => "n/a".to_owned(),
            _ => format!("{:.2}x", touched as f64 / changed as f64),
        };
        println!(
            "  {stage}: {changed} rows changed, {touched} rows touched ({amplification}, {:.1} touched/flush)",
            touched as f64 / self.flushes as f64
        );
    }

    fn report(&self) -> serde_json::Value {
        serde_json::json!({
            "selected_flushes": self.flushes,
            "stages": {
                "ingress": amplification_stage_report(self.input_changed, self.ingress_touched, self.flushes),
                "selector": amplification_stage_report(self.selector_changed, self.selector_touched, self.flushes),
                "match": amplification_stage_report(self.match_changed, self.match_touched, self.flushes),
                "cascade": amplification_stage_report(self.cascade_changed, self.cascade_touched, self.flushes),
            },
        })
    }
}

fn duration_ms(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1000.0
}

fn amplification_stage_report(changed_rows: u64, touched_rows: u64, flushes: u64) -> serde_json::Value {
    let amplification = (changed_rows != 0).then(|| touched_rows as f64 / changed_rows as f64);
    let touched_per_flush = (flushes != 0).then(|| touched_rows as f64 / flushes as f64);
    serde_json::json!({
        "changed_rows": changed_rows,
        "touched_rows": touched_rows,
        "amplification": amplification,
        "touched_per_flush": touched_per_flush,
    })
}

fn read_counter_value(engine: *mut c_void, index: usize) -> u64 {
    let mut value = 0_u64;
    let mut name_length = 0_usize;
    let name = unsafe { bridge::style_engine_counter(engine, index, &mut value, &mut name_length) };
    assert!(!name.is_null(), "cached counter index is out of range");
    value
}

fn accumulate_memory_pressure(total: &mut FfiMemoryPressureSnapshot, current: FfiMemoryPressureSnapshot) {
    total.tier3_limit = total.tier3_limit.max(current.tier3_limit);
    total.tier4_limit = total.tier4_limit.max(current.tier4_limit);
    total.tier3_bytes = total.tier3_bytes.saturating_add(current.tier3_bytes);
    total.tier4_bytes = total.tier4_bytes.saturating_add(current.tier4_bytes);
    total.tier3_refusals = total.tier3_refusals.saturating_add(current.tier3_refusals);
    total.tier4_refusals = total.tier4_refusals.saturating_add(current.tier4_refusals);
    for (total, current) in total
        .tier3_refusal_categories
        .iter_mut()
        .zip(current.tier3_refusal_categories)
    {
        *total = total.saturating_add(current);
    }
    for (total, current) in total
        .tier4_refusal_categories
        .iter_mut()
        .zip(current.tier4_refusal_categories)
    {
        *total = total.saturating_add(current);
    }
    total.tier3_evictions = total.tier3_evictions.saturating_add(current.tier3_evictions);
    for (total, current) in total.category_bytes.iter_mut().zip(current.category_bytes) {
        *total = total.saturating_add(current);
    }
}

fn memory_tier_totals(snapshot: &FfiMemoryPressureSnapshot) -> [u64; TIER_COUNT] {
    let mut totals = [0_u64; TIER_COUNT];
    for (category, bytes) in MEMORY_CATEGORIES.iter().zip(snapshot.category_bytes) {
        totals[category.tier().index()] = totals[category.tier().index()].saturating_add(bytes);
    }
    totals
}

fn memory_report(engine_count: u64, snapshot: &FfiMemoryPressureSnapshot) -> serde_json::Value {
    let tiers = memory_tier_totals(snapshot);
    let categories = MEMORY_CATEGORIES
        .iter()
        .zip(snapshot.category_bytes)
        .map(|(category, bytes)| (category.name(), bytes))
        .collect::<BTreeMap<_, _>>();
    let tier3_refusal_categories = TIER3_REFUSAL_CATEGORIES
        .into_iter()
        .map(|category| category.name())
        .zip(snapshot.tier3_refusal_categories)
        .collect::<BTreeMap<_, _>>();
    let tier4_refusal_categories = ["normalizationJournal", "batchScratch"]
        .into_iter()
        .zip(snapshot.tier4_refusal_categories)
        .collect::<BTreeMap<_, _>>();
    serde_json::json!({
        "engine_count": engine_count,
        "total_bytes": tiers.iter().sum::<u64>(),
        "tiers": {
            "authoritative_bytes": tiers[Tier::Authoritative.index()],
            "live_bytes": tiers[Tier::Live.index()],
            "program_bytes": tiers[Tier::Program.index()],
            "acceleration_bytes": tiers[Tier::Acceleration.index()],
            "scratch_bytes": tiers[Tier::Scratch.index()],
        },
        "categories_bytes": categories,
        "pressure": {
            "tier3_limit_bytes": snapshot.tier3_limit,
            "tier3_resident_bytes": snapshot.tier3_bytes,
            "tier4_limit_bytes": snapshot.tier4_limit,
            "tier4_resident_bytes": snapshot.tier4_bytes,
            "tier3_refusals": snapshot.tier3_refusals,
            "tier4_refusals": snapshot.tier4_refusals,
            "tier3_refusals_by_category": tier3_refusal_categories,
            "tier4_refusals_by_category": tier4_refusal_categories,
            "tier3_evictions": snapshot.tier3_evictions,
        },
    })
}

fn print_memory_residency(engine_count: u64, snapshot: &FfiMemoryPressureSnapshot) {
    let tiers = memory_tier_totals(snapshot);
    let total = tiers.iter().sum::<u64>();
    println!(
        "memory at engine retirement (sum across {engine_count} engines): total={total} bytes; live={} bytes; program={} bytes; acceleration={} bytes; scratch={} bytes",
        tiers[Tier::Live.index()],
        tiers[Tier::Program.index()],
        tiers[Tier::Acceleration.index()],
        tiers[Tier::Scratch.index()]
    );
    let categories = MEMORY_CATEGORIES
        .iter()
        .zip(snapshot.category_bytes)
        .filter(|(_, bytes)| *bytes != 0)
        .map(|(category, bytes)| format!("{}={bytes}", category.name()))
        .collect::<Vec<_>>()
        .join(", ");
    println!("  categories: {categories}");
}

fn print_memory_pressure(label: &str, snapshot: FfiMemoryPressureSnapshot) {
    let tier3_categories = TIER3_REFUSAL_CATEGORIES
        .into_iter()
        .map(|category| category.name())
        .zip(snapshot.tier3_refusal_categories)
        .filter(|(_, count)| *count != 0)
        .map(|(name, count)| format!("{name}: {count}"))
        .collect::<Vec<_>>()
        .join(", ");
    eprintln!(
        "memory pressure at {label}: tier3 limit={} bytes, resident={} bytes; tier4 limit={} bytes, resident={} bytes; refusals={{tier3: {}, tier4: {}}} [{}]; tier3 evictions={}",
        snapshot.tier3_limit,
        snapshot.tier3_bytes,
        snapshot.tier4_limit,
        snapshot.tier4_bytes,
        snapshot.tier3_refusals,
        snapshot.tier4_refusals,
        tier3_categories,
        snapshot.tier3_evictions,
    );
}

fn read_document_style_computation_inputs(
    payload: &mut PayloadReader,
    format_version: u64,
    registry: *const c_void,
) -> Result<bridge::FfiDocumentStyleComputationInputs, Box<dyn std::error::Error>> {
    Ok(bridge::FfiDocumentStyleComputationInputs {
        viewport_width: f64::from_bits(payload.read_u64()?),
        viewport_height: f64::from_bits(payload.read_u64()?),
        root_font_size: f64::from_bits(payload.read_u64()?),
        root_font_x_height: f64::from_bits(payload.read_u64()?),
        root_font_cap_height: f64::from_bits(payload.read_u64()?),
        root_font_zero_advance: f64::from_bits(payload.read_u64()?),
        root_line_height: f64::from_bits(payload.read_u64()?),
        root_font_metrics_depend_on_viewport_metrics: payload.read_bool()?,
        initial_font_size: if format_version >= 12 {
            f64::from_bits(payload.read_u64()?)
        } else {
            0.0
        },
        initial_font_x_height: if format_version >= 12 {
            f64::from_bits(payload.read_u64()?)
        } else {
            0.0
        },
        initial_font_cap_height: if format_version >= 12 {
            f64::from_bits(payload.read_u64()?)
        } else {
            0.0
        },
        initial_font_zero_advance: if format_version >= 12 {
            f64::from_bits(payload.read_u64()?)
        } else {
            0.0
        },
        initial_font_size_raw: payload.read_i32()?,
        default_font_size_raw: payload.read_i32()?,
        device_pixels_per_css_pixel: f64::from_bits(payload.read_u64()?),
        font_environment_generation: payload.read_u64()?,
        preferred_color_scheme: if format_version >= 12 { payload.read_u8()? } else { 0 },
        has_document_supported_schemes: if format_version >= 12 {
            payload.read_bool()?
        } else {
            false
        },
        document_supported_scheme_count: if format_version >= 12 { payload.read_u8()? } else { 0 },
        document_supported_scheme_codes: if format_version >= 12 {
            [
                payload.read_u8()?,
                payload.read_u8()?,
                payload.read_u8()?,
                payload.read_u8()?,
            ]
        } else {
            [0; 4]
        },
        custom_property_registry: if format_version >= 13 && payload.read_bool()? {
            registry
        } else {
            std::ptr::null()
        },
        custom_property_registration_generation: if format_version >= 13 { payload.read_u64()? } else { 0 },
        in_quirks_mode: format_version >= 15 && payload.read_bool()?,
        ..Default::default()
    })
}

#[inline]
fn read_engine_indexed(
    payload: &mut PayloadReader,
    live_engines: &[Option<*mut c_void>],
) -> Result<(usize, *mut c_void), Box<dyn std::error::Error>> {
    let engine_id = payload.read_u64()?;
    let index = usize::try_from(engine_id)?;
    let Some(pointer) = live_engines.get(index).copied().flatten() else {
        return engine_not_live(engine_id);
    };
    Ok((index, pointer))
}

fn release_computed_longhand_tables(
    tables: &mut Vec<Option<*mut libweb_rust::css::computed_longhand_table::ComputedLonghandTable>>,
) {
    for table in tables.drain(..).flatten() {
        unsafe { libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_release(table) };
    }
}

fn style_record_replay_index(style_record: u64) -> Result<usize, std::num::TryFromIntError> {
    const ANIMATION_OVERLAY_TAG: u64 = 1 << 63;
    const BASE_IDENTITY_MASK: u64 = u32::MAX as u64;

    let identity = style_record & BASE_IDENTITY_MASK;
    let namespace = u64::from(style_record & ANIMATION_OVERLAY_TAG != 0);
    usize::try_from(identity * 2 + namespace)
}

#[inline]
fn read_engine(
    payload: &mut PayloadReader,
    live_engines: &[Option<*mut c_void>],
) -> Result<*mut c_void, Box<dyn std::error::Error>> {
    let engine_id = payload.read_u64()?;
    let Some(pointer) = usize::try_from(engine_id)
        .ok()
        .and_then(|index| live_engines.get(index).copied().flatten())
    else {
        return engine_not_live(engine_id);
    };
    Ok(pointer)
}

#[cold]
fn engine_not_live<T>(engine_id: u64) -> Result<T, Box<dyn std::error::Error>> {
    Err(format!("event names engine {engine_id}, which is not live").into())
}

fn replay_pointer(token: u64) -> *const c_void {
    match token {
        0 => std::ptr::null(),
        token => usize::try_from(token).expect("pointer token exceeds usize") as *const c_void,
    }
}

fn assert_identity(category: &str, expected: u32, actual: u32) -> Result<(), Box<dyn std::error::Error>> {
    if actual != expected {
        return Err(format!("{category} allocation diverged: expected {expected}, got {actual}").into());
    }
    Ok(())
}

fn read_rule_matches(payload: &mut PayloadReader) -> Result<Vec<FfiRuleMatch>, Box<dyn std::error::Error>> {
    let count = payload.read_length()?;
    let mut matches = Vec::with_capacity(count);
    for _ in 0..count {
        matches.push(FfiRuleMatch {
            node: payload.read_u32()?,
            rule: payload.read_u32()?,
            semantic_declaration: 0,
            pseudo_element: payload.read_u32()?,
            scope_host: payload.read_u32()?,
            scope_proximity: payload.read_u32()?,
        });
    }
    Ok(matches)
}

fn rule_matches_equal(actual: &[FfiRuleMatch], expected: &[FfiRuleMatch]) -> bool {
    actual.len() == expected.len()
        && actual.iter().zip(expected).all(|(actual, expected)| {
            actual.node == expected.node
                && actual.rule == expected.rule
                && actual.pseudo_element == expected.pseudo_element
                && actual.scope_host == expected.scope_host
                && actual.scope_proximity == expected.scope_proximity
        })
}

#[derive(Clone)]
struct StyleTransactionEmission {
    transaction_version: u64,
    program_version: u64,
    answers: Vec<FfiStyleDelta>,
}

struct StyleTransactionOutputs {
    result: bool,
    filter_calls: Vec<(Vec<u32>, Vec<u32>)>,
    emissions: Vec<StyleTransactionEmission>,
    style_atoms_swept: bool,
    reclaimed_atoms: Vec<u32>,
}

struct ReplayStyleTransactionContext {
    expected_emissions: VecDeque<StyleTransactionEmission>,
    actual_emissions: Vec<StyleTransactionEmission>,
    match_answer_identity_mapping: *mut MatchAnswerIdentityMapping,
    error: Option<String>,
}

#[derive(Default)]
struct MatchAnswerIdentityMapping {
    actual_by_expected: Vec<Option<u32>>,
    expected_by_actual: Vec<Option<u32>>,
}

impl MatchAnswerIdentityMapping {
    fn record(&mut self, expected: u32, actual: u32) -> Result<(), String> {
        let expected_index = expected as usize;
        let actual_index = actual as usize;
        if self.actual_by_expected.len() <= expected_index {
            self.actual_by_expected.resize(expected_index + 1, None);
        }
        if self.expected_by_actual.len() <= actual_index {
            self.expected_by_actual.resize(actual_index + 1, None);
        }
        match self.actual_by_expected[expected_index] {
            Some(mapped) if mapped != actual => {
                return Err(format!(
                    "identity diverged: recorded {expected} previously mapped to {mapped}, got {actual}"
                ));
            }
            Some(_) | None => self.actual_by_expected[expected_index] = Some(actual),
        }
        match self.expected_by_actual[actual_index] {
            Some(mapped) if mapped != expected => {
                return Err(format!(
                    "identity alias: replayed {actual} previously mapped from {mapped}, got {expected}"
                ));
            }
            Some(_) | None => self.expected_by_actual[actual_index] = Some(expected),
        }
        Ok(())
    }
}

fn replay_style_transaction_output(
    context: &mut ReplayStyleTransactionContext,
    transaction_version: u64,
    program_version: u64,
    answers: &[FfiStyleDelta],
) {
    let actual = StyleTransactionEmission {
        transaction_version,
        program_version,
        answers: answers.to_vec(),
    };
    let Some(expected) = context.expected_emissions.pop_front() else {
        context
            .error
            .get_or_insert_with(|| "style transaction returned extra output".into());
        return;
    };
    if actual.answers.len() == expected.answers.len() {
        let identity_mapping = unsafe { &mut *context.match_answer_identity_mapping };
        for (actual, expected) in actual.answers.iter().zip(&expected.answers) {
            if let Err(error) = identity_mapping.record(expected.match_answer, actual.match_answer) {
                context.error.get_or_insert(error);
                return;
            }
        }
    }
    if actual.transaction_version != expected.transaction_version
        || actual.program_version != expected.program_version
        || actual.answers.len() != expected.answers.len()
        || actual.answers.iter().zip(&expected.answers).any(|(actual, expected)| {
            actual.style_node != expected.style_node
                || actual.reaction != expected.reaction
                || actual.inherited_style_groups != expected.inherited_style_groups
                || actual.old_style_record != expected.old_style_record
                || actual.new_style_record != expected.new_style_record
                || actual.damage != expected.damage
                || actual.pseudo_kind != expected.pseudo_kind
                || actual.gap != expected.gap
        })
    {
        context.error.get_or_insert_with(|| {
            format!(
                "style transaction output diverged: expected versions {}/{} and {:?}, got versions {}/{} and {:?}",
                expected.transaction_version,
                expected.program_version,
                expected.answers,
                actual.transaction_version,
                actual.program_version,
                actual.answers
            )
        });
    }
    context.actual_emissions.push(expected);
}

fn read_style_transaction_outputs(
    mut payload: PayloadReader,
) -> Result<StyleTransactionOutputs, Box<dyn std::error::Error>> {
    let result = payload.read_bool()?;
    let filter_count = payload.read_length()?;
    let mut filter_calls = Vec::with_capacity(filter_count);
    for _ in 0..filter_count {
        filter_calls.push((payload.read_u32_vec()?, payload.read_u32_vec()?));
    }
    let emission_count = payload.read_length()?;
    let mut emissions = Vec::with_capacity(emission_count);
    for _ in 0..emission_count {
        let transaction_version = payload.read_u64()?;
        let program_version = payload.read_u64()?;
        let answer_count = payload.read_length()?;
        let mut answers = Vec::with_capacity(answer_count);
        for _ in 0..answer_count {
            answers.push(FfiStyleDelta {
                style_node: payload.read_u32()?,
                match_answer: payload.read_u32()?,
                old_style_record: payload.read_u64()?,
                new_style_record: payload.read_u64()?,
                damage: match payload.read_u16()? {
                    0 => FfiStyleDeltaDamage::None,
                    1 => FfiStyleDeltaDamage::Full,
                    tag => return Err(format!("unknown style delta damage tag {tag}").into()),
                },
                reaction: payload.read_u8()?,
                inherited_style_groups: payload.read_u8()?,
                pseudo_kind: payload.read_u8()?,
                gap: match payload.read_u8()? {
                    0 => FfiStyleDeltaGap::None,
                    1 => FfiStyleDeltaGap::Materialize,
                    2 => FfiStyleDeltaGap::Computed,
                    3 => FfiStyleDeltaGap::RetryAfterAncestor,
                    tag => return Err(format!("unknown style delta gap tag {tag}").into()),
                },
            });
        }
        emissions.push(StyleTransactionEmission {
            transaction_version,
            program_version,
            answers,
        });
    }
    let style_atoms_swept = payload.read_bool()?;
    let reclaimed_atoms = payload.read_u32_vec()?;
    payload.finish()?;
    Ok(StyleTransactionOutputs {
        result,
        filter_calls,
        emissions,
        style_atoms_swept,
        reclaimed_atoms,
    })
}

fn write_style_transaction_outputs(outputs: &StyleTransactionOutputs, payload: &mut PayloadWriter) {
    payload.write_bool(outputs.result);
    payload.write_length(outputs.filter_calls.len());
    for (before, after) in &outputs.filter_calls {
        payload.write_u32_slice(before);
        payload.write_u32_slice(after);
    }
    payload.write_length(outputs.emissions.len());
    for emission in &outputs.emissions {
        payload.write_u64(emission.transaction_version);
        payload.write_u64(emission.program_version);
        payload.write_length(emission.answers.len());
        for answer in &emission.answers {
            payload.write_u32(answer.style_node);
            payload.write_u32(answer.match_answer);
            payload.write_u64(answer.old_style_record);
            payload.write_u64(answer.new_style_record);
            payload.write_u16(answer.damage as u16);
            payload.write_u8(answer.reaction);
            payload.write_u8(answer.inherited_style_groups);
            payload.write_u8(answer.pseudo_kind);
            payload.write_u8(answer.gap as u8);
        }
    }
    payload.write_bool(outputs.style_atoms_swept);
    payload.write_u32_slice(&outputs.reclaimed_atoms);
}

fn replay_atom_mappings(engine: *mut c_void, payload: &mut PayloadReader) -> Result<(), Box<dyn std::error::Error>> {
    let count = payload.read_length()?;
    for _ in 0..count {
        match payload.read_u8()? {
            0 => {
                let token = usize::try_from(payload.read_u64()?)?;
                let expected = payload.read_u32()?;
                let actual = unsafe { bridge::style_engine_intern_atom(engine, token) };
                assert_identity("selector atom", expected, actual)?;
            }
            1 => {
                let namespace = payload.read_u32()?;
                let name = payload.read_u32()?;
                let expected = payload.read_u32()?;
                let actual = unsafe { bridge::style_engine_intern_qualified_atom(engine, namespace, name) };
                assert_identity("selector qualified atom", expected, actual)?;
            }
            tag => return Err(format!("unknown selector atom mapping tag {tag}").into()),
        }
    }
    Ok(())
}

fn read_declared_properties(payload: &mut PayloadReader) -> Result<Vec<DeclaredProperty>, Box<dyn std::error::Error>> {
    let count = payload.read_length()?;
    let mut declared = Vec::with_capacity(count);
    for _ in 0..count {
        declared.push(DeclaredProperty {
            property: payload.read_u16()?,
            important: payload.read_bool()?,
            operator: read_cascade_operator(payload)?,
            value: SpecifiedValueID(payload.read_u64()?),
        });
    }
    Ok(declared)
}

fn read_custom_declarations(payload: &mut PayloadReader) -> Result<Vec<CustomDeclaration>, Box<dyn std::error::Error>> {
    let count = payload.read_length()?;
    let mut declared = Vec::with_capacity(count);
    for _ in 0..count {
        declared.push(CustomDeclaration {
            name: StyleAtomID(payload.read_u32()?),
            important: payload.read_bool()?,
            operator: read_cascade_operator(payload)?,
            value: SpecifiedValueID(payload.read_u64()?),
        });
    }
    Ok(declared)
}

fn read_cascade_operator(payload: &mut PayloadReader) -> Result<CascadeOperator, Box<dyn std::error::Error>> {
    match payload.read_u8()? {
        0 => Ok(CascadeOperator::Declared),
        1 => Ok(CascadeOperator::Inherit),
        2 => Ok(CascadeOperator::Initial),
        3 => Ok(CascadeOperator::Unset),
        4 => Ok(CascadeOperator::Revert),
        5 => Ok(CascadeOperator::RevertLayer),
        value => Err(format!("unknown cascade operator {value}").into()),
    }
}

fn read_exact_cascade_publication(
    payload: &mut PayloadReader,
) -> Result<FfiExactCascadePublication, Box<dyn std::error::Error>> {
    Ok(FfiExactCascadePublication {
        computed_group_mask: payload.read_u32()?,
        unchanged: payload.read_bool()?,
        donor_used: payload.read_bool()?,
    })
}

fn exact_cascade_publications_match(
    expected: FfiExactCascadePublication,
    mut actual: FfiExactCascadePublication,
    presence_is_comparable: bool,
) -> bool {
    if !presence_is_comparable {
        actual.computed_group_mask = expected.computed_group_mask;
    }
    actual == expected
}

fn require_matching_verification_gates(recorded: u8, actual: u8) -> Result<(), String> {
    if recorded == actual {
        Ok(())
    } else {
        Err(format!(
            "verification gates diverged: recording used {recorded:#04x}, replay uses {actual:#04x}"
        ))
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct EncodedU64Slice<'a> {
    bytes: &'a [u8],
}

impl EncodedU64Slice<'_> {
    fn len(self) -> usize {
        self.bytes.len() / size_of::<u64>()
    }

    fn is_empty(self) -> bool {
        self.bytes.is_empty()
    }

    fn iter(self) -> impl Iterator<Item = u64> {
        self.bytes
            .chunks_exact(size_of::<u64>())
            .map(|bytes| u64::from_le_bytes(bytes.try_into().unwrap()))
    }
}

impl std::fmt::Debug for EncodedU64Slice<'_> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_list().entries(self.iter()).finish()
    }
}

fn read_u64_slice<'a>(payload: &mut PayloadReader<'a>) -> Result<EncodedU64Slice<'a>, Box<dyn std::error::Error>> {
    Ok(EncodedU64Slice {
        bytes: payload.read_fixed_width_slice(size_of::<u64>())?,
    })
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct EncodedInheritanceDependentValues<'a> {
    bytes: &'a [u8],
}

impl EncodedInheritanceDependentValues<'_> {
    const ENTRY_SIZE: usize = size_of::<u16>() + size_of::<u64>();

    fn len(self) -> usize {
        self.bytes.len() / Self::ENTRY_SIZE
    }

    fn is_empty(self) -> bool {
        self.bytes.is_empty()
    }

    fn iter(self) -> impl Iterator<Item = (u16, u64)> {
        self.bytes.chunks_exact(Self::ENTRY_SIZE).map(|bytes| {
            let property = u16::from_le_bytes(bytes[..size_of::<u16>()].try_into().unwrap());
            let value = u64::from_le_bytes(bytes[size_of::<u16>()..].try_into().unwrap());
            (property, value)
        })
    }
}

impl std::fmt::Debug for EncodedInheritanceDependentValues<'_> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_list().entries(self.iter()).finish()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct RecordedStyleRecordView<'a> {
    payloads: EncodedU64Slice<'a>,
    base_payloads: EncodedU64Slice<'a>,
    property_importance: &'a [u8],
    property_inheritance: &'a [u8],
    inheritance_dependent_values: EncodedInheritanceDependentValues<'a>,
    raw_cascaded_font_size: u64,
    animated_overlay_present: bool,
    pseudo_element_styles: u64,
    counter_style_environment_identity: u64,
    animation_overlay_identity: u64,
    dependency_flags: u8,
    longhand_values: EncodedU64Slice<'a>,
}

fn read_style_record_view<'a>(
    payload: &mut PayloadReader<'a>,
) -> Result<RecordedStyleRecordView<'a>, Box<dyn std::error::Error>> {
    let payloads = read_u64_slice(payload)?;
    let base_payloads = read_u64_slice(payload)?;
    let property_importance = payload.read_bytes()?;
    let property_inheritance = payload.read_bytes()?;
    let inheritance_dependent_values = EncodedInheritanceDependentValues {
        bytes: payload.read_fixed_width_slice(EncodedInheritanceDependentValues::ENTRY_SIZE)?,
    };
    Ok(RecordedStyleRecordView {
        payloads,
        base_payloads,
        property_importance,
        property_inheritance,
        inheritance_dependent_values,
        raw_cascaded_font_size: payload.read_u64()?,
        animated_overlay_present: payload.read_u64()? != 0,
        pseudo_element_styles: payload.read_u64()?,
        counter_style_environment_identity: payload.read_u64()?,
        animation_overlay_identity: payload.read_u64()?,
        dependency_flags: payload.read_u8()?,
        longhand_values: read_u64_slice(payload)?,
    })
}

fn style_record_view_matches(
    expected: &RecordedStyleRecordView,
    actual: bridge::FfiStyleRecordView,
) -> Result<bool, std::num::TryFromIntError> {
    let (actual_longhand_values, actual_longhand_value_count) = style_record_longhand_values(actual);
    let (actual_property_importance, actual_property_inheritance, actual_property_flag_count) =
        style_record_property_flags(actual);
    let (actual_inheritance_dependent_values, actual_inheritance_dependent_value_count) =
        style_record_inheritance_dependent_values(actual);
    if actual.payload_count != expected.payloads.len()
        || actual.payload_count != expected.base_payloads.len()
        || actual_property_flag_count != expected.property_importance.len()
        || actual_property_flag_count != expected.property_inheritance.len()
        || actual_inheritance_dependent_value_count != expected.inheritance_dependent_values.len()
        || actual_longhand_value_count != expected.longhand_values.len()
    {
        return Ok(false);
    }
    let pointers_match =
        |pointer: *const *const c_void, expected: EncodedU64Slice<'_>| -> Result<bool, std::num::TryFromIntError> {
            if expected.is_empty() {
                return Ok(true);
            }
            unsafe { std::slice::from_raw_parts(pointer, expected.len()) }
                .iter()
                .zip(expected.iter())
                .try_fold(true, |matches, (&actual, expected)| {
                    Ok(matches && pointer_value(actual)? == expected)
                })
        };
    let bytes_match = |pointer: *const u8, expected: &[u8]| {
        expected.is_empty() || unsafe { std::slice::from_raw_parts(pointer, expected.len()) } == expected
    };
    let inheritance_dependent_values_match = if expected.inheritance_dependent_values.is_empty() {
        true
    } else {
        unsafe {
            std::slice::from_raw_parts(
                actual_inheritance_dependent_values,
                actual_inheritance_dependent_value_count,
            )
        }
        .iter()
        .zip(expected.inheritance_dependent_values.iter())
        .try_fold(true, |matches, (actual, expected)| {
            Ok::<bool, std::num::TryFromIntError>(
                matches && (actual.property, pointer_value(actual.value)?) == expected,
            )
        })?
    };
    Ok(pointers_match(actual.payloads, expected.payloads)?
        && pointers_match(actual.base_payloads, expected.base_payloads)?
        && pointers_match(actual_longhand_values, expected.longhand_values)?
        && bytes_match(actual_property_importance, &expected.property_importance)
        && bytes_match(actual_property_inheritance, &expected.property_inheritance)
        && inheritance_dependent_values_match
        && pointer_value(style_record_raw_cascaded_font_size(actual))? == expected.raw_cascaded_font_size
        && !actual.animated_overlay.is_null() == expected.animated_overlay_present
        && actual.pseudo_element_styles == expected.pseudo_element_styles
        && actual.counter_style_environment_identity == expected.counter_style_environment_identity
        && actual.animation_overlay_identity == expected.animation_overlay_identity
        && actual.dependency_flags == expected.dependency_flags)
}

fn semantic_style_record_view(
    view: bridge::FfiStyleRecordView,
) -> Result<OwnedSemanticStyleRecordView, Box<dyn std::error::Error>> {
    let (longhand_values, longhand_value_count) = style_record_longhand_values(view);
    let (property_importance, property_inheritance, property_flag_count) = style_record_property_flags(view);
    let (inheritance_dependent_values, inheritance_dependent_value_count) =
        style_record_inheritance_dependent_values(view);
    let pointers = |pointer: *const *const c_void, count: usize| -> Result<Vec<u64>, Box<dyn std::error::Error>> {
        if count == 0 {
            return Ok(Vec::new());
        }
        let mut values = Vec::with_capacity(count);
        for &pointer in unsafe { std::slice::from_raw_parts(pointer, count) } {
            values.push(pointer_value(pointer)?);
        }
        Ok(values)
    };
    let bytes = |pointer: *const u8, count: usize| match count {
        0 => Vec::new(),
        _ => unsafe { std::slice::from_raw_parts(pointer, count) }.to_vec(),
    };
    let inheritance_dependent_values = match inheritance_dependent_value_count {
        0 => Vec::new(),
        count => {
            let entries = unsafe { std::slice::from_raw_parts(inheritance_dependent_values, count) };
            let mut values = Vec::with_capacity(count);
            for entry in entries {
                values.push((entry.property, pointer_value(entry.value)?));
            }
            values
        }
    };
    Ok(OwnedSemanticStyleRecordView {
        payloads: pointers(view.payloads, view.payload_count)?,
        base_payloads: pointers(view.base_payloads, view.payload_count)?,
        longhand_values: pointers(longhand_values, longhand_value_count)?,
        property_importance: bytes(property_importance, property_flag_count),
        property_inheritance: bytes(property_inheritance, property_flag_count),
        inheritance_dependent_values,
        raw_cascaded_font_size: pointer_value(style_record_raw_cascaded_font_size(view))?,
        animated_overlay_present: !view.animated_overlay.is_null(),
        pseudo_element_styles: view.pseudo_element_styles,
        counter_style_environment_identity: view.counter_style_environment_identity,
        animation_overlay_identity: view.animation_overlay_identity,
        dependency_flags: view.dependency_flags,
    })
}

fn style_record_property_flags(view: bridge::FfiStyleRecordView) -> (*const u8, *const u8, usize) {
    if view.longhand_table.is_null() {
        return (std::ptr::null(), std::ptr::null(), 0);
    }
    let longhand_count = (libweb_rust::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
        - libweb_rust::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
        + 1) as usize;
    let count = longhand_count.div_ceil(8);
    unsafe {
        (
            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_importance_bits(
                view.longhand_table.cast(),
            ),
            libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_inheritance_bits(
                view.longhand_table.cast(),
            ),
            count,
        )
    }
}

fn style_record_inheritance_dependent_values(
    view: bridge::FfiStyleRecordView,
) -> (
    *const libweb_rust::css::computed_longhand_table::FfiTableInheritanceDependentValue,
    usize,
) {
    if view.longhand_table.is_null() {
        return (std::ptr::null(), 0);
    }
    let mut count = 0;
    let values = unsafe {
        libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_inheritance_dependent_values(
            view.longhand_table.cast(),
            &raw mut count,
        )
    };
    (values, count)
}

fn style_record_raw_cascaded_font_size(view: bridge::FfiStyleRecordView) -> *const c_void {
    if view.longhand_table.is_null() {
        return std::ptr::null();
    }
    unsafe {
        libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_raw_cascaded_font_size(
            view.longhand_table.cast(),
        )
    }
}

fn style_record_longhand_values(view: bridge::FfiStyleRecordView) -> (*const *const c_void, usize) {
    if view.longhand_table.is_null() {
        return (std::ptr::null(), 0);
    }
    let count = (libweb_rust::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
        - libweb_rust::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
        + 1) as usize;
    let values = unsafe {
        libweb_rust::css::computed_longhand_table::rust_computed_longhand_table_values(view.longhand_table.cast())
    };
    (values, count)
}

struct OwnedSemanticStyleRecordView {
    payloads: Vec<u64>,
    base_payloads: Vec<u64>,
    longhand_values: Vec<u64>,
    property_importance: Vec<u8>,
    property_inheritance: Vec<u8>,
    inheritance_dependent_values: Vec<(u16, u64)>,
    raw_cascaded_font_size: u64,
    animated_overlay_present: bool,
    pseudo_element_styles: u64,
    counter_style_environment_identity: u64,
    animation_overlay_identity: u64,
    dependency_flags: u8,
}

impl std::fmt::Debug for OwnedSemanticStyleRecordView {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("SemanticStyleRecordView")
            .field("payloads", &self.payloads)
            .field("base_payloads", &self.base_payloads)
            .field("longhand_values", &self.longhand_values)
            .field("property_importance", &self.property_importance)
            .field("property_inheritance", &self.property_inheritance)
            .field("inheritance_dependent_values", &self.inheritance_dependent_values)
            .field("raw_cascaded_font_size", &self.raw_cascaded_font_size)
            .field("animated_overlay_present", &self.animated_overlay_present)
            .field("pseudo_element_styles", &self.pseudo_element_styles)
            .field(
                "counter_style_environment_identity",
                &self.counter_style_environment_identity,
            )
            .field("animation_overlay_identity", &self.animation_overlay_identity)
            .field("dependency_flags", &self.dependency_flags)
            .finish()
    }
}

fn pointer_value(pointer: *const c_void) -> Result<u64, std::num::TryFromIntError> {
    u64::try_from(pointer as usize)
}

fn read_cascade_origin(payload: &mut PayloadReader) -> Result<FfiCascadeOrigin, Box<dyn std::error::Error>> {
    Ok(match payload.read_u8()? {
        0 => FfiCascadeOrigin::Author,
        1 => FfiCascadeOrigin::AuthorPresentationalHint,
        2 => FfiCascadeOrigin::User,
        3 => FfiCascadeOrigin::UserAgent,
        value => return Err(format!("unknown cascade origin {value}").into()),
    })
}

fn read_declaration_kind(payload: &mut PayloadReader) -> Result<FfiElementDeclarationKind, Box<dyn std::error::Error>> {
    Ok(match payload.read_u8()? {
        0 => FfiElementDeclarationKind::InlineStyle,
        1 => FfiElementDeclarationKind::PresentationalHint,
        2 => FfiElementDeclarationKind::SvgPresentationAttribute,
        value => return Err(format!("unknown element declaration kind {value}").into()),
    })
}

unsafe fn standalone_alloc(size: usize, alignment: usize) -> *mut u8 {
    if !alignment.is_power_of_two() {
        return std::ptr::null_mut();
    }
    let metadata_size = 2 * size_of::<usize>();
    let Some(total_size) = size
        .max(1)
        .checked_add(alignment.saturating_sub(1))
        .and_then(|size| size.checked_add(metadata_size))
    else {
        return std::ptr::null_mut();
    };
    let Ok(layout) = Layout::from_size_align(total_size, align_of::<usize>()) else {
        return std::ptr::null_mut();
    };
    let base = unsafe { System.alloc(layout) };
    if base.is_null() {
        return base;
    }
    let unaligned = base as usize + metadata_size;
    let aligned = unaligned.next_multiple_of(alignment);
    let metadata = (aligned - metadata_size) as *mut usize;
    unsafe {
        *metadata = base as usize;
        *metadata.add(1) = total_size;
    }
    aligned as *mut u8
}

unsafe fn standalone_dealloc(pointer: *mut u8) {
    if pointer.is_null() {
        return;
    }
    let metadata = unsafe { pointer.cast::<usize>().sub(2) };
    let base = unsafe { *metadata } as *mut u8;
    let total_size = unsafe { *metadata.add(1) };
    let layout = Layout::from_size_align(total_size, align_of::<usize>()).expect("invalid retained allocation layout");
    unsafe { System.dealloc(base, layout) };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_rust_alloc(size: usize, alignment: usize) -> *mut u8 {
    unsafe { standalone_alloc(size, alignment) }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_rust_alloc_zeroed(size: usize, alignment: usize) -> *mut u8 {
    let pointer = unsafe { standalone_alloc(size, alignment) };
    if !pointer.is_null() {
        unsafe { pointer.write_bytes(0, size) };
    }
    pointer
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_rust_dealloc(pointer: *mut u8, _alignment: usize) {
    unsafe { standalone_dealloc(pointer) };
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_rust_realloc(
    pointer: *mut u8,
    old_size: usize,
    new_size: usize,
    alignment: usize,
) -> *mut u8 {
    let new_pointer = unsafe { standalone_alloc(new_size, alignment) };
    if !new_pointer.is_null() {
        unsafe { pointer.copy_to_nonoverlapping(new_pointer, old_size.min(new_size)) };
        unsafe { standalone_dealloc(pointer) };
    }
    new_pointer
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_string_unref(_raw: usize) {}
#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_fly_string_unref(_raw: usize) {}

#[unsafe(no_mangle)]
unsafe extern "C" fn unicode_rust_idna_to_ascii(
    domain: *const u8,
    domain_length: usize,
    _options: *const c_void,
    context: *mut c_void,
    on_success: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    // Replays only need the ASCII hosts serialized by the style recorder. The browser uses
    // LibUnicode's C++ implementation for the full IDNA algorithm.
    let domain = unsafe { std::slice::from_raw_parts(domain, domain_length) };
    if domain.is_ascii() {
        unsafe { on_success(context, domain.as_ptr(), domain.len()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn computation_inputs_preserve_payload_alignment_across_versions() {
        for version in 11..=16 {
            let mut payload = PayloadWriter::default();
            for value in [800.0_f64, 600.0, 16.0, 8.0, 12.0, 9.0, 20.0] {
                payload.write_u64(value.to_bits());
            }
            payload.write_bool(true);
            if version >= 12 {
                for value in [15.0_f64, 7.0, 11.0, 8.0] {
                    payload.write_u64(value.to_bits());
                }
            }
            payload.write_i32(960);
            payload.write_i32(1024);
            payload.write_u64(2.0_f64.to_bits());
            payload.write_u64(123);
            if version >= 12 {
                payload.write_u8(2);
                payload.write_bool(true);
                payload.write_u8(2);
                for code in [1, 2, 0, 0] {
                    payload.write_u8(code);
                }
            }
            if version >= 13 {
                payload.write_bool(true);
                payload.write_u64(456);
            }
            if version >= 15 {
                payload.write_bool(true);
            }
            payload.write_bytes(b"following transaction output");
            let registry = ReplayCustomPropertyRegistry::new();
            let mut reader = PayloadReader::new(payload.as_bytes());
            let inputs = read_document_style_computation_inputs(&mut reader, version, registry.pointer()).unwrap();
            assert_eq!(inputs.viewport_width, 800.0);
            assert_eq!(inputs.root_line_height, 20.0);
            assert!(inputs.root_font_metrics_depend_on_viewport_metrics);
            assert_eq!(inputs.initial_font_size, if version >= 12 { 15.0 } else { 0.0 });
            assert_eq!(inputs.initial_font_x_height, if version >= 12 { 7.0 } else { 0.0 });
            assert_eq!(inputs.initial_font_cap_height, if version >= 12 { 11.0 } else { 0.0 });
            assert_eq!(inputs.initial_font_zero_advance, if version >= 12 { 8.0 } else { 0.0 });
            assert_eq!(inputs.initial_font_size_raw, 960);
            assert_eq!(inputs.default_font_size_raw, 1024);
            assert_eq!(inputs.device_pixels_per_css_pixel, 2.0);
            assert_eq!(inputs.font_environment_generation, 123);
            assert_eq!(inputs.preferred_color_scheme, if version >= 12 { 2 } else { 0 });
            assert_eq!(inputs.has_document_supported_schemes, version >= 12);
            assert_eq!(
                inputs.document_supported_scheme_count,
                if version >= 12 { 2 } else { 0 }
            );
            assert_eq!(
                inputs.document_supported_scheme_codes,
                if version >= 12 { [1, 2, 0, 0] } else { [0; 4] }
            );
            assert_eq!(
                inputs.custom_property_registry,
                if version >= 13 {
                    registry.pointer()
                } else {
                    std::ptr::null()
                }
            );
            assert_eq!(
                inputs.custom_property_registration_generation,
                if version >= 13 { 456 } else { 0 }
            );
            assert_eq!(inputs.in_quirks_mode, version >= 15);
            assert_eq!(reader.read_bytes().unwrap(), b"following transaction output");
        }
    }

    #[test]
    fn presence_degraded_publications_compare_every_non_mask_field() {
        let expected = FfiExactCascadePublication {
            computed_group_mask: 1,
            unchanged: false,
            donor_used: false,
        };
        let mut actual = expected;
        actual.computed_group_mask = 8;
        assert!(exact_cascade_publications_match(expected, actual, false));
        actual.unchanged = true;
        assert!(!exact_cascade_publications_match(expected, actual, false));
    }

    #[test]
    fn mismatched_verification_gates_are_rejected() {
        assert!(require_matching_verification_gates(0b0101, 0b0101).is_ok());
        assert_eq!(
            require_matching_verification_gates(0b0101, 0b0001).unwrap_err(),
            "verification gates diverged: recording used 0x05, replay uses 0x01"
        );
    }

    #[test]
    fn memory_statistics_accumulate_categories_and_derive_tiers() {
        let mut total = FfiMemoryPressureSnapshot::default();
        let mut first = FfiMemoryPressureSnapshot::default();
        first.category_bytes[MemoryCategory::StyleNodeMapping as usize] = 10;
        first.category_bytes[MemoryCategory::FeaturePosting as usize] = 20;
        let mut second = FfiMemoryPressureSnapshot::default();
        second.category_bytes[MemoryCategory::StyleNodeMapping as usize] = 30;
        second.category_bytes[MemoryCategory::BatchScratch as usize] = 40;

        accumulate_memory_pressure(&mut total, first);
        accumulate_memory_pressure(&mut total, second);

        assert_eq!(total.category_bytes[MemoryCategory::StyleNodeMapping as usize], 40);
        assert_eq!(memory_tier_totals(&total), [0, 40, 0, 20, 40]);
    }

    #[test]
    fn json_output_option_is_parsed() {
        let options = Options::parse_from(
            [
                "style-replay",
                "--assert-digests",
                "--detailed-counters",
                "--json-output",
                "results.json",
                "capture.sg",
            ]
            .map(std::ffi::OsString::from),
        )
        .unwrap();

        assert!(options.assert_digests);
        assert!(options.detailed_counters);
        assert_eq!(options.json_output, Some(PathBuf::from("results.json")));
        assert_eq!(options.paths, [PathBuf::from("capture.sg")]);
        assert!(matches!(options.selection, Selection::Full));
    }

    #[test]
    fn json_reports_raw_memory_and_amplification_counters() {
        let mut memory = FfiMemoryPressureSnapshot {
            tier3_limit: 100,
            tier4_limit: 200,
            tier3_bytes: 30,
            tier4_bytes: 40,
            tier3_refusals: 5,
            tier4_refusals: 6,
            tier3_evictions: 7,
            ..Default::default()
        };
        memory.category_bytes[MemoryCategory::FeaturePosting as usize] = 50;
        memory.tier3_refusal_categories[TIER3_REFUSAL_CATEGORIES
            .iter()
            .position(|&category| category == MemoryCategory::FeaturePosting)
            .unwrap()] = 8;
        memory.tier3_refusal_categories[TIER3_REFUSAL_CATEGORIES
            .iter()
            .position(|&category| category == MemoryCategory::ParsedSubstitutionCache)
            .unwrap()] = 9;
        let report = memory_report(2, &memory);
        assert_eq!(report["engine_count"], 2);
        assert_eq!(report["categories_bytes"]["featurePosting"], 50);
        assert_eq!(report["pressure"]["tier3_refusals_by_category"]["featurePosting"], 8);
        assert_eq!(
            report["pressure"]["tier3_refusals_by_category"]["parsedSubstitutionCache"],
            9
        );

        let amplification = AmplificationLedger {
            flushes: 2,
            selector_changed: 4,
            selector_touched: 10,
            ..Default::default()
        }
        .report();
        assert_eq!(amplification["stages"]["selector"]["changed_rows"], 4);
        assert_eq!(amplification["stages"]["selector"]["touched_rows"], 10);
        assert_eq!(amplification["stages"]["selector"]["amplification"], 2.5);
    }

    #[test]
    fn style_transaction_outputs_round_trip_a_sweep_without_reclaims() {
        let expected = StyleTransactionOutputs {
            result: true,
            filter_calls: Vec::new(),
            emissions: Vec::new(),
            style_atoms_swept: true,
            reclaimed_atoms: Vec::new(),
        };
        let mut payload = PayloadWriter::default();
        write_style_transaction_outputs(&expected, &mut payload);

        let actual = read_style_transaction_outputs(PayloadReader::new(payload.as_bytes())).unwrap();

        assert!(actual.result);
        assert!(actual.filter_calls.is_empty());
        assert!(actual.emissions.is_empty());
        assert!(actual.style_atoms_swept);
        assert_eq!(actual.reclaimed_atoms, expected.reclaimed_atoms);
    }

    #[test]
    fn style_record_replay_indices_ignore_base_generations() {
        let identity = 17;
        let first_generation = identity;
        let later_generation = (29_u64 << 32) | identity;
        let overlay = (1_u64 << 63) | identity;

        assert_eq!(style_record_replay_index(first_generation).unwrap(), 34);
        assert_eq!(style_record_replay_index(later_generation).unwrap(), 34);
        assert_eq!(style_record_replay_index(overlay).unwrap(), 35);
    }
}

/// A read-only memory mapping of one capture file.
///
/// Replay decodes tens of millions of small events; mapping the whole log once keeps that loop
/// free of per-event read syscalls and buffer management.
mod mapped_log {
    use std::ffi::c_int;
    use std::ffi::c_void;
    use std::fs::File;
    use std::io;
    use std::os::fd::AsRawFd;
    use std::path::Path;
    use std::ptr;
    use std::slice;

    const PROT_READ: c_int = 0x1;
    const MAP_PRIVATE: c_int = 0x2;
    const MADV_SEQUENTIAL: c_int = 2;

    unsafe extern "C" {
        fn mmap(
            address: *mut c_void,
            length: usize,
            protection: c_int,
            flags: c_int,
            fd: c_int,
            offset: i64,
        ) -> *mut c_void;
        fn munmap(address: *mut c_void, length: usize) -> c_int;
        fn madvise(address: *mut c_void, length: usize, advice: c_int) -> c_int;
    }

    pub struct MappedLog {
        address: *mut c_void,
        length: usize,
    }

    impl MappedLog {
        pub fn open(path: &Path) -> io::Result<Self> {
            let file = File::open(path)?;
            let length = usize::try_from(file.metadata()?.len()).expect("capture file exceeds the address space");
            if length == 0 {
                return Ok(Self {
                    address: ptr::null_mut(),
                    length: 0,
                });
            }
            // SAFETY: mapping a readable file we just opened, at the length its metadata reported.
            let address = unsafe { mmap(ptr::null_mut(), length, PROT_READ, MAP_PRIVATE, file.as_raw_fd(), 0) };
            if address as usize == usize::MAX {
                return Err(io::Error::last_os_error());
            }
            // SAFETY: advising over the mapping established above; a failure only costs readahead.
            unsafe { madvise(address, length, MADV_SEQUENTIAL) };
            Ok(Self { address, length })
        }

        pub fn bytes(&self) -> &[u8] {
            if self.length == 0 {
                return &[];
            }
            // SAFETY: the mapping is readable, never written, and lives until this value is dropped.
            unsafe { slice::from_raw_parts(self.address.cast(), self.length) }
        }
    }

    impl Drop for MappedLog {
        fn drop(&mut self) {
            if self.length != 0 {
                // SAFETY: unmapping exactly the region mapped in open().
                unsafe { munmap(self.address, self.length) };
            }
        }
    }
}
