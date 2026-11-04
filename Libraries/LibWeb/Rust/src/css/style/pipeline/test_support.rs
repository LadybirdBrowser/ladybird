/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the unit tests of the engine build on: a host stand-in that lets an engine compute records,
//! documents and rules built through the inputs the host records, and a check of what live style
//! the engine holds against a cold pass over the same document, and of what that pass matched against the
//! reference matcher.

use super::super::bridge::{
    BoundScopeChain, FfiDocumentStyleComputationInputs, FfiFontResolutionRequest, FfiHostHandle, FfiResolvedFont,
    element_adjustment_fact, publish_rule_declarations, publish_style_rule,
};
use super::super::font_resolution::{FontResolverHost, PublishedFontFaces};
use super::super::*;
use super::records::Record;
use super::write_once::WriteOnce;
use super::{self_check, traversal};
use std::cell::RefCell;
use std::sync::Arc;

/// Every request resolves to one font with these metrics, which nothing follows the handles of.
unsafe extern "C" fn resolve_font(
    _memo: *const std::ffi::c_void,
    _snapshot: *const std::ffi::c_void,
    _request: FfiFontResolutionRequest,
) -> FfiResolvedFont {
    FfiResolvedFont {
        first_available_font: FfiHostHandle::from_pointer(std::ptr::dangling()),
        font_cascade_list: FfiHostHandle::from_pointer(std::ptr::dangling()),
        ascent: 12.0,
        descent: 4.0,
        x_height: 8.0,
        zero_advance: 8.0,
        font_size_adjust_from_font: 0.0,
    }
}

/// An engine hosted as a document's is, as far as computing records goes: it has a font resolver
/// and a font resolution cache, and the document's computation inputs, those of an 800x600
/// viewport with a 16px font.
pub(super) fn hosted_engine() -> StyleEngine {
    let mut engine = StyleEngine::new();
    engine.share_the_process_style_for_test();
    // SAFETY: The test build's host object references and unreferences do nothing.
    let font_faces = unsafe { PublishedFontFaces::adopt(std::ptr::dangling(), std::ptr::dangling()) };
    engine.host.font_resolver = Some(FontResolverHost::with_callbacks(resolve_font, resolve_font, font_faces));
    engine.retained.font_resolution = Some(Default::default());
    // NB: CSS pixels count in 64ths of a pixel raw.
    engine.retained.document_style_computation_inputs = FfiDocumentStyleComputationInputs {
        viewport_width: 800.0,
        viewport_height: 600.0,
        root_font_size: 16.0,
        root_font_x_height: 8.0,
        root_font_cap_height: 11.0,
        root_font_zero_advance: 8.0,
        root_line_height: 18.0,
        root_effective_zoom: 1.0,
        initial_font_size: 16.0,
        initial_font_x_height: 8.0,
        initial_font_cap_height: 11.0,
        initial_font_zero_advance: 8.0,
        initial_font_size_raw: 16 << 6,
        default_font_size_raw: 16 << 6,
        device_pixels_per_css_pixel: 1.0,
        ..Default::default()
    };
    engine
}

/// The names `atom` interned: the strings, kept alive for their identities to stay theirs, and
/// the name of each atom, for a failure to name it by.
#[derive(Default)]
struct Names {
    strings: Vec<ak::Utf16FlyString>,
    by_atom: HashMap<StyleAtomID, String>,
}

thread_local! {
    static NAMES: RefCell<Names> = RefCell::new(Names::default());
}

/// The atom of `name`, as the host interns the names elements carry and selectors test.
pub(super) fn atom(engine: &mut StyleEngine, name: &str) -> StyleAtomID {
    let string = ak::Utf16FlyString::from_utf16(&name.encode_utf16().collect::<Vec<_>>());
    let atom = engine.intern_atom(string.raw_identity());
    // NB: Each engine has atoms of its own, so the name of an atom is the one interned last.
    NAMES.with_borrow_mut(|names| {
        names.strings.push(string);
        names.by_atom.insert(atom, name.to_owned());
    });
    atom
}

fn name_of(atom: StyleAtomID) -> String {
    NAMES.with_borrow(|names| {
        names
            .by_atom
            .get(&atom)
            .cloned()
            .unwrap_or_else(|| format!("atom{}", atom.0))
    })
}

pub(super) fn allocate_nodes(engine: &mut StyleEngine, count: usize) -> Vec<StyleNodeID> {
    let mut raw = vec![0_u32; count];
    engine.allocate_style_nodes(&mut raw);
    raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect()
}

pub(super) fn relations(
    parent: Option<StyleNodeID>,
    previous: Option<StyleNodeID>,
    next: Option<StyleNodeID>,
) -> TreeRelations {
    TreeRelations {
        parent,
        previous_element_sibling: previous,
        next_element_sibling: next,
        ..TreeRelations::detached(TreeScopeID::DOCUMENT)
    }
}

pub(super) fn add_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: LocalFeatureKey) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Absent),
        InputValue::Feature(FeatureValue::Present),
    );
}

pub(super) fn remove_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: LocalFeatureKey) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Present),
        InputValue::Feature(FeatureValue::Absent),
    );
}

pub(super) fn set_atom_feature(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    feature: LocalFeatureKey,
    atom: StyleAtomID,
) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Absent),
        InputValue::Feature(FeatureValue::Atom(atom)),
    );
}

pub(super) fn set_tag(engine: &mut StyleEngine, node: StyleNodeID, tag: &str) {
    let tag = atom(engine, tag);
    set_atom_feature(engine, node, LocalFeatureKey::TagName, tag);
}

pub(super) fn add_class(engine: &mut StyleEngine, node: StyleNodeID, class: &str) {
    let class = atom(engine, class);
    add_feature(engine, node, LocalFeatureKey::Class(class));
}

pub(super) fn remove_class(engine: &mut StyleEngine, node: StyleNodeID, class: &str) {
    let class = atom(engine, class);
    remove_feature(engine, node, LocalFeatureKey::Class(class));
}

/// Builds the document element `root`, a `div`, with `count - 1` `div` children, through the same
/// delta path C++ drives.
pub(super) fn document_with_children(count: usize) -> (StyleEngine, Vec<StyleNodeID>) {
    let mut engine = hosted_engine();
    let nodes = allocate_nodes(&mut engine, count);
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..count {
        let previous = (index > 1).then(|| nodes[index - 1]);
        engine.record_tree_delta(nodes[index], None, Some(relations(Some(nodes[0]), previous, None)));
    }
    engine.set_element_adjustment_facts(nodes[0], element_adjustment_fact::IS_DOCUMENT_ELEMENT);
    for &node in &nodes {
        set_tag(&mut engine, node, "div");
    }
    (engine, nodes)
}

/// Builds `root -> [a, b, c]`.
pub(super) fn linear_document() -> (StyleEngine, Vec<StyleNodeID>) {
    document_with_children(4)
}

/// Builds `root -> a -> b -> c`.
pub(super) fn nested_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let mut engine = hosted_engine();
    let nodes = allocate_nodes(&mut engine, 4);
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for pair in nodes.windows(2) {
        engine.record_tree_delta(pair[1], None, Some(relations(Some(pair[0]), None, None)));
    }
    engine.set_element_adjustment_facts(nodes[0], element_adjustment_fact::IS_DOCUMENT_ELEMENT);
    for &node in &nodes {
        set_tag(&mut engine, node, "div");
    }
    (engine, nodes)
}

/// Attaches the shadow tree `scope` to `host`: a shadow root and a chain of `depth` `div`
/// elements below it, root first.
pub(super) fn attach_shadow_tree(
    engine: &mut StyleEngine,
    host: StyleNodeID,
    scope: TreeScopeID,
    depth: usize,
) -> Vec<StyleNodeID> {
    let nodes = allocate_nodes(engine, depth + 1);
    engine.record_tree_delta(nodes[0], None, Some(TreeRelations::detached(scope)));
    for pair in nodes.windows(2) {
        engine.record_tree_delta(
            pair[1],
            None,
            Some(TreeRelations {
                parent: Some(pair[0]),
                tree_scope: scope,
                ..TreeRelations::detached(scope)
            }),
        );
        set_tag(engine, pair[1], "div");
    }
    engine.set_shadow_root(host, nodes[0]);
    engine.set_tree_scope_root(scope, nodes[0]);
    nodes
}

/// Builds `root -> [a, b, c]` with `.target:nth-of-type(2)`, where `a` and `c` are of one type and
/// `b`, with the same local name in another namespace, of another, and only `c` is a target.
pub(super) fn typed_nth_of_type_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".target:nth-of-type(2) { color: rgb(0, 128, 0); }");
    let first_namespace = atom(&mut engine, "first-namespace");
    let other_namespace = atom(&mut engine, "other-namespace");
    for (node, namespace) in [
        (nodes[1], first_namespace),
        (nodes[2], other_namespace),
        (nodes[3], first_namespace),
    ] {
        engine.set_element_namespace(node, namespace);
    }
    add_class(&mut engine, nodes[3], "target");
    (engine, nodes)
}

/// Builds `root -> [a, b, c]` with `.target:nth-child(3n)`, where every child is a target.
pub(super) fn nth_target_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".target:nth-child(3n) { color: rgb(0, 128, 0); }");
    for &node in &nodes[1..] {
        add_class(&mut engine, node, "target");
    }
    (engine, nodes)
}

/// An author sheet attached to `scope`.
pub(super) fn add_sheet(engine: &mut StyleEngine, scope: TreeScopeID) -> SheetID {
    add_sheet_with_origin(engine, scope, CascadeOrigin::Author)
}

pub(super) fn add_sheet_with_origin(engine: &mut StyleEngine, scope: TreeScopeID, origin: CascadeOrigin) -> SheetID {
    let sheet = engine.add_sheet(origin);
    engine.attach_sheet(sheet, scope);
    sheet
}

/// A style rule parsed as the host parses a sheet's: its selectors and its declarations.
pub(super) struct ParsedRule {
    selectors: Arc<crate::css::selector_parser::RustParsedSelectorList>,
    pub(super) declarations: crate::css::declaration_block::SharedDeclarationBlock,
}

/// The style rules of `source`.
pub(super) fn parse_rules(source: &str) -> Vec<ParsedRule> {
    use crate::css::css_tokenizer::TokenizerInput;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::rule::{NativeRuleList, NativeRuleType, rust_rule_list_count, rust_rule_type};
    let units: Vec<_> = source.encode_utf16().collect();
    // NB: A random function parses only where its declaration is numbered, as the host numbers
    //     those of a sheet.
    let random_function_index = crate::css::parser::value_parser::RandomFunctionIndex::default();
    let context = crate::css::parser::value_parser::ParseContext {
        random_function_index: Some(&random_function_index),
        ..Default::default()
    };
    let rules = NativeRuleList::from_parsed(parse_shared_stylesheet(TokenizerInput::Utf16(&units), Some(&context)));
    let count = rust_rule_list_count(&rules);
    assert_ne!(count, 0, "no rule parses from {source:?}");
    (0..count)
        .map(|index| {
            let rule = rules.rule_at(index);
            assert!(
                matches!(rust_rule_type(&rule), NativeRuleType::Style),
                "only style rules are published: {source:?}"
            );
            ParsedRule {
                selectors: rule.style_rule().selectors(),
                declarations: rule.cascade_declarations().expect("a style rule declares"),
            }
        })
        .collect()
}

/// Append `rule` to `sheet` with its selectors, answering the rule it is.
fn publish_parsed_rule(engine: &mut StyleEngine, sheet: SheetID, rule: &ParsedRule) -> RuleID {
    let published = publish_style_rule(
        engine,
        sheet.0 + 1,
        0,
        &rule.selectors.selectors,
        &BoundScopeChain::default(),
    );
    assert_ne!(published, 0, "the rule is published");
    RuleID(published - 1)
}

/// Append the style rules of `source` to `sheet`, as the host publishes a sheet's rules: their
/// selectors, then their declarations.
pub(super) fn add_rules(engine: &mut StyleEngine, sheet: SheetID, source: &str) -> Vec<RuleID> {
    parse_rules(source)
        .iter()
        .map(|parsed| {
            let rule = publish_parsed_rule(engine, sheet, parsed);
            publish_rule_declarations(engine, rule.0 + 1, &parsed.declarations);
            rule
        })
        .collect()
}

/// Attach a user agent sheet of the style rules of `source` to the document, as the host does: the
/// process keeps its rules, which one published sheet holds for every document attaching `source`.
/// Answers the sheet and the rules, as the document names them.
pub(super) fn add_user_agent_sheet(engine: &mut StyleEngine, source: &str) -> (SheetID, Vec<RuleID>) {
    use std::hash::{Hash, Hasher};
    let mut hasher = crate::css::style::fast_hash::fast_hasher();
    source.hash(&mut hasher);
    let source_identity = hasher.finish();
    Arc::clone(&engine.process_style).publish_user_agent_sheet(source_identity, |engine, sheet| {
        add_rules(engine, sheet, source);
    });
    let sheet = engine.add_user_agent_sheet(source_identity);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rules = engine.rules.rules_in_sheet(sheet);
    (sheet, rules)
}

/// Take a style update and every later wave of its pass, as the host does when it installs
/// each wave; nothing is installed here. The engine, where it owns the document, restyles what
/// the transaction changed as it does.
pub(super) fn run_style_update(engine: &mut StyleEngine, root: StyleNodeID) {
    engine.run_style_update(root, |_| {});
}

/// Have the engine own the document under `root`, from a cold pass of it after the inputs
/// recorded so far, with its passes on one thread.
pub(super) fn start_live(engine: &mut StyleEngine, root: StyleNodeID) {
    start_live_on(engine, root, 1);
}

/// Have the engine own the document under `root` as `start_live` does, with its passes on
/// `threads` threads.
pub(super) fn start_live_on(engine: &mut StyleEngine, root: StyleNodeID, threads: u32) {
    run_style_update(engine, root);
    engine.restart_live_style(root, threads);
    // NB: The engine owned the document from its first transaction on, so the restart has the next
    //     transaction restyle everything. Take that one here, so the next a test takes restyles
    //     only what it changed.
    run_style_update(engine, root);
    assert!(engine.has_live_style(), "the engine owns the document");
}

/// What `node` matches by the reference matcher, one entry per rule and pseudo-element-ness.
type MatchedRule = (RuleID, bool, crate::css::selector::Specificity);

/// One entry per rule and pseudo-element-ness, with the greatest specificity among its matching
/// entries, as the cascade weighs a rule.
fn merged_by_rule(matches: impl Iterator<Item = MatchedRule>) -> Vec<MatchedRule> {
    let mut merged: Vec<MatchedRule> = Vec::new();
    for (rule, pseudo_element, specificity) in matches {
        match merged
            .iter_mut()
            .find(|(existing_rule, existing_pseudo, _)| *existing_rule == rule && *existing_pseudo == pseudo_element)
        {
            Some(existing) => existing.2 = existing.2.max(specificity),
            None => merged.push((rule, pseudo_element, specificity)),
        }
    }
    merged.sort_unstable();
    merged
}

pub(super) fn exact_matches(engine: &StyleEngine, node: StyleNodeID) -> Vec<MatchedRule> {
    let mut exact = Vec::new();
    assert!(
        engine
            .retained
            .append_exact_matches_in_every_scope(node, &mut exact)
            .is_ok(),
        "the reference matcher matches {}",
        describe(&engine.retained, node)
    );
    merged_by_rule(
        exact
            .iter()
            .map(|matched| (matched.rule, matched.pseudo_element.is_some(), matched.specificity)),
    )
}

/// The element, as its tag, ID and classes, for a failure to name.
pub(super) fn describe(state: &RetainedState, node: StyleNodeID) -> String {
    self_check::describe(state, node, &name_of)
}

/// The record the engine holds for `node`.
pub(super) fn live_record(engine: &StyleEngine, node: StyleNodeID) -> Arc<Record> {
    let records = engine
        .retained
        .live_style
        .as_ref()
        .expect("the engine is live")
        .records();
    records
        .get(node)
        .unwrap_or_else(|| panic!("the engine holds a record for {}", describe(&engine.retained, node)))
        .clone()
}

/// Whether two records hold equal values, as `assert_live_matches_cold` compares them.
pub(super) fn records_equal(first: &Record, second: &Record) -> bool {
    self_check::record_differences(first, second).is_empty()
}

/// Whether the records the engine holds for `first` and `second` hold equal values, for a test
/// to tell that a rule reached an element.
pub(super) fn live_records_equal(engine: &StyleEngine, first: StyleNodeID, second: StyleNodeID) -> bool {
    records_equal(&live_record(engine, first), &live_record(engine, second))
}

/// How many passes settling the engine may take, as levels of elements composing animations
/// below one another.
const MAX_SETTLING_PASSES: usize = 1024;

/// Settle the engine, then check every record it holds for the document under `root`, of an
/// element and of its pseudo-elements, against the record a cold pass over the document computes,
/// and what that pass matched for each element against the reference matcher. Answers how many
/// elements it checked.
///
/// The records are compared by value: the payloads of each group and the longhand tables as the
/// host is handed them, and the custom properties by what each name resolves to, since the live
/// records are not the cold pass's, and their custom property stores chain to other parents.
pub(super) fn assert_live_matches_cold(engine: &mut StyleEngine, root: StyleNodeID) -> usize {
    assert!(engine.has_live_style(), "the engine owns the document");
    for _ in 0..MAX_SETTLING_PASSES {
        if !engine
            .retained
            .live_style
            .as_ref()
            .is_some_and(|live| live.has_pending())
        {
            break;
        }
        engine.live_restyle(root);
    }
    assert!(engine.has_live_style(), "the engine kept the document as it settled");
    let self_check::ReferencePass {
        records: cold, outputs, ..
    } = self_check::reference_pass(engine, root, false, 1).expect("the cold pass styles the document");
    let state = &engine.retained;
    let live = state.live_style.as_ref().expect("the engine is live").records();
    let describe = |node| describe(state, node);
    let (mut failures, checked) = self_check::differences(state, root, live, &cold, &describe);
    for node in state.projection.style_preorder(root) {
        let Some(output) = outputs.get(node.element_slot()).and_then(WriteOnce::get) else {
            continue;
        };
        match output.outcome {
            traversal::Outcome::CascadeDeclined | traversal::Outcome::ComputeDeclined => {
                failures.push(format!("{}: the cold pass declined to style it", describe(node)));
            }
            _ => {}
        }
        if let Some(matches) = &output.matches {
            let ours = merged_by_rule(
                matches
                    .iter()
                    .map(|matched| (matched.rule, matched.pseudo_element(), matched.specificity)),
            );
            let reference = exact_matches(engine, node);
            if ours != reference {
                failures.push(format!(
                    "{}: the cold pass matched {ours:?}, the reference matcher {reference:?}",
                    describe(node)
                ));
            }
        }
    }
    assert!(
        failures.is_empty(),
        "the engine is not what a cold pass computes:\n{}",
        failures.join("\n")
    );
    checked
}

mod tests {
    use super::*;

    #[test]
    fn an_element_holding_a_record_only_as_installed_hands_down_its_custom_properties() {
        let (mut engine, nodes) = nested_document();
        start_live(&mut engine, nodes[0]);
        engine.install_test_record(computed::ComputedStyleTarget::new(nodes[1], u8::MAX), None, None, 0);
        let cells = crate::css::style::pipeline::records::RecordCells::default();
        let records = crate::css::style::pipeline::records::Records::new(&engine.retained, &cells, None);
        assert!(records.custom_stores_of(nodes[1]).is_some());
    }

    #[test]
    fn a_class_change_restyles_a_plain_document_as_a_cold_pass_does() {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".target { color: rgb(0, 128, 0); width: 10px; } .target::before { content: \"x\"; }",
        );
        add_class(&mut engine, nodes[2], "target");
        start_live(&mut engine, nodes[0]);
        assert_eq!(assert_live_matches_cold(&mut engine, nodes[0]), nodes.len());
        assert!(!live_records_equal(&engine, nodes[1], nodes[2]));

        add_class(&mut engine, nodes[1], "target");
        remove_class(&mut engine, nodes[2], "target");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(live_records_equal(&engine, nodes[2], nodes[3]));
        assert!(!live_records_equal(&engine, nodes[1], nodes[2]));
    }

    #[test]
    fn a_guard_change_restyles_a_nested_document_as_a_cold_pass_does() {
        let (mut engine, nodes) = nested_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".guard { color: rgb(0, 0, 255); --size: 4px; } .guard .target { width: var(--size); }",
        );
        add_class(&mut engine, nodes[1], "guard");
        add_class(&mut engine, nodes[3], "target");
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);

        remove_class(&mut engine, nodes[1], "guard");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(live_records_equal(&engine, nodes[1], nodes[2]));

        add_class(&mut engine, nodes[2], "guard");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, nodes[1], nodes[2]));
    }

    #[test]
    fn a_class_change_in_a_shadow_tree_restyles_it_as_a_cold_pass_does() {
        let (mut engine, nodes) = linear_document();
        let scope = TreeScopeID(1);
        let shadow = attach_shadow_tree(&mut engine, nodes[1], scope, 3);
        let document_sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, document_sheet, ".target { color: rgb(255, 0, 0); }");
        let shadow_sheet = add_sheet(&mut engine, scope);
        add_rules(
            &mut engine,
            shadow_sheet,
            ":host { width: 20px; } .target { color: rgb(0, 128, 0); }",
        );
        add_class(&mut engine, shadow[2], "target");
        start_live(&mut engine, nodes[0]);
        let checked = assert_live_matches_cold(&mut engine, nodes[0]);
        assert_eq!(checked, nodes.len() + shadow.len() - 1);

        remove_class(&mut engine, shadow[2], "target");
        add_class(&mut engine, shadow[3], "target");
        add_class(&mut engine, nodes[3], "target");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, shadow[3], nodes[3]));
    }

    #[test]
    fn a_shadow_root_matches_no_compound_left_of_a_combinator() {
        let (mut engine, nodes) = linear_document();
        let scope = TreeScopeID(1);
        let shadow = attach_shadow_tree(&mut engine, nodes[1], scope, 2);
        let shadow_sheet = add_sheet(&mut engine, scope);
        add_rules(
            &mut engine,
            shadow_sheet,
            "* > .child { color: rgb(0, 128, 0); } * .descendant { width: 20px; } \
             :not(.absent) > .child { height: 20px; }",
        );
        add_class(&mut engine, shadow[2], "child");
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, shadow[2], nodes[2]));

        // The top-level element of the shadow tree has the shadow root for its parent and only
        // ancestor, which is not an element and matches nothing.
        remove_class(&mut engine, shadow[2], "child");
        add_class(&mut engine, shadow[1], "child");
        add_class(&mut engine, shadow[1], "descendant");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(exact_matches(&engine, shadow[1]).is_empty());
        assert!(live_records_equal(&engine, shadow[1], shadow[2]));

        remove_class(&mut engine, shadow[1], "child");
        add_class(&mut engine, shadow[2], "descendant");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, shadow[1], shadow[2]));
    }

    #[test]
    fn a_departure_restyles_an_nth_of_type_document_as_a_cold_pass_does() {
        let (mut engine, nodes) = typed_nth_of_type_document();
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, nodes[2], nodes[3]));
        let untargeted = live_record(&engine, nodes[1]);

        // Another type leaving moves no position of the target's type.
        engine.record_tree_delta(
            nodes[2],
            Some(relations(Some(nodes[0]), Some(nodes[1]), Some(nodes[3]))),
            None,
        );
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, nodes[1], nodes[3]));

        // The same type leaving makes the target the first of its type.
        engine.record_tree_delta(nodes[1], Some(relations(Some(nodes[0]), None, Some(nodes[3]))), None);
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(records_equal(&live_record(&engine, nodes[3]), &untargeted));
    }

    #[test]
    fn arrivals_and_departures_restyle_an_nth_child_document_as_a_cold_pass_does() {
        let (mut engine, nodes) = nth_target_document();
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, nodes[2], nodes[3]));

        // A departure makes the third child the second.
        engine.record_tree_delta(nodes[1], Some(relations(Some(nodes[0]), None, Some(nodes[2]))), None);
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(live_records_equal(&engine, nodes[2], nodes[3]));

        // An arrival before both makes the second child the third.
        let inserted = allocate_nodes(&mut engine, 1)[0];
        engine.record_tree_delta(inserted, None, Some(relations(Some(nodes[0]), None, Some(nodes[2]))));
        set_tag(&mut engine, inserted, "div");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!live_records_equal(&engine, nodes[2], nodes[3]));
    }
}

/// A declaration block declaring each `(property, important, value)` of `declarations`.
pub(crate) fn declaration_block(
    declarations: &[(u16, bool, crate::css::style_value::StyleValueData)],
) -> crate::css::declaration_block::SharedDeclarationBlock {
    crate::css::declaration_block::DeclarationBlockBuilder {
        properties: declarations
            .iter()
            .map(
                |(property, important, value)| crate::css::declaration_block::DeclaredProperty {
                    property_id: *property,
                    important: *important,
                    value: crate::css::style_value_arc::StyleValueArc::new(value.clone()),
                },
            )
            .collect(),
        custom_properties: Vec::new(),
    }
    .finish()
}
