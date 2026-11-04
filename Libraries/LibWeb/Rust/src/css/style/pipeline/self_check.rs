/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The check of what the engine holds against a cold reference pass over the same document:
//! one thread, every cache off, on a fork of the engine, so that nothing the reference pass does
//! reaches the document's engine. With `LIBWEB_STYLE_SELFCHECK` set, the document is checked so
//! at the end of each style update that installed styles; `report` prints what differs, any other
//! value but `0` aborts on it. `threads` (or `threads-report`) also checks it against cold passes
//! with their caches on, on one thread and on several: each thread keeps caches and style sharing
//! candidates of its own, so what a pass reuses depends on how its elements spread over threads.
//! The unit tests check the engine with the same comparison.

use super::super::*;
use super::records::{Record, RecordCells, Records, RetainedRecords};
use super::rules::RuleIndex;
use super::traversal;
#[cfg(test)]
use super::write_once::WriteOnce;
use crate::css::custom_properties::CustomPropertyStore;
use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_name};
use crate::css::style_value::{RetainedStyleValueData, retained_style_values_equal};
use std::sync::Arc;
use std::time::Instant;

/// What `LIBWEB_STYLE_SELFCHECK` asks for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Mode {
    Off,
    /// Print what differs, and abort.
    Abort,
    /// Print what differs only.
    Report,
}

/// What `LIBWEB_STYLE_SELFCHECK` asks for, read once per document host.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in super::super) struct SelfCheckSettings {
    mode: Mode,
    /// Whether it asks for the check against cold passes on one thread and on several.
    checks_thread_counts: bool,
}

impl SelfCheckSettings {
    pub(in super::super) fn from_environment() -> Self {
        let (mode, checks_thread_counts) = match std::env::var("LIBWEB_STYLE_SELFCHECK").as_deref() {
            Err(_) | Ok("" | "0") => (Mode::Off, false),
            Ok("report") => (Mode::Report, false),
            Ok("threads") => (Mode::Abort, true),
            Ok("threads-report") => (Mode::Report, true),
            Ok(_) => (Mode::Abort, false),
        };
        Self {
            mode,
            checks_thread_counts,
        }
    }

    pub(in super::super) fn is_enabled(self) -> bool {
        self.mode != Mode::Off
    }
}

/// How many threads the self-check's cold passes on several threads run on.
fn reference_thread_count() -> u32 {
    crate::style_layout_pool::default_thread_count().max(4) as u32
}

/// The names of the groups, by `group_index`.
const GROUP_NAMES: [&str; crate::css::table_group_builder::group_index::COUNT] = [
    "inherited list and table",
    "inherited UI",
    "inherited SVG",
    "inherited text",
    "font",
    "animation",
    "SVG reset",
    "grid",
    "anchor",
    "content",
    "alignment",
    "misc layout",
    "misc interaction",
    "effects",
    "mask",
    "text reset",
    "transform",
    "background",
    "border",
    "sizing",
    "surround",
    "box",
];

/// The element, as its tag, ID and classes, with `name_of` naming their atoms.
pub(super) fn describe(state: &RetainedState, node: StyleNodeID, name_of: &dyn Fn(StyleAtomID) -> String) -> String {
    let Some(facts) = state.projection.facts(node) else {
        return format!("node {}", node.raw());
    };
    let mut description = format!("<{}", name_of(facts.tag()));
    let id = facts.id();
    if !id.is_none() {
        description += &format!(" id={}", name_of(id));
    }
    let classes: Vec<_> = facts.classes().iter().map(|&class| name_of(class)).collect();
    if !classes.is_empty() {
        description += &format!(" class=\"{}\"", classes.join(" "));
    }
    description + &format!("> (node {})", node.raw())
}

pub(super) fn pseudo_element_name(kind: u8) -> String {
    use super::super::publication::pseudo_kind::{
        AFTER, BACKDROP, BEFORE, FIRST_LETTER, MARKER, SEARCH_TEXT, SEARCH_TEXT_CURRENT, SELECTION, SPELLING_ERROR,
    };
    match kind {
        BEFORE => "::before".to_owned(),
        AFTER => "::after".to_owned(),
        FIRST_LETTER => "::first-letter".to_owned(),
        SELECTION => "::selection".to_owned(),
        SEARCH_TEXT => "::search-text".to_owned(),
        SEARCH_TEXT_CURRENT => "::search-text:current".to_owned(),
        SPELLING_ERROR => "::spelling-error".to_owned(),
        BACKDROP => "::backdrop".to_owned(),
        MARKER => "::marker".to_owned(),
        kind => format!("pseudo-element {kind}"),
    }
}

fn payloads_equal(
    group: usize,
    first: &crate::css::computed_values::GroupPointer,
    second: &crate::css::computed_values::GroupPointer,
) -> bool {
    let (first, second) = (first.as_ptr(), second.as_ptr());
    // SAFETY: Both are live payloads of the group.
    first == second || unsafe { crate::css::computed_values::style_group_payloads_equal(group, first, second) }
}

fn serialized(value: Option<&RetainedStyleValueData>) -> String {
    value.map_or_else(
        || "(none)".to_owned(),
        |value| String::from_utf16_lossy(&crate::css::serialize::serialize_style_value_to_utf16(value.data())),
    )
}

/// The values of the custom properties whose names resolve to different values in two stores,
/// whichever stores the values are in, as the first's and the second's.
fn custom_property_differences(
    first: Option<&Arc<CustomPropertyStore>>,
    second: Option<&Arc<CustomPropertyStore>>,
) -> Vec<(String, String)> {
    fn append_names(store: &CustomPropertyStore, names: &mut Vec<usize>) {
        names.extend(store.own_values.keys());
        if let Some(parent) = store.parent() {
            append_names(parent, names);
        }
    }
    let (first, second) = (first.map(Arc::as_ref), second.map(Arc::as_ref));
    let mut all = Vec::new();
    for store in [first, second].into_iter().flatten() {
        append_names(store, &mut all);
    }
    all.sort_unstable();
    all.dedup();
    let describe = |entry: Option<&crate::css::custom_properties::CustomPropertyEntry>| {
        entry.map_or_else(
            || "(none)".to_owned(),
            |entry| match entry.important {
                true => format!("{} !important", serialized(Some(&entry.value))),
                false => serialized(Some(&entry.value)),
            },
        )
    };
    all.into_iter()
        .filter_map(|name| {
            let (first, second) = (
                first.and_then(|store| store.get(name)),
                second.and_then(|store| store.get(name)),
            );
            let equal = match (first, second) {
                (None, None) => true,
                (Some(first), Some(second)) => {
                    first.important == second.important && retained_style_values_equal(&first.value, &second.value)
                }
                _ => false,
            };
            (!equal).then(|| (describe(first), describe(second)))
        })
        .collect()
}

/// The parts `live` differs from `cold` in, compared by value: the payloads of the groups, the
/// longhands, what their groups are built from and their flags as published, the custom properties by the values their names resolve to, and
/// what the record notes of how it was computed, each with the values that differ where they
/// have a text.
pub(super) fn record_differences(live: &Record, cold: &Record) -> Vec<String> {
    let mut differences = Vec::new();
    let groups: Vec<_> = (0..crate::css::table_group_builder::group_index::COUNT)
        .filter(|&group| !payloads_equal(group, &live.payloads[group], &cold.payloads[group]))
        .map(|group| GROUP_NAMES[group])
        .collect();
    if !groups.is_empty() {
        differences.push(format!("the groups {}", groups.join(", ")));
    }
    let (live_longhands, cold_longhands) = (
        computed::ViewLonghands::of_record(live),
        computed::ViewLonghands::of_record(cold),
    );
    let longhands: Vec<_> = (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID)
        .filter_map(|property| {
            let (ours, theirs) = (live_longhands.handle(property), cold_longhands.handle(property));
            let equal = match (ours, theirs) {
                (None, None) => true,
                (Some(ours), Some(theirs)) => retained_style_values_equal(ours, theirs),
                _ => false,
            };
            (!equal).then(|| {
                format!(
                    "{}: live {}, cold {}",
                    property_name(property),
                    serialized(ours),
                    serialized(theirs)
                )
            })
        })
        .collect();
    if !longhands.is_empty() {
        differences.push(format!("the longhands {{{}}}", longhands.join("; ")));
    } else if !live.rare().value_facts.publication_equals(&cold.rare().value_facts) {
        differences.push(format!(
            "the groups, in {}",
            live.rare()
                .value_facts
                .difference(&cold.rare().value_facts)
                .unwrap_or_else(|| "what they are built from beside their values".into())
        ));
    }
    let flags = live
        .rare()
        .table_flags
        .publication_differences(&cold.rare().table_flags);
    if !flags.is_empty() {
        differences.push(format!("the longhand table flags {{{}}}", flags.join("; ")));
    }
    if live.facts() != cold.facts() {
        differences.push("the record facts".into());
    }
    if live.payloads.current_color() != cold.payloads.current_color() {
        differences.push(format!(
            "current color {:#010x} != {:#010x}",
            live.payloads.current_color(),
            cold.payloads.current_color()
        ));
    }
    for (name, live, cold) in [
        ("custom properties", live.custom_properties(), cold.custom_properties()),
        (
            "inheritable custom properties",
            live.inheritable_custom_properties(),
            cold.inheritable_custom_properties(),
        ),
    ] {
        let values = custom_property_differences(live, cold);
        if !values.is_empty() {
            let values: Vec<_> = values
                .iter()
                .map(|(live, cold)| format!("live {live}, cold {cold}"))
                .collect();
            differences.push(format!("{name} {{{}}}", values.join("; ")));
        }
    }
    // NB: A cascade decides one declaration per name, so the order of the list says nothing.
    let custom_declarations_equal = match (&live.custom_declarations(), &cold.custom_declarations()) {
        (None, None) => true,
        (Some(first), Some(second)) => {
            let same_declaration =
                |first: &(CustomDeclaration, RetainedStyleValueData),
                 second: &(CustomDeclaration, RetainedStyleValueData)| {
                    first.0 == second.0 && retained_style_values_equal(&first.1, &second.1)
                };
            first.len() == second.len()
                && (first
                    .iter()
                    .zip(second.iter())
                    .all(|(first, second)| same_declaration(first, second))
                    || first
                        .iter()
                        .all(|declaration| second.iter().any(|other| same_declaration(declaration, other))))
        }
        _ => false,
    };
    if !custom_declarations_equal {
        differences.push("custom declarations".to_owned());
    }
    if live.rare().custom_property_reads != cold.rare().custom_property_reads {
        differences.push("the custom properties the longhands read".to_owned());
    }
    if live.rare().reads.parent_reset_groups != cold.rare().reads.parent_reset_groups {
        differences.push(format!(
            "parent_reset_groups {:#x} != {:#x}",
            live.rare().reads.parent_reset_groups,
            cold.rare().reads.parent_reset_groups
        ));
    }
    for (name, live, cold) in [
        (
            "reads_sibling_position",
            live.rare().reads.reads_sibling_position,
            cold.rare().reads.reads_sibling_position,
        ),
        (
            "reads_attributes",
            live.rare().reads.reads_attributes,
            cold.rare().reads.reads_attributes,
        ),
    ] {
        if live != cold {
            differences.push(format!("{name} {live} != {cold}"));
        }
    }
    if live.rare().reads.container_units != cold.rare().reads.container_units {
        differences.push(format!(
            "container units {:#x} != {:#x}",
            live.rare().reads.container_units,
            cold.rare().reads.container_units
        ));
    }
    if live.rare().counter_style_environment_identity != cold.rare().counter_style_environment_identity {
        differences.push("counter style environment".to_owned());
    }
    if live.rare().animation_name_scope != cold.rare().animation_name_scope {
        differences.push("animation name scope".to_owned());
    }
    differences
}

/// What the records `live` holds for the document under `root`, of each element and of its
/// pseudo-elements, differ in from those `cold` holds, one line per element or pseudo-element,
/// with `describe` naming the elements, and how many elements it compared.
pub(super) fn differences(
    state: &RetainedState,
    root: StyleNodeID,
    live: &RetainedRecords,
    cold: &RetainedRecords,
    describe: &dyn Fn(StyleNodeID) -> String,
) -> (Vec<String>, usize) {
    let (failures, compared) = differences_by_element(state, root, live, cold);
    let failures = failures
        .into_iter()
        .map(|(node, difference)| match difference.strip_prefix("::") {
            Some(of_pseudo_element) => {
                let (name, rest) = of_pseudo_element.split_once(": ").unwrap_or((of_pseudo_element, ""));
                format!("::{name} of {}: {rest}", describe(node))
            }
            None => format!("{}: {difference}", describe(node)),
        })
        .collect();
    (failures, compared)
}

/// What differs between `live` and `cold`, one line per element or pseudo-element beginning with
/// the name of the pseudo-element where it is one, with the element it is about.
pub(super) fn differences_by_element(
    state: &RetainedState,
    root: StyleNodeID,
    live: &RetainedRecords,
    cold: &RetainedRecords,
) -> (Vec<(StyleNodeID, String)>, usize) {
    let mut failures = Vec::new();
    let mut compared = 0;
    // NB: The engine keeps the records of the elements below one that stopped being displayed,
    //     which no pass keeps current and the host installs none of.
    let hidden_in_cold = |node| {
        std::iter::successors(state.projection.inheritance_parent(node), |&ancestor| {
            state.projection.inheritance_parent(ancestor)
        })
        .any(|ancestor| cold.get(ancestor).is_none_or(|record| !record.is_displayed()))
    };
    for node in state.projection.style_preorder(root) {
        match (live.get(node), cold.get(node)) {
            (None, None) => continue,
            (Some(_), None) if hidden_in_cold(node) => continue,
            (Some(_), None) => failures.push((node, "live holds a record, cold computed none".to_owned())),
            (None, Some(_)) => failures.push((node, "live holds no record, cold computed one".to_owned())),
            // NB: No pass samples the animations of an element that is not displayed. A cold pass reads what a read
            //     below it last sampled, which the record the passes keep does not, and a read samples them again.
            (Some(live), Some(_)) if !live.is_displayed() && state.element_samples_custom_properties(node) => continue,
            (Some(live), Some(cold)) => {
                let differences = record_differences(live, cold);
                if !differences.is_empty() {
                    failures.push((node, format!("live differs from cold in {}", differences.join(", "))));
                }
            }
        }
        compared += 1;
        let (live, cold) = (live.pseudo_element_records(node), cold.pseudo_element_records(node));
        let kinds = |records: &[(u8, Arc<Record>)]| records.iter().map(|(kind, _)| *kind).collect::<Vec<_>>();
        if kinds(live) != kinds(cold) {
            let names = |kinds: Vec<u8>| kinds.into_iter().map(pseudo_element_name).collect::<Vec<_>>();
            failures.push((
                node,
                format!(
                    "live holds the pseudo-elements {:?}, cold computed {:?}",
                    names(kinds(live)),
                    names(kinds(cold))
                ),
            ));
            continue;
        }
        for ((kind, live), (_, cold)) in live.iter().zip(cold) {
            let differences = record_differences(live, cold);
            if !differences.is_empty() {
                failures.push((
                    node,
                    format!(
                        "{}: live differs from cold in {}",
                        pseudo_element_name(*kind),
                        differences.join(", ")
                    ),
                ));
            }
        }
    }
    (failures, compared)
}

/// What the records installed for the elements under `root` and their pseudo-elements, which the host renders, differ
/// in from those `live` holds for them, one line per element or pseudo-element, with `describe` naming the elements.
/// An element's animations compose over its installed record, which is what is compared.
pub(super) fn installed_differences(
    state: &RetainedState,
    root: StyleNodeID,
    live: &RetainedRecords,
    describe: &dyn Fn(StyleNodeID) -> String,
) -> Vec<String> {
    let installed = &state.computed_group_sets;
    let mut failures = Vec::new();
    for node in state.projection.style_preorder(root) {
        // NB: The engine keeps the records of the elements below one that stopped being displayed,
        //     which no pass keeps current and the host installs none of. What the host holds for an
        //     element that is itself not displayed is what a read below it last sampled, and a read
        //     of it demands its style again.
        let hidden = std::iter::successors(Some(node), |&ancestor| state.projection.inheritance_parent(ancestor))
            .any(|ancestor| live.get(ancestor).is_none_or(|record| !record.is_displayed()));
        if hidden {
            continue;
        }
        if let Some(held) = live.get(node)
            && let Some(record) = installed
                .assigned_style_record(node)
                .and_then(|record| installed.installed_base_record(record))
        {
            let differences = record_differences(record, held);
            if !differences.is_empty() {
                failures.push(format!(
                    "{}: the installed record differs from live in {}",
                    describe(node),
                    differences.join(", ")
                ));
            }
        }
        for (kind, held) in live.pseudo_element_records(node) {
            let Some(record) = installed
                .pseudo_style_record(node, *kind)
                .and_then(|record| installed.installed_base_record(record))
            else {
                continue;
            };
            let differences = record_differences(record, held);
            if !differences.is_empty() {
                failures.push(format!(
                    "{} of {}: the installed record differs from live in {}",
                    pseudo_element_name(*kind),
                    describe(node),
                    differences.join(", ")
                ));
            }
        }
    }
    failures
}

/// What a reference pass computed and measured.
pub(super) struct ReferencePass {
    pub(super) records: RetainedRecords,
    /// What its last round measured.
    pub(super) statistics: traversal::Statistics,
    pub(super) indexed_entries: u64,
    pub(super) index_nanoseconds: u64,
    pub(super) style_nanoseconds: u64,
    pub(super) threads: u64,
    /// What its last round produced for each element.
    #[cfg(test)]
    pub(super) outputs: Vec<WriteOnce<traversal::ElementOutput>>,
}

/// Style the document under `engine`'s `root` from scratch on `threads` threads, against a rule
/// index of its own, and with every cache off unless `uses_caches`, and keep what it computed, or
/// none where it computes no initial style or waits on fonts or random base values for good. It
/// resolves the fonts and draws the random base values it waits on in `engine`.
pub(super) fn reference_pass(
    engine: &mut StyleEngine,
    root: StyleNodeID,
    uses_caches: bool,
    threads: u32,
) -> Option<ReferencePass> {
    let initial = engine.computed_initial(root)?;
    // NB: A pass on one thread leaves the engine's pool as it is.
    let pool = (threads > 1).then(|| engine.style_layout_pool(threads)).flatten();
    let started = Instant::now();
    let index = RuleIndex::build(&engine.retained.rules, engine.retained.process_rule_indexes());
    let index_nanoseconds = started.elapsed().as_nanos() as u64;
    engine.retained.live_counters.note_cold_pass_index(&index);
    // A drive that waits on a font declines its element; the pass styles every element again once
    // the resolver answered.
    engine.until_resolved(root, |engine| {
        let state: &RetainedState = &engine.retained;
        let slot_count = state.projection.element_slot_count();
        let mut cells = RecordCells::default();
        cells.reserve(slot_count);
        #[cfg(test)]
        let outputs: Vec<WriteOnce<traversal::ElementOutput>> = (0..slot_count).map(|_| WriteOnce::default()).collect();
        let started = Instant::now();
        let statistics = {
            let records = Records::new(state, &cells, None)
                .composing_over_equal_records(|first, second| record_differences(first, second).is_empty());
            let context = traversal::PassContext {
                #[cfg(test)]
                outputs: Some(&outputs),
                uses_caches,
                cache_audit: state.cache_audit_mode(),
                ..traversal::PassContext::new(state, &index, &initial, &records, None)
            };
            traversal::style_document(&context, pool.as_deref(), root)
        };
        let style_nanoseconds = started.elapsed().as_nanos() as u64;
        if statistics.font_requests.is_empty() && statistics.random_requests.is_empty() {
            let styled: Vec<_> = state.projection.style_preorder(root).collect();
            let mut records = RetainedRecords::default();
            records.absorb(&mut cells, &styled, None, &mut Default::default());
            return Ok(ReferencePass {
                records,
                statistics,
                indexed_entries: index.entries.count() as u64,
                index_nanoseconds,
                style_nanoseconds,
                threads: pool.as_deref().map_or(0, rayon::ThreadPool::current_num_threads) as u64 + 1,
                #[cfg(test)]
                outputs,
            });
        }
        Err(super::Waits {
            fonts: statistics.font_requests,
            random_bases: statistics.random_requests,
        })
    })
}

impl StyleEngine {
    /// What the records the engine holds for the document under `root` differ in from those of
    /// a reference pass over a fork of the engine, one line per element or pseudo-element, or none
    /// where the engine does not own the document or has restyling left to do. Where
    /// `other_threads` names a number of threads, they are also checked against cold passes with
    /// their caches on, on one thread and on that many, with their lines naming the pass.
    pub(super) fn self_check_failures(&self, root: StyleNodeID, other_threads: Option<u32>) -> Vec<String> {
        let Some(live) = self.retained.live_style.as_deref() else {
            return Vec::new();
        };
        // NB: A pass leaving children for the next one, to inherit the composition of their
        //     parents' animations, leaves their records as they were until then.
        if live.has_pending() {
            return Vec::new();
        }
        let mut fork = self.clone();
        fork.detach_host_flags_for_fork();
        fork.detach_tree_depth_for_fork();
        let name_of = |atom: StyleAtomID| self.atoms.name_of(atom).unwrap_or_else(|| format!("atom{}", atom.0));
        let describe = |node| describe(&self.retained, node, &name_of);
        let mut references = vec![(None, false, 1)];
        if let Some(threads) = other_threads {
            references.push((Some("a cold pass on 1 thread with caches".to_owned()), true, 1));
            references.push((
                Some(format!("a cold pass on {threads} threads with caches")),
                true,
                threads,
            ));
        }
        let mut failures = Vec::new();
        for (name, uses_caches, threads) in references {
            let Some(ReferencePass { records: reference, .. }) = reference_pass(&mut fork, root, uses_caches, threads)
            else {
                failures.push(format!(
                    "the reference pass computes no style for the document{}",
                    name.as_ref().map_or_else(String::new, |name| format!(" on {name}"))
                ));
                continue;
            };
            let (differences, _) = differences(&self.retained, root, &live.records, &reference, &describe);
            failures.extend(differences.into_iter().map(|difference| match &name {
                Some(name) => format!("against {name}: {difference}"),
                None => difference,
            }));
        }
        failures.extend(installed_differences(&self.retained, root, &live.records, &describe));
        failures.extend(live.kept_hidden_differences(&self.retained, &describe));
        failures
    }

    /// Check the records the engine holds for the document under `root` against a reference
    /// pass, as `settings` ask.
    pub(in super::super) fn run_self_check(&self, root: StyleNodeID, settings: SelfCheckSettings) {
        if !settings.is_enabled() {
            return;
        }
        let other_threads = settings.checks_thread_counts.then(reference_thread_count);
        conclude(&self.self_check_failures(root, other_threads), settings.mode);
    }
}

/// Print `failures` to stderr, and abort where `mode` asks to.
pub(super) fn conclude(failures: &[String], mode: Mode) {
    if failures.is_empty() {
        return;
    }
    eprintln!(
        "Style self-check: the engine differs from a cold pass in {} places:",
        failures.len()
    );
    for failure in failures {
        eprintln!("    {failure}");
    }
    assert!(
        mode != Mode::Abort,
        "the engine differs from a cold pass; set LIBWEB_STYLE_SELFCHECK=report to only print the differences"
    );
}

#[cfg(test)]
mod tests {
    use super::super::test_support::*;
    use super::*;

    fn document_with_a_target() -> (StyleEngine, Vec<StyleNodeID>) {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".target { color: rgb(0, 128, 0); width: 10px; } .target::before { content: \"x\"; }",
        );
        add_class(&mut engine, nodes[2], "target");
        start_live(&mut engine, nodes[0]);
        (engine, nodes)
    }

    #[test]
    fn live_style_holds_what_a_reference_pass_computes() {
        let (mut engine, nodes) = document_with_a_target();
        assert_eq!(engine.self_check_failures(nodes[0], None), Vec::<String>::new());

        add_class(&mut engine, nodes[1], "target");
        remove_class(&mut engine, nodes[2], "target");
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(engine.self_check_failures(nodes[0], None), Vec::<String>::new());
    }

    #[test]
    fn live_style_holds_what_cold_passes_on_one_thread_and_on_four_compute() {
        let (engine, nodes) = document_with_a_target();
        assert_eq!(engine.self_check_failures(nodes[0], Some(4)), Vec::<String>::new());
    }

    #[test]
    fn a_record_live_style_holds_wrongly_is_reported() {
        let (mut engine, nodes) = document_with_a_target();
        // Seed a mismatch: the element no rule styles holds the record of the one the rule styles.
        let seeded = live_record(&engine, nodes[2]);
        let mut live = engine.retained.live_style.take().expect("the engine is live");
        let mut cells = RecordCells::default();
        cells.reserve(engine.retained.projection.element_slot_count());
        Records::new(&engine.retained, &cells, None).insert(nodes[1], seeded, nodes[1], None);
        live.records
            .absorb(&mut cells, &[nodes[1]], None, &mut Default::default());
        engine.retained.live_style = Some(live);

        let failures = engine.self_check_failures(nodes[0], Some(4));
        assert_eq!(failures.len(), 4, "{failures:?}");
        assert!(
            failures[1].starts_with("against a cold pass on 1 thread with caches: "),
            "{failures:?}"
        );
        assert!(
            failures[2].starts_with("against a cold pass on 4 threads with caches: "),
            "{failures:?}"
        );
        // NB: What it installed is what a pass computed before the seed, which the seeded record differs from too.
        assert!(
            failures[3].contains(&format!(
                "(node {}): the installed record differs from live in ",
                nodes[1].raw()
            )),
            "{failures:?}"
        );
        let failure = &failures[0];
        // NB: The atoms of a test's engine name no strings the self-check could show.
        assert!(
            failure.contains(&format!("(node {}): live differs from cold in ", nodes[1].raw())),
            "{failure}"
        );
        for part in [
            "the groups ",
            "inherited text",
            "color: live ",
            "width: live 10px, cold auto",
        ] {
            assert!(failure.contains(part), "{failure} names {part}");
        }
    }

    #[test]
    fn a_record_installed_for_another_element_is_reported() {
        let (mut engine, nodes) = document_with_a_target();
        // Seed a mismatch: the engine holds the right records, and the element no rule styles is
        // assigned the record installed for the one the rule styles.
        let sets = &mut engine.retained.computed_group_sets;
        let installed = sets.assigned_style_record(nodes[2]).expect("the element is installed");
        sets.assign_installed_record(nodes[1], installed);

        let failures = engine.self_check_failures(nodes[0], None);
        assert_eq!(failures.len(), 1, "{failures:?}");
        assert!(
            failures[0].contains(&format!(
                "(node {}): the installed record differs from live in ",
                nodes[1].raw()
            )),
            "{failures:?}"
        );
    }

    #[test]
    fn a_report_only_prints_the_differences() {
        conclude(&["a difference".to_owned()], Mode::Report);
        conclude(&[], Mode::Abort);
    }

    #[test]
    #[should_panic(expected = "the engine differs from a cold pass")]
    fn a_difference_aborts() {
        conclude(&["a difference".to_owned()], Mode::Abort);
    }
}
