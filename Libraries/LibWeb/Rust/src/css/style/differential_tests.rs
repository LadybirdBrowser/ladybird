/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::hash::Hash;
use std::hash::Hasher;
use std::rc::Rc;

use super::StyleEngine;
use super::batch_matcher::AncestorRequirements;
use super::batch_matcher::BatchMatchState;
use super::batch_matcher::BatchMatcher;
use super::batch_matcher::RuleMatch;
use super::batch_matcher::RuleMatches;
use super::cascade::PropertyWinner;
use super::cascade::WinnerGroupKey;
use super::computed::ComputedMetadataInput;
use super::fast_hash::fast_hasher;
use super::index::DispatchCandidateWorkspace;
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::index::StyleAtomID;
use super::instrumentation::Counters;
use super::memory::DeviceClass;
use super::partial_view::Lookup;
use super::program::CascadeOrigin;
use super::program::DeclarationBlockID;
use super::program::RuleID;
use super::program::RuleKind;
use super::program::StyleSheetObjectID;
use super::selector::FeatureTest;
use super::selector::MatchScratch;
use super::selector::SelectorOp;
use super::selector::SelectorProgramBuilder;
use super::transaction::InputKey;
use super::transaction::InputValue;
use super::transaction::TreeRelations;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;

// Cover hundreds of committed mutation batches while staying cheap enough for every Rust run.
const CONTAINERS: usize = 6;
const CHILDREN: usize = 8;
const CLASSES: usize = 8;
const RULES: usize = 16;
const SEEDS: u64 = 12;
const STEPS: usize = 48;

struct Lcg(u64);

impl Lcg {
    fn next(&mut self) -> u64 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        self.0 >> 33
    }

    fn under(&mut self, bound: usize) -> usize {
        (self.next() % bound as u64) as usize
    }
}

fn relations(parent: Option<StyleNodeID>, previous: Option<StyleNodeID>, next: Option<StyleNodeID>) -> TreeRelations {
    TreeRelations {
        parent,
        previous_element_sibling: previous,
        next_element_sibling: next,
        ..TreeRelations::detached(TreeScopeID::DOCUMENT)
    }
}

fn class_atom(index: usize) -> StyleAtomID {
    StyleAtomID(200 + index as u32)
}

fn tag_atom(node: StyleNodeID) -> StyleAtomID {
    StyleAtomID(100 + node.raw() % 3)
}

struct Workload {
    engine: StyleEngine,
    nodes: Vec<StyleNodeID>,
    root: StyleNodeID,
    containers: Vec<StyleNodeID>,
    children: Vec<Vec<StyleNodeID>>,
    classes: Vec<Vec<usize>>,
    rules: Vec<RuleID>,
    rule_conditions: Vec<bool>,
}

impl Workload {
    fn new(seed: u64) -> Self {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut raw = vec![0; 1 + CONTAINERS + CONTAINERS * CHILDREN];
        engine.allocate_style_nodes(&mut raw);
        let nodes: Vec<_> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
        let root = nodes[0];
        let containers = nodes[1..1 + CONTAINERS].to_vec();
        let children: Vec<Vec<_>> = (0..CONTAINERS)
            .map(|container| {
                nodes[1 + CONTAINERS + container * CHILDREN..1 + CONTAINERS + (container + 1) * CHILDREN].to_vec()
            })
            .collect();

        engine.record_tree_delta(root, None, Some(relations(None, None, None)));
        for (index, &container) in containers.iter().enumerate() {
            engine.record_tree_delta(
                container,
                None,
                Some(relations(
                    Some(root),
                    index.checked_sub(1).map(|previous| containers[previous]),
                    containers.get(index + 1).copied(),
                )),
            );
        }
        for (container, list) in children.iter().enumerate() {
            for (index, &child) in list.iter().enumerate() {
                engine.record_tree_delta(
                    child,
                    None,
                    Some(relations(
                        Some(containers[container]),
                        index.checked_sub(1).map(|previous| list[previous]),
                        list.get(index + 1).copied(),
                    )),
                );
            }
        }

        let mut rng = Lcg(seed);
        let mut classes = vec![Vec::new(); nodes.len()];
        for (index, &node) in nodes.iter().enumerate() {
            engine.record_input(
                InputKey::LocalFeature(node, LocalFeatureKey::TagName),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Atom(tag_atom(node))),
            );
            for _ in 0..rng.under(3) {
                let class = rng.under(CLASSES);
                if classes[index].contains(&class) {
                    continue;
                }
                classes[index].push(class);
                engine.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Class(class_atom(class))),
                    InputValue::Feature(FeatureValue::Absent),
                    InputValue::Feature(FeatureValue::Present),
                );
            }
        }

        let mut rules = Vec::new();
        for index in 0..RULES {
            let a = class_atom(index % CLASSES);
            let b = class_atom((index + 3) % CLASSES);
            let c = class_atom((index + 5) % CLASSES);
            let mut builder = SelectorProgramBuilder::new();
            let a = builder.push_feature(FeatureTest::Class(a));
            let b = builder.push_feature(FeatureTest::Class(b));
            let c = builder.push_feature(FeatureTest::Class(c));
            let selector = match index % 8 {
                0 => a,
                1 => {
                    let ancestor = builder.push(SelectorOp::Ancestor(a));
                    builder.push_compound(&[b, ancestor])
                }
                2 => {
                    let parent = builder.push(SelectorOp::Parent(a));
                    builder.push_compound(&[b, parent])
                }
                3 => {
                    let preceding = builder.push(SelectorOp::PrecedingSibling(a));
                    builder.push_compound(&[b, preceding])
                }
                4 => {
                    let previous = builder.push(SelectorOp::PreviousSibling(a));
                    builder.push_compound(&[b, previous])
                }
                5 => {
                    let preceding = builder.push(SelectorOp::PrecedingSibling(a));
                    let sibling = builder.push_compound(&[b, preceding]);
                    let ancestor = builder.push(SelectorOp::Ancestor(sibling));
                    builder.push_compound(&[c, ancestor])
                }
                6 => {
                    let ancestor = builder.push(SelectorOp::Ancestor(a));
                    let below = builder.push_compound(&[b, ancestor]);
                    let preceding = builder.push(SelectorOp::PrecedingSibling(below));
                    builder.push_compound(&[c, preceding])
                }
                _ => {
                    let previous = builder.push(SelectorOp::PreviousSibling(a));
                    let middle = builder.push_compound(&[b, previous]);
                    let previous = builder.push(SelectorOp::PreviousSibling(middle));
                    builder.push_compound(&[c, previous])
                }
            };
            builder.push_entry(selector);
            let selector = engine.programs.add(builder.finish());
            let sheet = engine.add_sheet(StyleSheetObjectID(1000 + index as u32), CascadeOrigin::Author);
            engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
            let rule = engine.append_rule(sheet, None, RuleKind::Style);
            engine.add_routing_rule(rule, selector);
            let mut version = engine.program.rule_version(rule);
            version.selector_program = Some(selector);
            version.declaration_block = Some(DeclarationBlockID(1000 + index as u32));
            engine.replace_rule_version(rule, version);
            engine.set_rule_declared_properties(rule, &[(1 + index as u16 % 5, index % 7 == 0)], true);
            rules.push(rule);
        }

        Self {
            engine,
            nodes,
            root,
            containers,
            children,
            classes,
            rules,
            rule_conditions: vec![true; RULES],
        }
    }

    fn node_relations(&self, node: StyleNodeID) -> TreeRelations {
        if node == self.root {
            return relations(None, None, None);
        }
        if let Some(index) = self.containers.iter().position(|&candidate| candidate == node) {
            return relations(
                Some(self.root),
                index.checked_sub(1).map(|previous| self.containers[previous]),
                self.containers.get(index + 1).copied(),
            );
        }
        self.children
            .iter()
            .enumerate()
            .find_map(|(container, children)| {
                let index = children.iter().position(|&candidate| candidate == node)?;
                Some(relations(
                    Some(self.containers[container]),
                    index.checked_sub(1).map(|previous| children[previous]),
                    children.get(index + 1).copied(),
                ))
            })
            .expect("every workload node remains connected")
    }

    fn mutate(&mut self, rng: &mut Lcg) -> (Option<StyleNodeID>, bool, String) {
        match rng.under(10) {
            0..=5 => {
                let index = 1 + rng.under(self.nodes.len() - 1);
                let node = self.nodes[index];
                let class = rng.under(CLASSES);
                let (old, new) = if self.classes[index].contains(&class) {
                    self.classes[index].retain(|&existing| existing != class);
                    (FeatureValue::Present, FeatureValue::Absent)
                } else {
                    self.classes[index].push(class);
                    (FeatureValue::Absent, FeatureValue::Present)
                };
                self.engine.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Class(class_atom(class))),
                    InputValue::Feature(old),
                    InputValue::Feature(new),
                );
                (None, false, format!("toggle {node:?} class{class}: {old:?} -> {new:?}"))
            }
            6..=7 => {
                let from = rng.under(CONTAINERS);
                if self.children[from].is_empty() {
                    return (None, false, format!("skip empty container {from}"));
                }
                let from_index = rng.under(self.children[from].len());
                let node = self.children[from][from_index];
                let old = self.node_relations(node);
                self.children[from].remove(from_index);
                let to = rng.under(CONTAINERS);
                let to_index = rng.under(self.children[to].len() + 1);
                self.children[to].insert(to_index, node);
                let new = self.node_relations(node);
                if old == new {
                    return (None, false, format!("skip no-op move of {node:?}"));
                }
                self.engine.record_tree_delta(node, Some(old), Some(new));
                (Some(node), false, format!("move {node:?}: {old:?} -> {new:?}"))
            }
            _ => {
                let index = rng.under(self.rules.len());
                self.rule_conditions[index] = !self.rule_conditions[index];
                self.engine
                    .set_rule_conditions_hold(self.rules[index], self.rule_conditions[index]);
                (
                    None,
                    true,
                    format!(
                        "set {:?} conditions_hold={}",
                        self.rules[index], self.rule_conditions[index]
                    ),
                )
            }
        }
    }
}

fn exact_matches(engine: &mut StyleEngine, node: StyleNodeID) -> Vec<RuleMatch> {
    engine.match_element_with_exact_matcher(node, false).unwrap()
}

fn retained_matches(engine: &mut StyleEngine, node: StyleNodeID) -> Option<Vec<RuleMatch>> {
    let retained = Rc::clone(engine.retained_match_answer(node).sparse().ok()?);
    if !matches!(
        engine.retained_match_answers.cascade_input_lookup(node),
        Lookup::Known(_)
    ) || !engine.match_answer_is_comparable_across_elements(node)
    {
        return None;
    }

    let (_, dispatch) = engine.prepare_scope_program(TreeScopeID::DOCUMENT);
    let mut orders: Vec<_> = (0..dispatch.entry_count())
        .map(|index| dispatch.entry_at(index))
        .map(|entry| (entry.rule, entry.program, entry.entry, entry.cascade_order))
        .collect();
    orders.sort_unstable_by_key(|&(rule, program, entry, _)| (rule, program, entry));
    orders.dedup_by_key(|&mut (rule, program, entry, _)| (rule, program, entry));
    let materialized = retained
        .iter()
        .copied()
        .filter(|entry| engine.program.rule_can_decide(entry.rule))
        .map(|entry| {
            let index = orders
                .binary_search_by_key(
                    &(entry.rule, entry.program, entry.entry),
                    |&(rule, program, entry, _)| (rule, program, entry),
                )
                .ok()?;
            entry.materialize(node, &engine.programs, orders[index].3)
        })
        .collect::<Option<Vec<_>>>()?;
    Some(engine.in_cascade_order(materialized, false))
}

fn batch_matches(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    ancestor_cache: bool,
    scratch: Option<&mut MatchScratch>,
) -> Vec<RuleMatch> {
    let (_, dispatch) = engine.prepare_scope_program(TreeScopeID::DOCUMENT);
    let requirements =
        ancestor_cache.then(|| AncestorRequirements::build(&engine.tree, engine.facts.primary(), &dispatch));
    let mut matcher = BatchMatcher::new(
        &engine.tree,
        engine.facts.primary(),
        &dispatch,
        &engine.programs,
        &engine.program,
    );
    if let Some(requirements) = &requirements {
        matcher = matcher.with_ancestor_requirements(requirements);
    }
    let mut matches = RuleMatches::new();
    matcher
        .match_node_collecting_requests(
            node,
            &mut matches,
            &mut Counters::new(),
            BatchMatchState {
                match_workspace: scratch,
                witness_effects: None,
                dispatch_workspace: &mut DispatchCandidateWorkspace::default(),
                requests: None,
                completed: None,
                prefix_states: None,
                deferred_prefix_matches: None,
            },
        )
        .result
        .unwrap();
    matches.as_slice().to_vec()
}

fn normalized_rows(mut matches: Vec<RuleMatch>) -> Vec<RuleMatch> {
    for matched in &mut matches {
        matched.cascade_order = 0;
    }
    matches.sort_unstable();
    matches
}

fn winners(engine: &mut StyleEngine, node: StyleNodeID, matches: &[RuleMatch]) -> Vec<PropertyWinner> {
    engine.resolved_cascade_winners_for_properties(node, matches, None, None)
}

fn style_record_for_winners(engine: &mut StyleEngine, winners: &[PropertyWinner], dependency_flags: u8) -> u64 {
    // The Rust harness has no C++ computed-group payloads. Project the winner inventory into the
    // opaque custom-property-environment input, then ask the real style-record interner for its
    // identity. Winner equality is checked directly; this also checks downstream canonicalization.
    let mut hasher = fast_hasher();
    winners.hash(&mut hasher);
    engine
        .intern_computed_groups(
            &[],
            0,
            hasher.finish(),
            ComputedMetadataInput {
                pseudo_element_styles: 0,
                dependency_flags,
                counter_style_environment_identity: 0,
                animation_overlay_identity: 0,
                animated_overlay: std::ptr::null(),
                animation_overlay_payloads: &[],
                longhand_table: std::ptr::null(),
            },
        )
        .style_record_identity
        .raw()
}

fn compare_mode(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    exact: &[RuleMatch],
    actual: Vec<RuleMatch>,
    mode: &str,
    seed: u64,
    step: usize,
) {
    assert_eq!(
        normalized_rows(actual.clone()),
        normalized_rows(exact.to_vec()),
        "selector rows diverged in {mode}; seed={seed:#018x}, first step={step}, node={node:?}"
    );
    let expected_winners = winners(engine, node, exact);
    let actual_winners = winners(engine, node, &actual);
    assert_eq!(
        actual_winners, expected_winners,
        "property winners diverged in {mode}; seed={seed:#018x}, first step={step}, node={node:?}"
    );
    let expected_style = style_record_for_winners(engine, &expected_winners, 0);
    let actual_style = style_record_for_winners(engine, &actual_winners, 0);
    assert_eq!(
        actual_style, expected_style,
        "style-record IDs diverged in {mode}; seed={seed:#018x}, first step={step}, node={node:?}"
    );
}

fn verify_step(
    workload: &mut Workload,
    planned: &[StyleNodeID],
    compare_retained: bool,
    history: &[String],
    seed: u64,
    step: usize,
) -> usize {
    let mut unplanned_retained_checks = 0;
    if compare_retained {
        for &node in &workload.nodes {
            if planned.contains(&node) {
                continue;
            }
            let Some(retained) = retained_matches(&mut workload.engine, node) else {
                continue;
            };
            let exact = exact_matches(&mut workload.engine, node);
            assert_eq!(
                normalized_rows(retained),
                normalized_rows(exact),
                "unplanned retained selector rows diverged; seed={seed:#018x}, step={step}, node={node:?}\noperation history:\n{}",
                history.join("\n")
            );
            unplanned_retained_checks += 1;
        }
    }

    let mut incremental = Vec::with_capacity(workload.nodes.len());
    for &node in &workload.nodes {
        incremental.push(
            workload
                .engine
                .match_element_for_purpose(node, false)
                .expect("a committed workload node has complete facts"),
        );
    }
    workload.engine.end_cold_matching_batch();

    // Reuse private scratch in both orders. In reverse order the sibling cursor
    // must restart exactly; cache presence and the asking order cannot change truth.
    for reverse in [false, true] {
        let mut scratch = MatchScratch::default();
        for index in 0..workload.nodes.len() {
            let index = if reverse {
                workload.nodes.len() - 1 - index
            } else {
                index
            };
            let node = workload.nodes[index];
            let actual = batch_matches(&mut workload.engine, node, true, Some(&mut scratch));
            compare_mode(
                &mut workload.engine,
                node,
                &incremental[index],
                actual,
                "private scratch with reordered asks",
                seed,
                step,
            );
        }
        assert!(
            scratch.capacity_bytes() > 0,
            "generated selectors must exercise scratch"
        );
    }

    for (&node, incremental) in workload.nodes.iter().zip(incremental) {
        let exact = exact_matches(&mut workload.engine, node);
        compare_mode(
            &mut workload.engine,
            node,
            &exact,
            incremental,
            "incremental",
            seed,
            step,
        );
        let prefix_off = batch_matches(&mut workload.engine, node, true, None);
        compare_mode(&mut workload.engine, node, &exact, prefix_off, "prefix-off", seed, step);
        let caches_off = batch_matches(&mut workload.engine, node, false, None);
        compare_mode(
            &mut workload.engine,
            node,
            &exact,
            caches_off,
            "optional-caches-off",
            seed,
            step,
        );
    }

    for &node in &workload.nodes {
        workload
            .engine
            .match_element_with_exact_matcher(node, true)
            .expect("a committed workload node has complete facts");
    }
    workload.engine.end_cold_matching_batch();
    unplanned_retained_checks
}

#[test]
fn seeded_transactions_match_the_cache_free_floor_in_every_mode() {
    for seed_index in 0..SEEDS {
        let seed = 0x5EED_5400_0000_0000_u64 | seed_index;
        let mut workload = Workload::new(seed);
        let mut rng = Lcg(seed ^ 0xD1FF_E2E1_71A1_0001);
        assert!(!workload.engine.take_style_transaction(workload.root, |_, _, _| {}));
        let initially_planned = workload.nodes.clone();
        let mut unplanned_retained_checks = verify_step(&mut workload, &initially_planned, true, &[], seed, 0);
        let mut history = Vec::new();

        for step in 1..=STEPS {
            history.push(format!("--- step {step}"));
            let mut caller_planned = Vec::new();
            let mut changed_rule_conditions = false;
            for _ in 0..3 {
                let (moved, conditions_changed, description) = workload.mutate(&mut rng);
                caller_planned.extend(moved);
                changed_rule_conditions |= conditions_changed;
                history.push(description);
            }
            let mut planned = Vec::new();
            let scoped = workload
                .engine
                .take_style_transaction(workload.root, |_, _, reactions| {
                    planned.extend(
                        reactions
                            .iter()
                            .map(|reaction| StyleNodeID::from_raw(reaction.style_node).unwrap()),
                    );
                });
            if scoped {
                planned.extend(caller_planned);
                planned.sort_unstable();
                planned.dedup();
            } else {
                planned.clone_from(&workload.nodes);
            }
            history.push(format!("planned: {planned:?}"));
            unplanned_retained_checks +=
                verify_step(&mut workload, &planned, !changed_rule_conditions, &history, seed, step);
        }
        assert!(
            unplanned_retained_checks > 0,
            "seed {seed:#018x} never checked an unplanned retained answer"
        );
    }
}

#[test]
fn budget_histories_preserve_answers_winners_and_records_across_mutations() {
    use super::memory::{MemoryCategory, Tier};

    let seed = 0x5EED_5400_0000_0003;
    let mut warm = Workload::new(seed);
    let mut pressured = Workload::new(seed);
    let mut warm_rng = Lcg(seed);
    let mut pressured_rng = Lcg(seed);
    let mut saw_pressure = false;
    let mut saw_retention_difference = false;
    for step in 0..12 {
        // The first loop starts admitting with both budgets. The low budget is crossed
        // by ordinary matching allocations, and only the next loop observes closure.
        warm.engine.memory.set_tier3_limit_for_test(u64::MAX);
        pressured.engine.memory.set_tier3_limit_for_test(1024);
        for workload in [&mut warm, &mut pressured] {
            workload.engine.take_style_transaction(workload.root, |_, _, _| {});
            workload.engine.begin_adaptive_cold_matching_batch(workload.root);
        }
        let mut warm_answers = Vec::new();
        let mut pressured_answers = Vec::new();
        for (workload, answers) in [(&mut warm, &mut warm_answers), (&mut pressured, &mut pressured_answers)] {
            for node in workload.nodes.clone() {
                let answer = workload.engine.match_element_for_purpose(node, false).unwrap();
                let exact = exact_matches(&mut workload.engine, node);
                assert_eq!(
                    normalized_rows(answer.clone()),
                    normalized_rows(exact),
                    "step {step}, {node:?}"
                );
                let winners = winners(&mut workload.engine, node, &answer);
                // Host-published dependency metadata changes independently of the budget.
                let dependency_flags = 1 << (step % 2);
                let record = style_record_for_winners(&mut workload.engine, &winners, dependency_flags);
                let view = workload.engine.style_record_view(record).unwrap();
                assert_eq!(view.dependency_flags, dependency_flags);
                answers.push((
                    normalized_rows(answer),
                    winners,
                    record,
                    view.dependency_flags,
                    view.pseudo_element_styles,
                    view.counter_style_environment_identity,
                ));
            }
            workload.engine.end_cold_matching_batch();
        }
        assert_eq!(warm_answers, pressured_answers, "step {step}");
        saw_pressure |= pressured.engine.memory.refusals(MemoryCategory::RetainedMatchAnswer) > 0;
        saw_retention_difference |= warm.engine.memory.bytes_in_tier(Tier::Acceleration)
            != pressured.engine.memory.bytes_in_tier(Tier::Acceleration);
        assert_eq!(warm.mutate(&mut warm_rng), pressured.mutate(&mut pressured_rng));
    }
    assert!(saw_pressure, "the low history never closed retained-answer admission");
    assert!(
        saw_retention_difference,
        "both histories retained the same acceleration"
    );
}

#[test]
fn incomplete_answer_batches_preserve_pending_lookups_and_release_ownership() {
    for discard in [false, true] {
        let mut workload = Workload::new(19);
        let transaction = workload.engine.take_transaction();
        workload.engine.release_transaction(transaction);
        let len = workload.nodes.len();
        let nodes = [
            workload.nodes[len - 2],
            workload.nodes[len - 3],
            workload.nodes[len - 1],
        ];
        let expected: Vec<_> = nodes
            .iter()
            .map(|&node| {
                let exact = exact_matches(&mut workload.engine, node);
                normalized_rows(workload.engine.matches_for_cascade(exact, false, Some(node)))
            })
            .collect();
        for &node in &nodes {
            let state = &mut workload.engine.state;
            state.retained_match_answers.forget(&mut state.match_answers, node);
            state.winner_groups.remove(node);
        }
        // NB: Deliberately leave the final subject's fact row unavailable. Earlier
        //     subjects can complete without it, in descending identity order.
        workload.engine.facts.forget(nodes[2]);
        workload.engine.begin_adaptive_cold_matching_batch(workload.root);
        assert!(
            workload
                .engine
                .complete_published_match_answers_for_closure(&nodes)
                .is_err()
        );
        assert!(workload.engine.match_answers.pending_reference_count() > 0);
        assert!(workload.engine.winner_groups.pending_reference_count() > 0);
        for index in 0..2 {
            assert!(
                workload
                    .engine
                    .retained_match_answers
                    .answer_identity(nodes[index])
                    .is_none()
            );
            assert_eq!(
                normalized_rows(workload.engine.consume_published_match_answer(nodes[index]).unwrap()),
                expected[index]
            );
            assert!(workload.engine.published_match_answer_signature(nodes[index]).is_some());
            let key = WinnerGroupKey::current(nodes[index], workload.engine.program.version());
            assert!(matches!(workload.engine.winner_groups.lookup(key), Lookup::Missing(_)));
            assert!(matches!(
                workload.engine.current_winner_groups().lookup(key),
                Lookup::Known(_)
            ));
        }
        if discard {
            workload
                .engine
                .state
                .discard_published_match_answers(&mut workload.engine.counters);
            assert_eq!(workload.engine.match_answers.pending_reference_count(), 0);
            assert_eq!(workload.engine.winner_groups.pending_reference_count(), 0);
            for &node in &nodes[..2] {
                assert!(workload.engine.state.current_published_answer(node).is_none());
                assert!(workload.engine.retained_match_answers.answer_identity(node).is_none());
            }
        } else {
            // NB: Refill the same facts without starting another transaction or
            //     installing the pending prefix before this completion call resumes it.
            let node = nodes[2];
            let node_index = workload.nodes.iter().position(|&candidate| candidate == node).unwrap();
            let state = &mut workload.engine.state;
            state.facts.set_tag(node, tag_atom(node), &mut state.memory);
            for &class in &workload.classes[node_index] {
                state.facts.set_class(node, class_atom(class), true, &mut state.memory);
            }
            state.facts.apply_staged(&mut state.memory);
            workload
                .engine
                .complete_published_match_answers_for_closure(&nodes)
                .unwrap();
            assert!(workload.engine.match_answers.pending_reference_count() > 0);
            for &node in &nodes {
                assert!(workload.engine.retained_match_answers.answer_identity(node).is_none());
            }
        }
        workload.engine.end_cold_matching_batch();
        assert_eq!(workload.engine.match_answers.pending_reference_count(), 0);
        assert_eq!(workload.engine.winner_groups.pending_reference_count(), 0);
        if !discard {
            for &node in &nodes {
                assert!(workload.engine.retained_match_answers.answer_identity(node).is_some());
                assert_eq!(
                    normalized_rows(retained_matches(&mut workload.engine, node).unwrap()),
                    normalized_rows(exact_matches(&mut workload.engine, node))
                );
            }
        }
    }
}
