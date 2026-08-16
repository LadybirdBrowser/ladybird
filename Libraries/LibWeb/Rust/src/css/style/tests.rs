/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::batch_matcher::insert_scope_rule;
use super::instrumentation::Counter;
use super::program::DeclarationBlockID;
use super::program::SelectorProgramID;
use super::transaction::InputKind;
use super::tree::PseudoElementKind;
use super::tree::PseudoElementTarget;
use super::*;

fn test_nth_position(argument: &str, of_type: bool) -> selector::NthPosition {
    let (step, offset) = match argument.split_once('n') {
        Some((step, offset)) => {
            let step = step
                .parse()
                .unwrap_or_else(|_| panic!("invalid test an+b step: {argument}"));
            let offset = offset.strip_prefix('+').unwrap_or(offset);
            let offset = if offset.is_empty() {
                0
            } else {
                offset
                    .parse()
                    .unwrap_or_else(|_| panic!("invalid test an+b offset: {argument}"))
            };
            (step, offset)
        }
        None => (
            0,
            argument
                .parse()
                .unwrap_or_else(|_| panic!("invalid test an+b position: {argument}")),
        ),
    };
    selector::NthPosition {
        step,
        offset,
        from_end: false,
        of_selector: None,
        of_type,
    }
}

fn test_selector_program_with_metadata(
    selector_text: &str,
    atoms: &[(&str, StyleAtomID)],
    specificity: Option<Specificity>,
    pseudo_element: Option<PseudoElementTarget>,
) -> selector::SelectorProgram {
    let mut builder = selector::SelectorProgramBuilder::new();
    for complex_selector_text in selector_text.split(',') {
        let mut complex_selector = None;
        let mut combinator = None;
        for compound_text in complex_selector_text.trim().split_ascii_whitespace() {
            if matches!(compound_text, ">" | "+" | "~") {
                assert!(
                    complex_selector.is_some(),
                    "test selector starts with a combinator: {selector_text}"
                );
                assert!(
                    combinator.replace(compound_text).is_none(),
                    "test selector has adjacent combinators: {selector_text}"
                );
                continue;
            }
            let mut operands = Vec::new();
            let relation = complex_selector.map(|left| match combinator.take() {
                None => builder.push_ancestor(left),
                Some(">") => builder.push(selector::SelectorOp::Parent(left)),
                Some("+") => builder.push(selector::SelectorOp::PreviousSibling(left)),
                Some("~") => builder.push(selector::SelectorOp::PrecedingSibling(left)),
                Some(_) => unreachable!(),
            });
            let mut remaining = compound_text;
            if let Some(rest) = remaining.strip_prefix('*') {
                operands.push(builder.push_feature(selector::FeatureTest::AnyElement));
                remaining = rest;
            }
            while let Some(rest) = remaining.strip_prefix('.') {
                let name_length = rest
                    .find(|character: char| !character.is_ascii_alphanumeric() && character != '-' && character != '_')
                    .unwrap_or(rest.len());
                assert!(
                    name_length != 0,
                    "test selector has an empty class name: {selector_text}"
                );
                let (name, rest) = rest.split_at(name_length);
                let atom = atoms
                    .iter()
                    .find_map(|&(candidate, atom)| (candidate == name).then_some(atom))
                    .unwrap_or_else(|| panic!("test selector names an unbound class: {name}"));
                operands.push(builder.push_feature(selector::FeatureTest::Class(atom)));
                remaining = rest;
            }
            let pseudo_class = remaining;
            if let Some(argument) = pseudo_class
                .strip_prefix(":nth-child(")
                .and_then(|argument| argument.strip_suffix(')'))
            {
                operands.push(builder.push(selector::SelectorOp::NthPosition(test_nth_position(argument, false))));
                remaining = "";
            } else if let Some(argument) = pseudo_class
                .strip_prefix(":nth-of-type(")
                .and_then(|argument| argument.strip_suffix(')'))
            {
                operands.push(builder.push(selector::SelectorOp::NthPosition(test_nth_position(argument, true))));
                remaining = "";
            } else if let Some(name) = pseudo_class
                .strip_prefix(":state(")
                .and_then(|name| name.strip_suffix(')'))
            {
                let state = atoms
                    .iter()
                    .find_map(|&(candidate, atom)| (candidate == name).then_some(atom))
                    .unwrap_or_else(|| panic!("test selector names an unbound state: {name}"));
                operands.push(builder.push(selector::SelectorOp::ValueState {
                    kind: selector::ValueStateTestKind::CustomState,
                    value: state,
                }));
                remaining = "";
            } else if let Some(name) = pseudo_class
                .strip_prefix(":not(:state(")
                .and_then(|name| name.strip_suffix("))"))
            {
                let state = atoms
                    .iter()
                    .find_map(|&(candidate, atom)| (candidate == name).then_some(atom))
                    .unwrap_or_else(|| panic!("test selector names an unbound state: {name}"));
                let state = builder.push(selector::SelectorOp::ValueState {
                    kind: selector::ValueStateTestKind::CustomState,
                    value: state,
                });
                operands.push(builder.push(selector::SelectorOp::Not(state)));
                remaining = "";
            }
            assert!(
                remaining.is_empty(),
                "unsupported test selector syntax: {selector_text}"
            );
            assert!(
                !operands.is_empty(),
                "test selector has an empty compound: {selector_text}"
            );
            if let Some(relation) = relation {
                operands.push(relation);
            }
            complex_selector = Some(builder.push_compound(&operands));
        }

        assert!(
            combinator.is_none(),
            "test selector ends with a combinator: {selector_text}"
        );
        let root = complex_selector.unwrap_or_else(|| panic!("test selector is empty"));
        let entry = builder.push_entry_for_pseudo(root, pseudo_element);
        if let Some(specificity) = specificity {
            builder.set_entry_specificity(entry, specificity);
        }
    }
    builder.finish()
}

fn test_selector_program(selector_text: &str, atoms: &[(&str, StyleAtomID)]) -> selector::SelectorProgram {
    test_selector_program_with_metadata(selector_text, atoms, None, None)
}

fn test_class_selector_program(
    selector_text: &str,
    atoms: &[(&str, StyleAtomID)],
    pseudo_element: Option<PseudoElementTarget>,
) -> selector::SelectorProgram {
    test_selector_program_with_metadata(
        selector_text,
        atoms,
        Some(Specificity {
            classes: 1,
            ..Specificity::default()
        }),
        pseudo_element,
    )
}

#[test]
fn test_selector_parser_builds_class_descendant_selectors() {
    let guard = StyleAtomID(1);
    let also = StyleAtomID(2);
    let target_atom = StyleAtomID(3);
    let parsed = test_selector_program(
        ".guard.also .target, .target",
        &[("guard", guard), ("also", also), ("target", target_atom)],
    );

    let mut builder = selector::SelectorProgramBuilder::new();
    let guard = builder.push_feature(selector::FeatureTest::Class(guard));
    let also = builder.push_feature(selector::FeatureTest::Class(also));
    let ancestor = builder.push_compound(&[guard, also]);
    let ancestor = builder.push_ancestor(ancestor);
    let target = builder.push_feature(selector::FeatureTest::Class(target_atom));
    let root = builder.push_compound(&[target, ancestor]);
    builder.push_entry(root);
    let target = builder.push_feature(selector::FeatureTest::Class(target_atom));
    builder.push_entry(target);

    assert!(parsed == builder.finish());
}

fn prepare_empty_transaction_fact_view(engine: &mut StyleEngine, root: StyleNodeID) {
    let mut transaction = engine.take_transaction();
    assert!(transaction.is_empty(), "test setup left a transaction pending");
    let view = engine.transaction_fact_view_for(&mut transaction, root, &ImpactRegions::new());
    engine.release_transaction(transaction);
    engine.transaction_fact_view = Some(view);
}

fn add_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: FeatureKey) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Absent),
        InputValue::Feature(FeatureValue::Present),
    );
}

fn remove_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: FeatureKey) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Present),
        InputValue::Feature(FeatureValue::Absent),
    );
}

fn set_atom_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: FeatureKey, atom: StyleAtomID) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Absent),
        InputValue::Feature(FeatureValue::Atom(atom)),
    );
}

fn discard_transaction(engine: &mut StyleEngine) {
    let transaction = engine.take_transaction();
    engine.release_transaction(transaction);
}

fn published_match_answer(node: u32, cascade_input: Option<u32>, match_count: usize) -> PublishedMatchAnswer {
    let rule_match = RuleMatch {
        node: StyleNodeID::element(node),
        pseudo_element: None,
        rule: RuleID(1),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: 0,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: u32::MAX,
    };
    PublishedMatchAnswer {
        node: StyleNodeID::element(node),
        cascade_input: cascade_input.map(CascadeInputID),
        matches: Some(vec![rule_match; match_count].into_boxed_slice()),
        cascade_winners_are_complete: true,
        observed: false,
    }
}

#[test]
fn repaired_selector_truth_deltas_do_not_depend_on_retained_order() {
    let node = StyleNodeID::element(1);
    let retained = [3, 1, 2, 5, 4].map(|rule| RetainedRuleMatch {
        rule: RuleID(rule),
        program: SelectorProgramID(1),
        entry: 0,
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: u32::MAX,
    });
    let mut current = [1, 2, 3, 4, 5].map(|rule| RuleMatch {
        node,
        pseudo_element: None,
        rule: RuleID(rule),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: rule,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: u32::MAX,
    });

    assert_eq!(
        planning::repaired_selector_truth_deltas(node, &retained, &mut current),
        Some(Vec::new())
    );
}

#[test]
fn verification_gates_only_execute_checks() {
    let _: () = verify_style_answer_patch(|| {});
    let _: () = verify_cascade_winners(|| {});
    let _: () = verify_style_plan_provenance(|| {});
    let _: () = verify_published_style_transaction(|| {});
}

#[test]
fn published_match_answer_accounting_stays_exact_incrementally() {
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut counters = Counters::new();
    let mut answers = PublishedMatchAnswers::default();

    let mut push_and_verify = |answer| {
        answers.push(answer, &mut memory, &mut counters);
        assert_eq!(answers.memory.bytes(), answers.recompute_capacity_bytes());
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::BatchScratch),
            answers.recompute_capacity_bytes()
        );
    };

    push_and_verify(published_match_answer(1, None, 2));
    push_and_verify(published_match_answer(2, Some(1), 3));
    push_and_verify(published_match_answer(3, Some(1), 3));
    for node in 4..128 {
        push_and_verify(published_match_answer(node, Some(node), node as usize % 5));
    }

    answers.clear();
    assert_eq!(answers.memory.bytes(), answers.recompute_capacity_bytes());
    assert_eq!(memory.bytes_in_category(MemoryCategory::BatchScratch), 0);
}

fn publish_current_cascade_as_computed(engine: &mut StyleEngine, node: StyleNodeID) {
    let cascade_state = engine
        .winner_groups
        .token_for(WinnerGroupKey::current(node, engine.program.version()))
        .sparse()
        .unwrap_or_else(|gap| panic!("the test style has no current cascade state: {gap:?}"));
    let target = computed::ComputedStyleTarget::new(node, u8::MAX);
    engine.publish_computed_groups(
        target,
        &[],
        0,
        0,
        computed::ComputedMetadataInput {
            pseudo_element_styles: 0,
            dependency_flags: 0,
            counter_style_environment_identity: 0,
            animation_overlay_identity: 0,
            animated_properties: std::ptr::null(),
            animation_overlay_payloads: &[],
            longhand_table: std::ptr::null(),
            reconstruction: computed::ComputedReconstructionMetadataInput {
                property_importance: &[],
                property_inheritance: &[],
                inheritance_dependent_properties: &[],
                inheritance_dependent_values: &[],
                raw_cascaded_font_size: std::ptr::null(),
            },
        },
    );
    assert_eq!(
        engine.computed_group_sets.bind_cascade_state(target, cascade_state),
        None
    );
}

#[test]
fn flat_tree_descendant_collection_follows_shadow_and_slot_relations() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 7];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let [host, assigned, unassigned, shadow_root, wrapper, slot, assigned_child] = nodes.as_slice() else {
        unreachable!()
    };

    engine.tree.set_first_element_child(*host, Some(*assigned));
    engine.tree.set_parent(*assigned, Some(*host));
    engine.tree.set_next_element_sibling(*assigned, Some(*unassigned));
    engine.tree.set_previous_element_sibling(*unassigned, Some(*assigned));
    engine.tree.set_parent(*unassigned, Some(*host));
    engine.tree.set_first_element_child(*shadow_root, Some(*wrapper));
    engine.tree.set_parent(*wrapper, Some(*shadow_root));
    engine.tree.set_first_element_child(*wrapper, Some(*slot));
    engine.tree.set_parent(*slot, Some(*wrapper));
    engine.tree.set_first_element_child(*assigned, Some(*assigned_child));
    engine.tree.set_parent(*assigned_child, Some(*assigned));
    engine.tree.set_shadow_root(*host, *shadow_root, &mut engine.memory);
    engine
        .tree
        .set_assigned_slot(*assigned, Some(*slot), &mut engine.memory);

    let mut descendants = Vec::new();
    engine.for_each_flat_tree_descendant(*host, |node| descendants.push(node));

    assert_eq!(descendants, vec![*wrapper, *slot, *assigned, *assigned_child]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn delta_batch_keeps_singletons_inline_and_consolidates_larger_batches() {
    let mut batch = DeltaBatch::default();
    batch.push(2_u32);
    assert_eq!(batch.as_slice(), &[2]);
    assert_eq!(batch.capacity_bytes(), 0);

    batch.consolidate();
    assert_eq!(batch.as_slice(), &[2]);
    assert_eq!(batch.capacity_bytes(), 0);

    batch.push(1);
    batch.push(2);
    assert!(batch.capacity_bytes() > 0);
    batch.consolidate();
    assert_eq!(batch.as_slice(), &[1, 2]);
}

#[test]
fn selector_truth_changes_consolidate_by_semantic_key() {
    let node = StyleNodeID::element(1);
    let rule = RuleID(2);
    let program = SelectorProgramID(3);
    let delta = |entry, change| SelectorTruthDelta {
        node,
        rule,
        program,
        entry,
        change,
        selector_truth_changed: true,
    };
    let mut changes = SelectorTruthChanges::default();
    changes.deltas.push(delta(0, SetChange::Added));
    changes.deltas.push(delta(0, SetChange::Added));
    changes.deltas.push(delta(0, SetChange::Removed));
    changes.deltas.push(delta(1, SetChange::Added));
    changes.deltas.push(delta(1, SetChange::Added));
    changes.deltas.push(delta(2, SetChange::Removed));
    changes.deltas.push(delta(2, SetChange::Removed));
    changes.refreshes.push(SelectorTruthRefresh {
        node,
        rule: Some((rule, program)),
    });
    changes.refreshes.push(SelectorTruthRefresh {
        node,
        rule: Some((rule, program)),
    });
    let mut counters = Counters::new();

    changes.consolidate(&mut counters);

    assert_eq!(
        changes.deltas.as_slice(),
        &[delta(1, SetChange::Added), delta(2, SetChange::Removed)]
    );
    assert_eq!(changes.refreshes.as_slice().len(), 1);
    assert_eq!(counters.get(Counter::SelectorTruthAdditions), 1);
    assert_eq!(counters.get(Counter::SelectorTruthRemovals), 1);
    assert_eq!(counters.get(Counter::SelectorTruthCancellations), 1);
    assert_eq!(counters.get(Counter::SelectorTruthRefreshes), 1);
}

#[test]
fn exact_tree_routing_is_reserved_for_incremental_changes() {
    assert!(exact_tree_routing_is_selective(20, 8_000));
    assert!(!exact_tree_routing_is_selective(8_000, 8_000));
}

#[test]
fn an_evicted_prefix_answer_is_a_typed_missing_key() {
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut answers = PrefixAnswerCache::default();
    let mut catalog = MatchAnswerCatalog::default();
    let contribution_key = PrefixContributionKey {
        program: ScopeProgramID(1),
        matches: PrefixMatchSetID::default(),
    };
    assert!(matches!(
        answers.prefix_contribution(&catalog, contribution_key),
        Lookup::Missing(gap) if gap == contribution_key
    ));
    let contribution =
        answers.remember_prefix_contribution(&mut catalog, contribution_key.program, contribution_key.matches, &[]);
    let non_prefix = PrefixAnswerCache::non_prefix_identity(&mut catalog, &[]);
    assert_eq!(contribution, non_prefix, "equal factors share one answer identity");
    assert!(matches!(
        answers.prefix_contribution(&catalog, contribution_key),
        Lookup::Known((identity, matches)) if identity == contribution && matches.is_empty()
    ));
    let key = PrefixAnswerKey {
        prefix_contribution: contribution,
        non_prefix_matches: non_prefix,
    };
    answers.remember(&mut catalog, key, &[], None, CascadeInputID(1), true);
    answers.settle_memory(&catalog, &mut memory);
    assert!(answers.retain(&mut memory));
    assert!(matches!(
        answers.lookup(key),
        Lookup::Known(answer)
            if catalog.answer(answer.matches).is_some_and(|matches| matches.is_empty())
    ));
    let retained_bytes = memory.bytes_in_category(MemoryCategory::PrefixAnswerCache);
    assert!(retained_bytes > 0);

    answers.make_scratch(&mut memory);
    assert_eq!(memory.bytes_in_category(MemoryCategory::PrefixAnswerCache), 0);
    assert_eq!(memory.bytes_in_category(MemoryCategory::BatchScratch), retained_bytes);
    assert!(matches!(answers.lookup(key), Lookup::Known(_)));
    assert!(answers.retain(&mut memory));
    assert_eq!(memory.bytes_in_category(MemoryCategory::BatchScratch), 0);
    assert_eq!(
        memory.bytes_in_category(MemoryCategory::PrefixAnswerCache),
        retained_bytes
    );

    answers.release(&mut catalog);
    assert!(catalog.answer(contribution).is_none());
    let replacement = catalog.intern(&[]);
    assert_ne!(replacement, contribution, "retired answer identities are never reused");
    assert!(matches!(
        answers.prefix_contribution(&catalog, contribution_key),
        Lookup::Missing(gap) if gap == contribution_key
    ));
    assert!(matches!(answers.lookup(key), Lookup::Missing(gap) if gap == key));
    assert_eq!(memory.bytes_in_category(MemoryCategory::PrefixAnswerCache), 0);
}

#[test]
fn prefix_answer_payload_accounting_uses_the_retained_match_shape() {
    let mut catalog = MatchAnswerCatalog::default();
    let identity = catalog.intern(&[RuleMatch {
        node: StyleNodeID::element(1),
        pseudo_element: None,
        rule: RuleID(1),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: 1,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: 0,
    }]);

    catalog.retain_prefix(identity);
    assert_eq!(catalog.prefix_payload_bytes, size_of::<RetainedRuleMatch>());
    catalog.retain_prefix(identity);
    assert_eq!(catalog.prefix_payload_bytes, size_of::<RetainedRuleMatch>());
    catalog.release_prefix(identity);
    assert_eq!(catalog.prefix_payload_bytes, size_of::<RetainedRuleMatch>());
    catalog.release_prefix(identity);
    assert_eq!(catalog.prefix_payload_bytes, 0);
}

#[test]
fn cascade_input_catalog_entries_follow_retained_column_lifetimes() {
    let mut catalog = MatchAnswerCatalog::default();
    let mut answers = RetainedMatchAnswers::default();
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let node = StyleNodeID::element(1);

    for _ in 0..128 {
        let identity = catalog.intern(&[]);
        answers.remember_cascade_input(&mut catalog, node, CascadeInputID(identity.0), &mut memory);
        answers.forget(&mut catalog, node);
    }

    let released_cascade_payload_bytes = catalog.sweep_unreferenced();
    answers.release_swept_cascade_payloads(released_cascade_payload_bytes);
    assert!(catalog.answers.live_is_empty());
    assert_eq!(
        memory.bytes_in_category(MemoryCategory::MatchAnswerIdentity),
        (answers.cascade_input_column.capacity() * size_of::<CascadeInputID>()) as u64
    );
}

#[test]
fn retained_match_answer_payloads_are_evictable_without_losing_identity() {
    assert_eq!(size_of::<RetainedRuleMatch>(), 20);

    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut answers = RetainedMatchAnswers::default();
    let mut catalog = MatchAnswerCatalog::default();
    let node = StyleNodeID::element(1);
    let mut programs = SelectorPrograms::new();
    let program = programs.add(test_class_selector_program(
        ".target",
        &[("target", StyleAtomID(1))],
        None,
    ));
    let retained = RuleMatch {
        node,
        pseudo_element: None,
        rule: RuleID(1),
        program,
        entry: 0,
        cascade_order: 7,
        specificity: Specificity {
            classes: 1,
            ..Specificity::default()
        },
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: 3,
    };

    assert!(
        answers
            .remember_prepared(
                &mut catalog,
                node,
                prepare_retained_match_answer([retained].into_iter()),
                &mut memory
            )
            .is_ok()
    );
    let (identity, compact) = match answers.lookup(node) {
        Lookup::Known(identity) => (*identity, catalog.retained_answer(*identity).unwrap()),
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a retained answer"),
    };
    let cascade_payload_bytes = size_of_val(compact.as_ref()) + 2 * size_of::<usize>();
    assert_eq!(compact[0].materialize(node, &programs, 7), Some(retained));
    assert!(memory.bytes_in_category(MemoryCategory::RetainedMatchAnswer) > 0);
    let cascade_input = catalog.intern(&[retained]);
    assert_eq!(
        cascade_input, identity,
        "both answer owners share one canonical identity"
    );
    let same_retained_match = RuleMatch {
        node: StyleNodeID::element(2),
        cascade_order: 99,
        ..retained
    };
    assert_eq!(catalog.intern(&[same_retained_match]), identity);
    answers.remember_cascade_input(&mut catalog, node, CascadeInputID(cascade_input.0), &mut memory);

    answers.evict(&mut catalog);
    assert!(catalog.retained_answer(identity).is_none());
    assert!(matches!(answers.lookup(node), Lookup::Missing(gap) if gap == node));
    assert!(matches!(
        answers.cascade_input_lookup(node),
        Lookup::Known(retained) if *retained == CascadeInputID(cascade_input.0)
    ));
    assert_eq!(memory.bytes_in_category(MemoryCategory::RetainedMatchAnswer), 0);
    assert_eq!(
        memory.bytes_in_category(MemoryCategory::MatchAnswerIdentity),
        (answers.cascade_input_column.capacity() * size_of::<CascadeInputID>() + cascade_payload_bytes) as u64
    );
}

#[test]
fn refused_retained_match_answer_replacement_forgets_only_that_node() {
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut answers = RetainedMatchAnswers::default();
    let mut catalog = MatchAnswerCatalog::default();
    let replaced_node = StyleNodeID::element(1);
    let preserved_node = StyleNodeID::element(2);
    let retained = RuleMatch {
        node: replaced_node,
        pseudo_element: None,
        rule: RuleID(1),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: 1,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: 0,
    };

    assert!(
        answers
            .remember_prepared(
                &mut catalog,
                replaced_node,
                prepare_retained_match_answer([retained].into_iter()),
                &mut memory
            )
            .is_ok()
    );
    let preserved = RuleMatch {
        node: preserved_node,
        ..retained
    };
    assert!(
        answers
            .remember_prepared(
                &mut catalog,
                preserved_node,
                prepare_retained_match_answer([preserved].into_iter()),
                &mut memory
            )
            .is_ok()
    );
    let shared_identity = match answers.lookup(replaced_node) {
        Lookup::Known(identity) => *identity,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a retained answer"),
    };
    assert!(matches!(answers.lookup(preserved_node), Lookup::Known(identity) if *identity == shared_identity));
    let charged = memory.bytes_in_category(MemoryCategory::RetainedMatchAnswer);
    assert!(
        memory
            .reserve(MemoryCategory::CascadeWinnerGroup, memory.tier3_limit() - charged)
            .is_granted()
    );

    let replacement = RuleMatch {
        rule: RuleID(2),
        program: SelectorProgramID(2),
        ..retained
    };
    assert!(
        answers
            .remember_prepared(
                &mut catalog,
                replaced_node,
                prepare_retained_match_answer([replacement].into_iter()),
                &mut memory
            )
            .is_err()
    );
    assert!(matches!(answers.lookup(replaced_node), Lookup::Missing(gap) if gap == replaced_node));
    assert!(matches!(answers.lookup(preserved_node), Lookup::Known(identity) if *identity == shared_identity));
    assert!(catalog.retained_answer(shared_identity).is_some());
    assert_eq!(memory.bytes_in_category(MemoryCategory::RetainedMatchAnswer), charged);
}

#[test]
fn retained_match_answer_replacement_releases_the_displaced_identity() {
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut answers = RetainedMatchAnswers::default();
    let mut catalog = MatchAnswerCatalog::default();
    let node = StyleNodeID::element(1);
    let original = RuleMatch {
        node,
        pseudo_element: None,
        rule: RuleID(1),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: 1,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: 0,
    };

    assert!(
        answers
            .remember_prepared(
                &mut catalog,
                node,
                prepare_retained_match_answer([original].into_iter()),
                &mut memory,
            )
            .is_ok()
    );
    let original_identity = match answers.lookup(node) {
        Lookup::Known(identity) => *identity,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected the original retained answer"),
    };

    let replacement = RuleMatch {
        rule: RuleID(2),
        program: SelectorProgramID(2),
        ..original
    };
    assert!(
        answers
            .remember_prepared(
                &mut catalog,
                node,
                prepare_retained_match_answer([replacement].into_iter()),
                &mut memory,
            )
            .is_ok()
    );
    let replacement_identity = match answers.lookup(node) {
        Lookup::Known(identity) => *identity,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected the replacement retained answer"),
    };
    assert_ne!(replacement_identity, original_identity);
    assert!(catalog.answer(original_identity).is_none());
    assert_eq!(catalog.retained_answer_count, 1);

    let mut referenced_programs = Vec::new();
    catalog.mark_referenced_selector_programs(&mut referenced_programs);
    assert!(!referenced_programs[1]);
    assert!(referenced_programs[2]);
}

#[test]
fn shared_retained_match_answer_lives_until_its_last_column_owner_forgets() {
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut answers = RetainedMatchAnswers::default();
    let mut catalog = MatchAnswerCatalog::default();
    let first_node = StyleNodeID::element(1);
    let second_node = StyleNodeID::element(2);
    let retained = RuleMatch {
        node: first_node,
        pseudo_element: None,
        rule: RuleID(1),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: 1,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: 0,
    };

    for node in [first_node, second_node] {
        assert!(
            answers
                .remember_prepared(
                    &mut catalog,
                    node,
                    prepare_retained_match_answer([RuleMatch { node, ..retained }].into_iter()),
                    &mut memory,
                )
                .is_ok()
        );
    }
    let shared_identity = match answers.lookup(first_node) {
        Lookup::Known(identity) => *identity,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a shared retained answer"),
    };
    assert_eq!(
        catalog.answers[shared_identity].as_ref().unwrap().retained_references,
        2
    );

    answers.forget_answer(&mut catalog, first_node);
    assert_eq!(
        catalog.answers[shared_identity].as_ref().unwrap().retained_references,
        1
    );
    assert!(catalog.retained_answer(shared_identity).is_some());

    answers.forget_answer(&mut catalog, second_node);
    assert!(catalog.answer(shared_identity).is_none());
    assert_eq!(catalog.retained_payload_bytes, 0);
    assert_eq!(catalog.retained_answer_count, 0);
}

#[test]
fn retained_match_answer_admission_reclaims_a_lower_benefit_view() {
    let (mut engine, nodes) = nested_document();
    let retained = RuleMatch {
        node: nodes[1],
        pseudo_element: None,
        rule: RuleID(1),
        program: SelectorProgramID(1),
        entry: 0,
        cascade_order: 1,
        specificity: Specificity::default(),
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: 0,
    };
    engine.remember_retained_match_answer(nodes[1], &[retained]);
    assert!(matches!(engine.retained_match_answer(nodes[1]), Lookup::Known(_)));
    engine.counters.bump(Counter::RetainedMatchAnswerDeltaPatches);
    let posting_bytes = engine.memory.bytes_in_category(MemoryCategory::FeaturePosting);
    assert!(posting_bytes > 0);
    let used = engine.memory.bytes_in_tier(memory::Tier::Acceleration);
    engine.memory.set_tier3_limit_for_test(used);

    let replacement = RuleMatch {
        rule: RuleID(2),
        program: SelectorProgramID(2),
        ..retained
    };
    engine.remember_retained_match_answer(nodes[1], &[replacement]);

    let missing_posting_bytes = engine.memory.bytes_in_category(MemoryCategory::FeaturePosting);
    assert!(missing_posting_bytes > 0);
    assert!(missing_posting_bytes < posting_bytes);
    let retained = match engine.retained_match_answer(nodes[1]) {
        Lookup::Known(answer) => answer,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a retained answer"),
    };
    assert_eq!(retained[0].rule, RuleID(2));
    assert_eq!(engine.counters.get(Counter::Tier3BenefitEvictions), 1);
    assert_eq!(engine.counters.get(Counter::Tier3AdmissionRetries), 1);
    assert_eq!(engine.counters.get(Counter::RetainedMatchAnswerRefusals), 0);
}

#[test]
fn an_evicted_feature_posting_is_missing_instead_of_empty() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let key = PostingKey::Selector(SelectorPostingKey::Class(target));
    assert!(matches!(engine.facts.postings().lookup(key), Lookup::Known(_)));
    engine.facts.postings_mut().evict(key);
    assert!(matches!(engine.facts.postings().lookup(key), Lookup::Missing(gap) if gap == key));

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
}

#[test]
fn part_names_are_derived_from_their_host_pairs() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw_nodes = [0; 3];
    engine.allocate_style_nodes(&mut raw_nodes);
    let element = StyleNodeID::from_raw(raw_nodes[0]).unwrap();
    let inner_host = StyleNodeID::from_raw(raw_nodes[1]).unwrap();
    let outer_host = StyleNodeID::from_raw(raw_nodes[2]).unwrap();
    let inner_name = StyleAtomID(1);
    let outer_name = StyleAtomID(2);
    let pairs = [
        (inner_name, inner_host),
        (outer_name, outer_host),
        (outer_name, inner_host),
    ];

    engine.set_element_parts(element, &pairs);

    assert_eq!(engine.facts.parts_of(element), &[inner_name, outer_name]);
    assert_eq!(engine.tree.part_hosts_of(element), &pairs);
}

#[test]
fn a_planned_node_never_returns_to_a_remaining_posting() {
    let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
    let mut postings = FeaturePostings::new();
    let key = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));
    let nodes = [
        StyleNodeID::element(1),
        StyleNodeID::element(2),
        StyleNodeID::element(3),
    ];
    for node in nodes {
        assert!(postings.insert(key, node, &mut memory));
    }
    postings.ensure_dense_ids();

    let mut counters = Counters::new();
    let mut plan = ImpactRegions::new();
    plan.add(ImpactRegion::Node(nodes[1]), &mut counters);
    let mut workspace = ImpactPlanningWorkspace::default();
    let mut candidates = Vec::new();

    let (_, reused, copied, inspected, pruned) = workspace
        .extend_remaining_posting(key, &postings, &plan, &mut candidates, None)
        .unwrap();
    assert!(!reused);
    assert_eq!(copied, 3);
    assert_eq!(inspected, 3);
    assert_eq!(pruned, 1);
    assert_eq!(candidates, [nodes[0], nodes[2]]);

    plan.add(ImpactRegion::Node(nodes[0]), &mut counters);
    candidates.clear();
    let (_, reused, copied, inspected, pruned) = workspace
        .extend_remaining_posting(key, &postings, &plan, &mut candidates, None)
        .unwrap();
    assert!(reused);
    assert_eq!(copied, 0);
    assert_eq!(inspected, 0);
    assert_eq!(pruned, 1);
    assert_eq!(candidates, [nodes[2]]);

    candidates.clear();
    let (_, reused, copied, inspected, pruned) = workspace
        .extend_remaining_posting(key, &postings, &plan, &mut candidates, None)
        .unwrap();
    assert!(reused);
    assert_eq!(copied, 0);
    assert_eq!(inspected, 0);
    assert_eq!(pruned, 0);
    assert_eq!(candidates, [nodes[2]]);

    let outside = StyleNodeID::element(4);
    plan.add(ImpactRegion::Node(outside), &mut counters);
    candidates.clear();
    let (_, reused, copied, inspected, pruned) = workspace
        .extend_remaining_posting(key, &postings, &plan, &mut candidates, None)
        .unwrap();
    assert!(reused);
    assert_eq!(copied, 0);
    assert_eq!(inspected, 0);
    assert_eq!(pruned, 0);
    assert_eq!(candidates, [nodes[2]]);

    plan.add(ImpactRegion::Node(nodes[2]), &mut counters);
    plan.add(ImpactRegion::Node(StyleNodeID::element(5)), &mut counters);
    candidates.clear();
    let (_, reused, copied, inspected, pruned) = workspace
        .extend_remaining_posting(key, &postings, &plan, &mut candidates, None)
        .unwrap();
    assert!(reused);
    assert_eq!(copied, 0);
    assert_eq!(inspected, 0);
    assert_eq!(pruned, 1);
    assert!(candidates.is_empty());

    let mut fallback_plan = ImpactRegions::new();
    let mut fallback_workspace = ImpactPlanningWorkspace::default();
    candidates.clear();
    fallback_workspace
        .extend_remaining_posting(key, &postings, &fallback_plan, &mut candidates, None)
        .unwrap();
    fallback_plan.add(ImpactRegion::Node(nodes[1]), &mut counters);
    for index in 0..MAX_POINT_REMOVED_EXACT_NODES - 1 {
        fallback_plan.add(
            ImpactRegion::Node(StyleNodeID::element(100 + index as u32)),
            &mut counters,
        );
    }
    candidates.clear();
    let (_, reused, copied, inspected, pruned) = fallback_workspace
        .extend_remaining_posting(key, &postings, &fallback_plan, &mut candidates, None)
        .unwrap();
    assert!(reused);
    assert_eq!(copied, 0);
    assert_eq!(inspected, 0);
    assert_eq!(pruned, 1);
    assert_eq!(candidates, [nodes[0], nodes[2]]);

    let mut reordered_plan = ImpactRegions::new();
    for index in (20..20 + impact::MAX_PAIRWISE_COALESCE as u32).rev() {
        reordered_plan.add(ImpactRegion::Node(StyleNodeID::element(index)), &mut counters);
    }
    let mut reordered_workspace = ImpactPlanningWorkspace::default();
    candidates.clear();
    reordered_workspace
        .extend_remaining_posting(key, &postings, &reordered_plan, &mut candidates, None)
        .unwrap();
    reordered_plan.add(ImpactRegion::Node(nodes[1]), &mut counters);
    let tree = StyleNodeTree::new(&mut memory);
    reordered_plan.normalize(&tree);
    candidates.clear();
    let (_, reused, copied, inspected, pruned) = reordered_workspace
        .extend_remaining_posting(key, &postings, &reordered_plan, &mut candidates, None)
        .unwrap();
    assert!(reused);
    assert_eq!(copied, 0);
    assert_eq!(inspected, 3);
    assert_eq!(pruned, 1);
    assert_eq!(candidates, [nodes[0], nodes[2]]);
}

#[test]
fn routing_phases_share_remaining_postings_for_one_transaction() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let guard = StyleAtomID(200);
    let also = StyleAtomID(201);
    let target = StyleAtomID(202);
    add_guarded_nth_target_rule(&mut engine, guard, also, target);

    const CONTAINER_COUNT: usize = 16;
    const LEAVES_PER_CONTAINER: usize = 32;
    let mut raw = vec![0_u32; 1 + CONTAINER_COUNT * (1 + LEAVES_PER_CONTAINER)];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));

    let mut next = 1;
    let mut previous_container = None;
    let mut containers = Vec::new();
    for _ in 0..CONTAINER_COUNT {
        let container = nodes[next];
        next += 1;
        engine.record_tree_delta(
            container,
            None,
            Some(relations(
                Some(nodes[0].raw()),
                previous_container.map(StyleNodeID::raw),
                None,
            )),
        );
        previous_container = Some(container);
        containers.push(container);
        for leaf_index in 0..LEAVES_PER_CONTAINER {
            let leaf = nodes[next];
            next += 1;
            engine.record_tree_delta(
                leaf,
                None,
                Some(relations(
                    Some(container.raw()),
                    (leaf_index != 0).then(|| nodes[next - 2].raw()),
                    None,
                )),
            );
            if leaf_index == 0 {
                add_feature(&mut engine, leaf, FeatureKey::Class(target));
            }
        }
        for class in [guard, also] {
            add_feature(&mut engine, container, FeatureKey::Class(class));
        }
    }
    discard_transaction(&mut engine);

    let mut inserted_raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut inserted_raw);
    let inserted = StyleNodeID::from_raw(inserted_raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(containers[0].raw()))),
    );
    remove_feature(&mut engine, containers[0], FeatureKey::Class(guard));

    let builds_before = engine.counters().get(Counter::RemainingPostingBuilds);
    let reuses_before = engine.counters().get(Counter::RemainingPostingReuses);
    assert!(engine.take_style_transaction_nodes(nodes[0], |_| {}));
    assert_eq!(
        engine.counters().get(Counter::RemainingPostingBuilds) - builds_before,
        1,
        "the sequence and local routing phases must materialize the target posting once"
    );
    assert!(
        engine.counters().get(Counter::RemainingPostingReuses) > reuses_before,
        "the later routing phase must reuse the posting retained by the earlier phase"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_document_root_arrival_is_already_a_whole_document_plan() {
    let (mut engine, nodes) = linear_document();
    let routed_before = engine.counters().get(Counter::RoutedEntryPoints);
    let mut planned = Vec::new();

    assert!(!engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, nodes.iter().map(|node| node.raw()).collect::<Vec<_>>());
    assert_eq!(engine.counters().get(Counter::RoutedEntryPoints), routed_before);
    assert_eq!(
        engine.memory().bytes_in_category(MemoryCategory::NormalizationJournal),
        0
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_non_bulk_document_root_arrival_publishes_style_reactions() {
    let (mut engine, nodes) = linear_document();
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }
    let mut published = Vec::new();

    assert!(!engine.take_style_transaction(nodes[0], |_, _, reactions| {
        published.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert_eq!(published, nodes.iter().map(|node| node.raw()).collect::<Vec<_>>());
    for node in nodes {
        assert_eq!(engine.consume_published_match_answer(node), Some(Vec::new()));
    }
}

#[test]
fn a_document_program_plan_skips_dom_routing() {
    let (mut engine, nodes) = linear_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);

    let program = engine.programs.add(test_selector_program("*", &[]));
    let sheet = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(2));
    engine.replace_rule_version(rule, version);

    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let routed_before = engine.counters().get(Counter::RoutedEntryPoints);
    let mut planned = Vec::new();
    assert!(!engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, nodes.iter().map(|node| node.raw()).collect::<Vec<_>>());
    assert_eq!(
        engine.counters().get(Counter::RoutedEntryPoints) - routed_before,
        0,
        "the document envelope makes every DOM route redundant"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_program_change_does_not_repeat_an_arriving_subtree() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let arriving: Vec<StyleNodeID> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    for (index, &node) in arriving.iter().enumerate() {
        let relations = match index {
            0 => relations(Some(nodes[0].raw()), Some(nodes[3].raw()), None),
            _ => relations(Some(arriving[index - 1].raw()), None, None),
        };
        engine.record_tree_delta(node, None, Some(relations));
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    planned.sort_unstable();
    let mut expected = vec![nodes[2].raw()];
    expected.extend(arriving.iter().map(|node| node.raw()));
    expected.sort_unstable();
    assert_eq!(planned, expected);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_rare_program_candidate_inside_an_arrival_is_already_covered() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let arriving = StyleNodeID::from_raw(raw[0]).unwrap();
    let target = StyleAtomID(200);
    engine.record_tree_delta(
        arriving,
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[3].raw()), None)),
    );
    add_feature(&mut engine, arriving, FeatureKey::Class(target));
    add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, [arriving.raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_program_region_inside_an_arrival_is_already_covered() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let arriving: Vec<StyleNodeID> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(
        arriving[0],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[3].raw()), None)),
    );
    engine.record_tree_delta(arriving[1], None, Some(relations(Some(arriving[0].raw()), None, None)));
    let guard = StyleAtomID(200);
    add_feature(&mut engine, arriving[0], FeatureKey::Class(guard));
    add_guard_universal_rule(&mut engine, guard);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, raw);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

fn relations(parent: Option<u32>, previous: Option<u32>, next: Option<u32>) -> TreeRelations {
    TreeRelations {
        parent: parent.map(StyleNodeID::element),
        previous_element_sibling: previous.map(StyleNodeID::element),
        next_element_sibling: next.map(StyleNodeID::element),
        ..TreeRelations::detached(TreeScopeID::DOCUMENT)
    }
}

#[test]
fn a_departed_following_sibling_anchor_widens_when_its_old_next_sibling_relocated() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();

    // The departed node's old next sibling is now last: root -> [first, second, old_next].
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, Some(raw[2]))));
    engine.record_tree_delta(
        nodes[2],
        None,
        Some(relations(Some(raw[0]), Some(raw[1]), Some(raw[3]))),
    );
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[0]), Some(raw[2]), None)));
    engine.apply_staged_tree_deltas();

    let departed_relations = relations(Some(raw[0]), None, Some(raw[3]));
    let transaction_inputs = [NormalizedInput {
        key: InputKey::TreeRelations(nodes[3]),
        old: InputValue::TreeRelations(Some(relations(Some(raw[0]), None, Some(raw[1])))),
        new: InputValue::TreeRelations(Some(relations(Some(raw[0]), Some(raw[2]), None))),
    }];
    let tree_routing = TreeRoutingMode {
        use_exact: false,
        has_before_sibling_relations: false,
        transaction_inputs: &transaction_inputs,
    };
    let old_next_relocated = tree_routing.tree_position_changed(nodes[3]);
    assert!(old_next_relocated);
    assert_eq!(
        engine.follow_from_departed_position(
            departed_relations,
            &[selector::InverseStep::FollowingSiblings],
            old_next_relocated,
        ),
        Some(ImpactRegion::Children(nodes[0]))
    );
}

/// Builds `root -> [a, b, c]` through the same delta path C++ drives.
fn linear_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(nodes[0].raw()), None, None)));
    engine.record_tree_delta(
        nodes[2],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[1].raw()), None)),
    );
    engine.record_tree_delta(
        nodes[3],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
    );
    (engine, nodes)
}

/// Builds `root -> outer -> inner -> target`.
fn nested_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(nodes[index - 1].raw()), None, None)),
        );
    }
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }
    (engine, nodes)
}

fn add_guard_target_rule(engine: &mut StyleEngine, guard: StyleAtomID, target: StyleAtomID) -> RuleID {
    let program = engine.programs.add(test_selector_program(
        ".guard .target",
        &[("guard", guard), ("target", target)],
    ));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn add_guard_target_rule_in_sheet(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    guard: StyleAtomID,
    target: StyleAtomID,
) -> RuleID {
    let program = engine.programs.add(test_selector_program(
        ".guard .target",
        &[("guard", guard), ("target", target)],
    ));
    let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn add_guard_universal_rule(engine: &mut StyleEngine, guard: StyleAtomID) {
    let program = engine
        .programs
        .add(test_selector_program(".guard *", &[("guard", guard)]));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_has_descendant_rule(engine: &mut StyleEngine, anchor: StyleAtomID, witness: StyleAtomID) {
    let mut builder = selector::SelectorProgramBuilder::new();
    let witness_test = builder.push_feature(selector::FeatureTest::Class(witness));
    let has_witness = builder.push_relative_exists(relative_selector::RelativeQuery {
        axis: RelativeAxis::Descendant,
        compound: witness_test,
        driving_feature: Some(FeatureKey::Class(witness)),
        simple: true,
        witness_is_below_the_axis: false,
        match_in_shadow_tree: false,
    });
    let anchor = builder.push_feature(selector::FeatureTest::Class(anchor));
    let selector = builder.push_compound(&[anchor, has_witness]);
    builder.push_entry(selector);
    let program = engine.programs.add(builder.finish());

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

#[test]
fn a_nested_arrival_routes_relational_facts_from_the_outer_subtree() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_descendant_rule(&mut engine, anchor, witness);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    add_feature(&mut engine, nodes[0], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    engine.record_tree_delta(nodes[1], None, Some(relations(Some(nodes[0].raw()), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(nodes[1].raw()), None, None)));
    add_feature(&mut engine, nodes[2], FeatureKey::Class(witness));

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, raw);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

fn add_has_sibling_rule(engine: &mut StyleEngine, anchor: StyleAtomID, witness: StyleAtomID, axis: RelativeAxis) {
    let mut builder = selector::SelectorProgramBuilder::new();
    let witness_test = builder.push_feature(selector::FeatureTest::Class(witness));
    let has_witness = builder.push_relative_exists(relative_selector::RelativeQuery {
        axis,
        compound: witness_test,
        driving_feature: Some(FeatureKey::Class(witness)),
        simple: matches!(axis, RelativeAxis::NextSibling | RelativeAxis::FollowingSibling),
        witness_is_below_the_axis: false,
        match_in_shadow_tree: false,
    });
    let anchor = builder.push_feature(selector::FeatureTest::Class(anchor));
    let selector = builder.push_compound(&[anchor, has_witness]);
    builder.push_entry(selector);
    let program = engine.programs.add(builder.finish());

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

#[test]
fn a_batch_of_arrivals_routes_a_following_sibling_anchor_once() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 6];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::FollowingSibling);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    for index in 2..6 {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(raw[0]), Some(raw[index - 1]), None)),
        );
    }
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.contains(&raw[1]), "the anchor is in the plan");
}

#[test]
fn a_batch_of_departures_routes_a_following_sibling_anchor_once() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 6];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::FollowingSibling);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    for index in 2..6 {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(raw[0]), Some(raw[index - 1]), None)),
        );
    }
    add_feature(&mut engine, nodes[1], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    // Two departures from one sequence, and the first one's recorded neighbour leaves right
    // after it: the anchors are asked once for the whole sequence, and a seam whose
    // neighbours are gone still places itself through the sibling on its other side.
    engine.record_tree_delta(
        nodes[4],
        Some(relations(Some(raw[0]), Some(raw[3]), Some(raw[5]))),
        None,
    );
    engine.record_tree_delta(
        nodes[3],
        Some(relations(Some(raw[0]), Some(raw[2]), Some(raw[5]))),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.contains(&raw[1]), "the anchor is in the plan");
}

#[test]
fn an_arrival_within_the_adjacent_reach_routes_the_anchor() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::NextSibling);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    // An element landing right beside the anchor breaks `.a:has(+ .w)` however little it
    // carries, so the anchor is asked.
    engine.record_tree_delta(
        nodes[3],
        None,
        Some(relations(Some(raw[0]), Some(raw[1]), Some(raw[2]))),
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.contains(&raw[1]), "the anchor is in the plan");
}

#[test]
fn an_arrival_beyond_the_adjacent_reach_routes_no_anchor() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::NextSibling);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    // An adjacent chain reaches one step back, so an element landing two steps past the
    // anchor cannot have moved `.a:has(+ .w)` and nothing is routed.
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[0]), Some(raw[2]), None)));
    engine.take_style_transaction_nodes(nodes[0], |_| {});
}

#[test]
fn a_departed_subtree_routes_sibling_subtree_anchors_above_its_parent() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::FollowingSiblingSubtree);

    // root -> [x(.a), p]; p -> [c1, c2]. `x:has(~ p .w)` reaches under p, so c2 leaving has
    // to ask x even though nothing beside x moved.
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[2]), None, None)));
    engine.record_tree_delta(nodes[4], None, Some(relations(Some(raw[2]), Some(raw[3]), None)));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    engine.record_tree_delta(nodes[4], Some(relations(Some(raw[2]), Some(raw[3]), None)), None);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.contains(&raw[1]), "the anchor is in the plan");
}

#[test]
fn a_retained_witness_carries_an_anchor_through_its_lifecycle() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    add_has_descendant_rule(&mut engine, anchor, witness);

    // root -> card(.card) -> [w1(.error), w2]
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[1]), None, None)));
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[1]), Some(raw[2]), None)));
    for (node, class) in [(nodes[1], anchor), (nodes[2], witness)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(!engine.match_element(nodes[1]).unwrap().is_empty());

    // A second witness appearing cannot flip an anchor that is already true, so the retained
    // witness answers for it and nothing is routed.
    add_feature(&mut engine, nodes[3], FeatureKey::Class(witness));
    let mut planned = Vec::new();
    engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    });
    assert_eq!(engine.counters().get(Counter::RelationalAnchorsSkippedByWitness), 1);
    assert!(!planned.contains(&raw[1]), "the anchor stays out of the plan");

    // The witness the entry does not name losing the feature cannot flip the anchor either,
    // and the retained witness proves it without any evaluation of the anchor.
    remove_feature(&mut engine, nodes[3], FeatureKey::Class(witness));
    let mut planned = Vec::new();
    engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    });
    assert_eq!(engine.counters().get(Counter::RelationalAnchorsSkippedByWitness), 2);
    assert!(!planned.contains(&raw[1]), "the anchor still stays out of the plan");

    // The retained witness losing the feature is exactly what the entry cannot vouch past:
    // the anchor is routed, recomputes to false, and the entry is cleared.
    remove_feature(&mut engine, nodes[2], FeatureKey::Class(witness));
    let mut planned = Vec::new();
    engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    });
    assert!(
        planned.contains(&raw[1]),
        "the anchor is asked once its witness is gone"
    );
    assert!(engine.match_element(nodes[1]).unwrap().is_empty(), "no witness left");

    // A witness returning is routed rather than skipped: false-to-true discovery never
    // consults a witness, it creates one.
    add_feature(&mut engine, nodes[2], FeatureKey::Class(witness));
    let mut planned = Vec::new();
    engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    });
    assert!(planned.contains(&raw[1]));
    assert!(!engine.match_element(nodes[1]).unwrap().is_empty());
    assert_eq!(engine.counters().get(Counter::RelationalAnchorsSkippedByWitness), 2);
}

#[test]
fn a_retained_witness_absorbs_arrivals_into_a_watched_sequence() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    let other = StyleAtomID(202);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::FollowingSibling);

    // root -> [a(.a), b(.other), w(.w)]
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[0]), Some(raw[2]), None)));
    for (node, class) in [(nodes[1], anchor), (nodes[2], other), (nodes[3], witness)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);
    assert!(!engine.match_element(nodes[1]).unwrap().is_empty());

    // An element landing in the sequence moves the seams, but a `~` witness that still follows
    // the anchor still witnesses it, so the anchor drops out of the plan.
    engine.record_tree_delta(
        nodes[4],
        None,
        Some(relations(Some(raw[0]), Some(raw[2]), Some(raw[3]))),
    );
    let mut planned = Vec::new();
    engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes));
    assert_eq!(engine.counters().get(Counter::RelationalAnchorsSkippedByWitness), 1);
    assert!(!planned.contains(&raw[1]), "the anchor stays out of the plan");

    // The witness itself departing is a different matter.
    engine.record_tree_delta(nodes[3], Some(relations(Some(raw[0]), Some(raw[4]), None)), None);
    let mut planned = Vec::new();
    engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes));
    assert!(planned.contains(&raw[1]), "the anchor is asked once its witness left");
}

#[test]
fn an_element_landing_between_an_anchor_and_its_adjacent_witness_routes_it() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let anchor = StyleAtomID(200);
    let witness = StyleAtomID(201);
    let other = StyleAtomID(202);
    add_has_sibling_rule(&mut engine, anchor, witness, RelativeAxis::NextSibling);

    // root -> [a(.a), w(.w)]
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    for (node, class) in [(nodes[1], anchor), (nodes[2], witness)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);
    assert!(!engine.match_element(nodes[1]).unwrap().is_empty());

    // The arrival breaks the adjacency the retained witness proved, so the entry cannot vouch
    // for the anchor and it is routed.
    engine.record_tree_delta(
        nodes[3],
        None,
        Some(relations(Some(raw[0]), Some(raw[1]), Some(raw[2]))),
    );
    add_feature(&mut engine, nodes[3], FeatureKey::Class(other));
    let mut planned = Vec::new();
    engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes));
    assert_eq!(engine.counters().get(Counter::RelationalAnchorsSkippedByWitness), 0);
    assert!(planned.contains(&raw[1]), "the anchor is in the plan");
    assert!(
        engine.match_element(nodes[1]).unwrap().is_empty(),
        "the adjacency is broken"
    );
}

fn add_guard_sibling_target_rule(engine: &mut StyleEngine, guard: StyleAtomID, target: StyleAtomID) -> RuleID {
    let program = engine.programs.add(test_selector_program(
        ".guard ~ .target",
        &[("guard", guard), ("target", target)],
    ));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn add_hover_sibling_target_rule(engine: &mut StyleEngine, target: StyleAtomID) {
    let mut builder = selector::SelectorProgramBuilder::new();
    let hover = builder.push(selector::SelectorOp::State(StateFact::Hover));
    let preceding_sibling = builder.push(selector::SelectorOp::PrecedingSibling(hover));
    let target = builder.push_feature(selector::FeatureTest::Class(target));
    let selector = builder.push_compound(&[target, preceding_sibling]);
    builder.push_entry(selector);
    let program = engine.programs.add(builder.finish());

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

#[test]
fn clearing_state_while_departing_routes_following_siblings() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    add_hover_sibling_target_rule(&mut engine, target);
    engine.record_input(
        InputKey::State(nodes[1], StateFact::Hover),
        InputValue::State(false),
        InputValue::State(true),
    );
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    discard_transaction(&mut engine);
    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    engine.match_element(nodes[2]).unwrap();
    engine.end_cold_matching_batch();

    engine.record_input(
        InputKey::State(nodes[1], StateFact::Hover),
        InputValue::State(true),
        InputValue::State(false),
    );
    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
        None,
    );

    let mut published = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        published.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert!(
        published.contains(&nodes[2].raw()),
        "the following sibling is published: {published:?}"
    );
}

/// An always-true positional test keeps a compound off every linear chain without changing
/// what it matches, so tests of the exact fallback machinery keep their workload now that
/// plain sibling chains answer from the prefix automaton.
/// A truth-neutral operand that keeps its compound off the prefix automaton: the doubly
/// negated positional test matches every element, and negation is neither prefix-local nor
/// canonical, so fixtures built with this stay on the exact match evaluator.
fn push_all_positions(builder: &mut selector::SelectorProgramBuilder) -> selector::SelectorNodeID {
    let position = builder.push(selector::SelectorOp::NthPosition(selector::NthPosition {
        step: 1,
        offset: 0,
        from_end: false,
        of_selector: None,
        of_type: false,
    }));
    let negated = builder.push(selector::SelectorOp::Not(position));
    builder.push(selector::SelectorOp::Not(negated))
}

fn add_guard_sibling_target_fallback_rule(engine: &mut StyleEngine, guard: StyleAtomID, target: StyleAtomID) -> RuleID {
    let mut builder = selector::SelectorProgramBuilder::new();
    let guard = builder.push_feature(selector::FeatureTest::Class(guard));
    let preceding_sibling = builder.push(selector::SelectorOp::PrecedingSibling(guard));
    let target = builder.push_feature(selector::FeatureTest::Class(target));
    let all_positions = push_all_positions(&mut builder);
    let selector = builder.push_compound(&[target, all_positions, preceding_sibling]);
    builder.push_entry(selector);
    let program = engine.programs.add(builder.finish());

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn add_guard_target_selector_list_rule(
    engine: &mut StyleEngine,
    first_guard: StyleAtomID,
    second_guard: StyleAtomID,
    target: StyleAtomID,
) -> RuleID {
    let program = engine.programs.add(test_selector_program(
        ".first-guard .target, .second-guard .target",
        &[
            ("first-guard", first_guard),
            ("second-guard", second_guard),
            ("target", target),
        ],
    ));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn add_target_rule_with_origin(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    target: StyleAtomID,
    origin: CascadeOrigin,
) -> RuleID {
    let program = engine
        .programs
        .add(test_class_selector_program(".target", &[("target", target)], None));

    let sheet = engine.add_sheet(sheet_object, origin);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn add_target_rule(engine: &mut StyleEngine, sheet_object: StyleSheetObjectID, target: StyleAtomID) -> RuleID {
    add_target_rule_with_origin(engine, sheet_object, target, CascadeOrigin::Author)
}

fn add_selector_list_rule(
    engine: &mut StyleEngine,
    first: StyleAtomID,
    second: StyleAtomID,
) -> (RuleID, SelectorProgramID) {
    let program = engine.programs.add(test_class_selector_program(
        ".first, .second",
        &[("first", first), ("second", second)],
        None,
    ));
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    (rule, program)
}

fn add_custom_state_rule(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    state: StyleAtomID,
    matches_state: bool,
) -> RuleID {
    let selector_text = if matches_state {
        ":state(target)"
    } else {
        ":not(:state(target))"
    };
    let program = engine
        .programs
        .add(test_class_selector_program(selector_text, &[("target", state)], None));

    let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn concrete_rule_match(
    engine: &StyleEngine,
    node: StyleNodeID,
    rule: RuleID,
    cascade_order: u32,
    pseudo_element: Option<PseudoElementTarget>,
) -> RuleMatch {
    RuleMatch {
        node,
        pseudo_element,
        rule,
        program: engine.program.rule_version(rule).selector_program.unwrap(),
        entry: 0,
        cascade_order,
        specificity: Specificity {
            classes: 1,
            ..Specificity::default()
        },
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: u32::MAX,
    }
}

fn add_pseudo_target_rule(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    target: StyleAtomID,
    pseudo: PseudoElementTarget,
) -> RuleID {
    let program = engine.programs.add(test_class_selector_program(
        ".target",
        &[("target", target)],
        Some(pseudo),
    ));
    let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    rule
}

fn commit_test_setup(engine: &mut StyleEngine) {
    discard_transaction(engine);
}

#[test]
fn cascade_matching_discards_rules_that_lose_every_property() {
    let (mut engine, nodes) = linear_document();
    let lower = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(lower, &[(1, false)], true);
    engine.set_rule_declared_properties(winner, &[(1, false)], true);
    commit_test_setup(&mut engine);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], lower, 0, None),
        concrete_rule_match(&engine, nodes[0], winner, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(compacted.len(), 1);
    assert_eq!(compacted[0].rule, winner);
}

#[test]
fn cascade_matching_publishes_the_same_top_1_winners_it_compacts() {
    let (mut engine, nodes) = linear_document();
    let lower = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let later = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(lower, &[(1, false), (2, false)], true);
    engine.set_rule_declared_properties(later, &[(1, false)], true);
    commit_test_setup(&mut engine);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], lower, 0, None),
        concrete_rule_match(&engine, nodes[0], later, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, Some(nodes[0]));

    assert_eq!(
        compacted.iter().map(|entry| entry.rule).collect::<Vec<_>>(),
        vec![lower, later]
    );
    let key = WinnerGroupKey::current(nodes[0], engine.program.version());
    assert!(
        matches!(engine.winner_groups.winner(key, 1), Lookup::Known(winner) if winner.source == WinnerSource::Rule(later))
    );
    assert!(
        matches!(engine.winner_groups.winner(key, 2), Lookup::Known(winner) if winner.source == WinnerSource::Rule(lower))
    );
    assert_eq!(engine.counters().get(Counter::CascadeNodeHandlesPublished), 1);
    assert_eq!(engine.counters().get(Counter::CascadeStatesInterned), 1);
    assert_eq!(engine.counters().get(Counter::CascadeWinnerGroupsInterned), 1);
    assert_eq!(engine.counters().get(Counter::CascadeWinnerEntriesInterned), 2);
}

#[test]
fn cascade_directed_matching_equals_compacting_the_exact_answer() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let lower = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(lower, &[(1, false)], true);
    engine.set_rule_declared_properties(winner, &[(1, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact = engine.match_element(nodes[1]).unwrap();
    let expected = engine.matches_for_cascade(exact, false, None);
    let rejected_before = engine.counters().get(Counter::CascadeCandidatesRejectedByWinner);
    let actual = engine.match_element_for_cascade(nodes[1]).unwrap();

    assert_eq!(actual, expected);
    assert_eq!(actual.len(), 1);
    assert_eq!(actual[0].rule, winner);
    assert_eq!(
        engine.counters().get(Counter::CascadeCandidatesRejectedByWinner) - rejected_before,
        1
    );
}

#[test]
fn an_incomplete_matching_rule_blocks_cascade_directed_pruning() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let lower = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    let incomplete = add_target_rule(&mut engine, StyleSheetObjectID(3), target);
    engine.set_rule_declared_properties(lower, &[(1, false)], true);
    engine.set_rule_declared_properties(winner, &[(1, false)], true);
    engine.set_rule_declared_properties(incomplete, &[(2, false)], false);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact = engine.match_element(nodes[1]).unwrap();
    let expected = engine.matches_for_cascade(exact, false, None);
    let rejected_before = engine.counters().get(Counter::CascadeCandidatesRejectedByWinner);
    let actual = engine.match_element_for_cascade(nodes[1]).unwrap();

    assert_eq!(actual, expected);
    assert_eq!(actual.len(), 3);
    assert_eq!(
        engine.counters().get(Counter::CascadeCandidatesRejectedByWinner),
        rejected_before
    );
}

#[test]
fn cascade_matching_keeps_rules_that_win_different_properties() {
    let (mut engine, nodes) = linear_document();
    let first = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let second = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(first, &[(1, false)], true);
    engine.set_rule_declared_properties(second, &[(2, false)], true);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], first, 0, None),
        concrete_rule_match(&engine, nodes[0], second, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(
        compacted.iter().map(|entry| entry.rule).collect::<Vec<_>>(),
        vec![first, second]
    );
}

#[test]
fn cascade_matching_accounts_for_important_declarations() {
    let (mut engine, nodes) = linear_document();
    let important = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let later = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(important, &[(1, true)], true);
    engine.set_rule_declared_properties(later, &[(1, false)], true);
    commit_test_setup(&mut engine);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], important, 0, None),
        concrete_rule_match(&engine, nodes[0], later, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(compacted.len(), 1);
    assert_eq!(compacted[0].rule, important);
}

#[test]
fn a_layer_reorder_patches_the_retained_compact_answer() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let base_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let theme_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(base_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(theme_rule, &[(1, false)], true);
    let base = CascadeLayerID(1);
    let theme = CascadeLayerID(2);
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[base, theme]);
    engine.set_rule_layer(base_rule, base);
    engine.set_rule_layer(theme_rule, theme);
    engine.set_rule_in_a_layer(base_rule);
    engine.set_rule_in_a_layer(theme_rule);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let old_answer = engine.matches_for_cascade(exact_answer.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &old_answer);
    assert_eq!(old_answer.len(), 1);
    assert_eq!(old_answer[0].rule, theme_rule);

    engine.set_layer_order(TreeScopeID::DOCUMENT, &[theme, base]);
    engine.record_layer_statement(engine.program.rule_sheet(base_rule));
    let patches_before = engine.counters().get(Counter::RetainedMatchAnswerDeltaPatches);
    let feature_tests_before = engine.counters().get(Counter::LocalFeatureTests);
    let selector_upqueries_before = engine.counters().get(Counter::SelectorTruthRepairUpqueries);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[1].raw()]);
    assert_eq!(
        engine.counters().get(Counter::RetainedMatchAnswerDeltaPatches),
        patches_before + 1
    );
    assert_eq!(engine.counters().get(Counter::LocalFeatureTests), feature_tests_before);
    assert_eq!(
        engine.counters().get(Counter::SelectorTruthRepairUpqueries),
        selector_upqueries_before
    );

    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    let new_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    engine.end_cold_matching_batch();
    assert_eq!(new_answer.len(), 1);
    assert_eq!(new_answer[0].rule, base_rule);
    assert_eq!(engine.counters().get(Counter::RetainedMatchAnswerReuses), 1);
}

#[test]
fn an_unused_layer_priority_shift_stops_before_recomputation() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let base_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let theme_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(base_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(theme_rule, &[(1, false)], true);
    let base = CascadeLayerID(1);
    let theme = CascadeLayerID(2);
    let unused = CascadeLayerID(3);
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[base, theme]);
    engine.set_rule_layer(base_rule, base);
    engine.set_rule_layer(theme_rule, theme);
    engine.set_rule_in_a_layer(base_rule);
    engine.set_rule_in_a_layer(theme_rule);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let old_answer = engine.matches_for_cascade(exact_answer.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &old_answer);
    assert_eq!(old_answer.len(), 1);
    assert_eq!(old_answer[0].rule, theme_rule);

    engine.set_layer_order(TreeScopeID::DOCUMENT, &[base, unused, theme]);
    engine.record_layer_statement(engine.program.rule_sheet(base_rule));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert!(planned.is_empty());

    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    let new_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    engine.end_cold_matching_batch();
    assert_eq!(new_answer.len(), 1);
    assert_eq!(new_answer[0].rule, theme_rule);
}

#[test]
fn an_evicted_retained_match_answer_falls_back_to_cold_matching() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let base_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let theme_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(base_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(theme_rule, &[(1, false)], true);
    let base = CascadeLayerID(1);
    let theme = CascadeLayerID(2);
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[base, theme]);
    engine.set_rule_layer(base_rule, base);
    engine.set_rule_layer(theme_rule, theme);
    engine.set_rule_in_a_layer(base_rule);
    engine.set_rule_in_a_layer(theme_rule);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, None);
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    engine.retained_match_answers.evict(&mut engine.match_answers);
    assert!(matches!(
        engine.retained_match_answer(nodes[1]),
        Lookup::Missing(gap) if gap == nodes[1]
    ));
    assert!(matches!(
        engine.retained_match_answers.cascade_input_lookup(nodes[1]),
        Lookup::Known(cascade_input) if *cascade_input != CascadeInputID::default()
    ));

    engine.set_layer_order(TreeScopeID::DOCUMENT, &[theme, base]);
    engine.record_layer_statement(engine.program.rule_sheet(base_rule));
    let misses_before = engine.counters().get(Counter::RetainedMatchAnswerPatchMisses);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert_eq!(planned, vec![nodes[1].raw()]);
    assert_eq!(
        engine.counters().get(Counter::RetainedMatchAnswerPatchMisses),
        misses_before + 1
    );

    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    let new_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    engine.end_cold_matching_batch();
    assert_eq!(new_answer.len(), 1);
    assert_eq!(new_answer[0].rule, base_rule);
    assert_eq!(
        engine
            .counters()
            .get(Counter::MatchElementCallsDuringPublishedStyleTransaction),
        0
    );
}

#[test]
fn an_evicted_answer_payload_repairs_to_its_retained_identity() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let losing_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    let winning_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(losing_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(winning_rule, &[(1, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, None);
    assert_eq!(compact_answer.len(), 1);
    assert_eq!(compact_answer[0].rule, winning_rule);
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    engine.retained_match_answers.evict(&mut engine.match_answers);

    engine.set_rule_conditions_hold(losing_rule, false);
    let repairs_before = engine.counters().get(Counter::PublishedMatchAnswerIdentityRepairs);
    let stops_before = engine.counters().get(Counter::PublishedMatchAnswerIdentityRepairStops);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert!(planned.is_empty());
    assert_eq!(
        engine.counters().get(Counter::PublishedMatchAnswerIdentityRepairs),
        repairs_before + 1
    );
    assert_eq!(
        engine.counters().get(Counter::PublishedMatchAnswerIdentityRepairStops),
        stops_before + 1
    );
}

#[test]
fn an_exact_unchanged_cascade_stops_before_style_recomputation() {
    let (mut engine, nodes) = linear_document();
    let first_class = StyleAtomID(200);
    let second_class = StyleAtomID(201);
    let first_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), first_class);
    let second_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), second_class);
    let value = SpecifiedValueID(101);
    engine.set_rule_declared_properties_with_values(first_rule, &[(1, false, value)], true);
    engine.set_rule_declared_properties_with_values(second_rule, &[(1, false, value)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(first_class));
    discard_transaction(&mut engine);

    let old_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert_eq!(old_answer.len(), 1);
    assert_eq!(old_answer[0].rule, first_rule);
    publish_current_cascade_as_computed(&mut engine, nodes[1]);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(first_class));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(second_class));
    let stops_before = engine.counters().get(Counter::PublishedExactCascadeStops);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));

    assert!(planned.is_empty());
    assert_eq!(
        engine.counters().get(Counter::PublishedExactCascadeStops),
        stops_before + 1
    );
    let current = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert_eq!(current.len(), 1);
    assert_eq!(current[0].rule, second_rule);
}

#[test]
fn a_changed_exact_cascade_is_still_published_for_recomputation() {
    let (mut engine, nodes) = linear_document();
    let first_class = StyleAtomID(200);
    let second_class = StyleAtomID(201);
    let first_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), first_class);
    let second_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), second_class);
    engine.set_rule_declared_properties_with_values(first_rule, &[(1, false, SpecifiedValueID(101))], true);
    engine.set_rule_declared_properties_with_values(second_rule, &[(1, false, SpecifiedValueID(102))], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(first_class));
    discard_transaction(&mut engine);
    engine.match_element_for_cascade(nodes[1]).unwrap();
    publish_current_cascade_as_computed(&mut engine, nodes[1]);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(first_class));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(second_class));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));

    assert_eq!(planned, vec![nodes[1].raw()]);
}

#[test]
fn program_and_local_routes_merge_retained_answer_attribution() {
    let (mut engine, nodes) = linear_document();
    let departing_class = StyleAtomID(200);
    let arriving_class = StyleAtomID(201);
    let departing_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), departing_class);
    engine.set_rule_declared_properties(departing_rule, &[(1, false)], true);
    for class in [departing_class, arriving_class] {
        add_feature(&mut engine, nodes[1], FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let old_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert_eq!(old_answer.len(), 1);
    assert_eq!(old_answer[0].rule, departing_rule);
    publish_current_cascade_as_computed(&mut engine, nodes[1]);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(departing_class));

    let program = engine.programs.add(test_class_selector_program(
        ".arriving",
        &[("arriving", arriving_class)],
        None,
    ));
    let sheet = engine.program.rule_sheet(departing_rule);
    let arriving_rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(arriving_rule, program);
    let mut version = engine.program.rule_version(arriving_rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(2));
    engine.replace_rule_version(arriving_rule, version);
    engine.set_rule_declared_properties(arriving_rule, &[(2, false)], true);

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));

    assert_eq!(planned, vec![nodes[1].raw()]);
    let retained = match engine.retained_match_answer(nodes[1]) {
        Lookup::Known(answer) => answer,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a retained answer"),
    };
    assert_eq!(retained.len(), 1);
    assert_eq!(retained[0].rule, arriving_rule);
}

#[test]
fn an_exact_unchanged_custom_state_cascade_stops_before_style_recomputation() {
    let (mut engine, nodes) = linear_document();
    let state = StyleAtomID(200);
    let first_rule = add_custom_state_rule(&mut engine, StyleSheetObjectID(1), state, false);
    let second_rule = add_custom_state_rule(&mut engine, StyleSheetObjectID(2), state, true);
    let value = SpecifiedValueID(101);
    engine.set_rule_declared_properties_with_values(first_rule, &[(1, false, value)], true);
    engine.set_rule_declared_properties_with_values(second_rule, &[(1, false, value)], true);
    discard_transaction(&mut engine);
    engine.facts.set_custom_states(nodes[1], &[]);
    engine.facts.commit_pending(&mut engine.memory);

    let old_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert_eq!(old_answer.len(), 1);
    assert_eq!(old_answer[0].rule, first_rule);
    publish_current_cascade_as_computed(&mut engine, nodes[1]);

    engine.set_element_custom_states(nodes[1], &[state]);
    let stops_before = engine.counters().get(Counter::PublishedExactCascadeStops);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));

    assert!(planned.is_empty());
    assert_eq!(
        engine.counters().get(Counter::PublishedExactCascadeStops),
        stops_before + 1
    );
    let current = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert_eq!(current.len(), 1);
    assert_eq!(current[0].rule, second_rule);
}

#[test]
fn retained_answer_patching_evaluates_narrow_affected_rules_directly() {
    let (mut engine, nodes) = linear_document();
    let matching_class = StyleAtomID(200);
    let unrelated_class = StyleAtomID(201);
    let matching_rule = add_target_rule(&mut engine, StyleSheetObjectID(1), matching_class);
    let unrelated_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), unrelated_class);
    engine.set_rule_declared_properties(matching_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(unrelated_rule, &[(2, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(matching_class));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, None);
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    let mut patch = engine.prepare_retained_answer_patch(RetainedAnswerPatchSelection {
        affected: vec![
            RetainedAnswerPatchSelectionRule {
                rule: matching_rule,
                program: engine.program.rule_version(matching_rule).selector_program.unwrap(),
                evaluate: true,
            },
            RetainedAnswerPatchSelectionRule {
                rule: unrelated_rule,
                program: engine.program.rule_version(unrelated_rule).selector_program.unwrap(),
                evaluate: true,
            },
        ],
        always_emit: false,
        orders_shifted: false,
        requires_full_match: false,
        ..Default::default()
    });
    let feature_tests_before = engine.counters().get(Counter::LocalFeatureTests);

    assert_eq!(
        engine
            .patch_retained_match_answer(nodes[1], &mut patch, SelectorTruthPatch::Full)
            .map(|outcome| outcome.emit),
        Some(false)
    );
    // The narrow repair path filters out the unrelated rule before selector evaluation without
    // paying the fixed cost to gather and filter the node's subject dispatch buckets.
    assert_eq!(
        engine.counters().get(Counter::LocalFeatureTests),
        feature_tests_before + 1
    );
}

#[test]
fn retained_answer_patching_applies_complete_signed_deltas_without_matching() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    remove_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    let program = engine.program.rule_version(rule).selector_program.unwrap();
    let mut patch = engine.prepare_retained_answer_patch(RetainedAnswerPatchSelection {
        affected: vec![RetainedAnswerPatchSelectionRule {
            rule,
            program,
            evaluate: true,
        }],
        always_emit: false,
        orders_shifted: false,
        requires_full_match: false,
        ..Default::default()
    });
    let feature_tests_before = engine.counters().get(Counter::LocalFeatureTests);

    let outcome = engine
        .patch_retained_match_answer(
            nodes[1],
            &mut patch,
            SelectorTruthPatch::Direct(&[SelectorTruthDelta {
                node: nodes[1],
                rule,
                program,
                entry: 0,
                change: SetChange::Removed,
                selector_truth_changed: true,
            }]),
        )
        .unwrap();
    assert!(outcome.emit);
    assert!(outcome.incremental_cascade_answer.is_some());
    assert_eq!(engine.counters().get(Counter::LocalFeatureTests), feature_tests_before);
    assert_eq!(engine.counters().get(Counter::RetainedMatchAnswerDeltaPatches), 1);
    assert_eq!(engine.counters().get(Counter::RetainedMatchAnswerDeltaEntries), 1);
    assert!(matches!(engine.retained_match_answer(nodes[1]), Lookup::Known(answer) if answer.is_empty()));

    assert!(engine.apply_cascade_winner_match_deltas(
        nodes[1],
        &exact_answer,
        &[SelectorTruthDelta {
            node: nodes[1],
            rule,
            program,
            entry: 0,
            change: SetChange::Added,
            selector_truth_changed: true,
        }],
        &mut Vec::new(),
    ));
    assert!(matches!(
        engine
            .winner_groups
            .winner(WinnerGroupKey::current(nodes[1], engine.program.version()), 1),
        Lookup::Known(winner) if winner.source == WinnerSource::Rule(rule)
    ));
}

#[test]
fn retained_answer_patching_matches_only_unresolved_rules_after_signed_deltas() {
    let (mut engine, nodes) = linear_document();
    let delta_target = StyleAtomID(200);
    let second_delta_target = StyleAtomID(201);
    let refresh_target = StyleAtomID(202);
    let mut add_rule = |target, sheet_object| {
        let program = engine
            .programs
            .add(test_selector_program(".target:nth-child(2n+1)", &[("target", target)]));
        let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
        engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        let rule = engine.append_rule(sheet, None, RuleKind::Style);
        engine.add_routing_rule(rule, program);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(program);
        version.declaration_block = Some(DeclarationBlockID(1));
        engine.replace_rule_version(rule, version);
        (rule, program)
    };
    let (delta_rule, delta_program) = add_rule(delta_target, StyleSheetObjectID(1));
    let (second_delta_rule, second_delta_program) = add_rule(second_delta_target, StyleSheetObjectID(2));
    let (refresh_rule, refresh_program) = add_rule(refresh_target, StyleSheetObjectID(3));
    engine.set_rule_declared_properties(delta_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(second_delta_rule, &[(2, false)], true);
    engine.set_rule_declared_properties(refresh_rule, &[(3, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(refresh_target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, None);
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(delta_target));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(second_delta_target));
    engine.facts.commit_pending(&mut engine.memory);
    let mut patch = engine.prepare_retained_answer_patch(RetainedAnswerPatchSelection {
        affected: vec![
            RetainedAnswerPatchSelectionRule {
                rule: delta_rule,
                program: delta_program,
                evaluate: true,
            },
            RetainedAnswerPatchSelectionRule {
                rule: second_delta_rule,
                program: second_delta_program,
                evaluate: true,
            },
            RetainedAnswerPatchSelectionRule {
                rule: refresh_rule,
                program: refresh_program,
                evaluate: true,
            },
        ],
        always_emit: false,
        orders_shifted: false,
        requires_full_match: false,
        ..Default::default()
    });
    let candidate_checks_before = engine.counters().get(Counter::CandidateChecks);

    assert_eq!(
        engine
            .patch_retained_match_answer(
                nodes[1],
                &mut patch,
                SelectorTruthPatch::Refresh {
                    deltas: &[
                        SelectorTruthDelta {
                            node: nodes[1],
                            rule: delta_rule,
                            program: delta_program,
                            entry: 0,
                            change: SetChange::Added,
                            selector_truth_changed: true,
                        },
                        SelectorTruthDelta {
                            node: nodes[1],
                            rule: second_delta_rule,
                            program: second_delta_program,
                            entry: 0,
                            change: SetChange::Added,
                            selector_truth_changed: true,
                        },
                    ],
                    refreshes: &[SelectorTruthRefresh {
                        node: nodes[1],
                        rule: Some((refresh_rule, refresh_program)),
                    }],
                },
            )
            .map(|outcome| outcome.emit),
        Some(true)
    );
    assert_eq!(
        engine.counters().get(Counter::CandidateChecks),
        candidate_checks_before + 1
    );
    assert!(matches!(
        engine.retained_match_answer(nodes[1]),
        Lookup::Known(answer) if answer.len() == 3
    ));
}

#[test]
fn retained_answer_patching_preserves_incomplete_cascade_winners() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let winning_target = StyleAtomID(201);
    let pseudo = PseudoElementTarget::new(PseudoElementKind(0));
    let gated_rule = add_pseudo_target_rule(&mut engine, StyleSheetObjectID(1), target, pseudo);
    let winning_rule = add_target_rule(&mut engine, StyleSheetObjectID(2), winning_target);
    engine.set_rule_declared_properties(gated_rule, &[(1, false)], true);
    engine.set_rule_declared_properties(winning_rule, &[(2, false)], true);
    engine.set_rule_gated_by_container_query(gated_rule);
    for class in [target, winning_target] {
        add_feature(&mut engine, nodes[1], FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    assert_eq!(exact_answer.len(), 2);
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    let winning_program = engine.program.rule_version(winning_rule).selector_program.unwrap();
    let mut patch = engine.prepare_retained_answer_patch(RetainedAnswerPatchSelection {
        affected: vec![RetainedAnswerPatchSelectionRule {
            rule: winning_rule,
            program: winning_program,
            evaluate: true,
        }],
        always_emit: false,
        orders_shifted: false,
        requires_full_match: false,
        ..Default::default()
    });

    let outcome = engine
        .patch_retained_match_answer(
            nodes[1],
            &mut patch,
            SelectorTruthPatch::Direct(&[SelectorTruthDelta {
                node: nodes[1],
                rule: winning_rule,
                program: winning_program,
                entry: 0,
                change: SetChange::Removed,
                selector_truth_changed: true,
            }]),
        )
        .unwrap();
    let incremental = outcome.incremental_cascade_answer.unwrap();
    assert!(!incremental.cascade_winners_are_complete);
}

#[test]
fn selector_list_entry_deltas_fall_back_when_the_compact_winner_is_insufficient() {
    let (mut engine, nodes) = linear_document();
    let (rule, program) = add_selector_list_rule(&mut engine, StyleAtomID(200), StyleAtomID(201));
    let mut patch = engine.prepare_retained_answer_patch(RetainedAnswerPatchSelection {
        affected: vec![RetainedAnswerPatchSelectionRule {
            rule,
            program,
            evaluate: true,
        }],
        always_emit: false,
        orders_shifted: false,
        requires_full_match: false,
        ..Default::default()
    });
    let retained = [RetainedRuleMatch {
        rule,
        program,
        entry: 1,
        tree_scope: TreeScopeID::DOCUMENT,
        scope_proximity: u32::MAX,
    }];
    let delta = |entry, change| SelectorTruthDelta {
        node: nodes[1],
        rule,
        program,
        entry,
        change,
        selector_truth_changed: true,
    };

    assert!(
        engine
            .apply_retained_match_answer_deltas(
                nodes[1],
                &mut patch,
                MatchAnswerID::default(),
                &retained,
                CascadeInputID::default(),
                &[delta(0, SetChange::Added)],
            )
            .is_none(),
        "a newly matching hidden loser needs rule-scoped repair"
    );
    assert!(
        engine
            .apply_retained_match_answer_deltas(
                nodes[1],
                &mut patch,
                MatchAnswerID::default(),
                &retained,
                CascadeInputID::default(),
                &[delta(1, SetChange::Removed)],
            )
            .is_none(),
        "removing a winner must recover any hidden matching loser"
    );
    engine.prefix_caches.borrow_mut().states.release();
}

#[test]
fn retained_answer_repair_returns_signed_selector_truth() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, None);
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);
    remove_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);
    let program = engine.program.rule_version(rule).selector_program.unwrap();
    let mut patch = engine.prepare_retained_answer_patch(RetainedAnswerPatchSelection {
        affected: vec![RetainedAnswerPatchSelectionRule {
            rule,
            program,
            evaluate: true,
        }],
        always_emit: false,
        orders_shifted: false,
        requires_full_match: false,
        ..Default::default()
    });
    let repair_upqueries_before = engine.counters().get(Counter::SelectorTruthRepairUpqueries);
    let repair_removals_before = engine.counters().get(Counter::SelectorTruthRepairRemovals);
    let delta_patches_before = engine.counters().get(Counter::RetainedMatchAnswerDeltaPatches);

    assert_eq!(
        engine
            .patch_retained_match_answer(
                nodes[1],
                &mut patch,
                SelectorTruthPatch::Refresh {
                    deltas: &[],
                    refreshes: &[SelectorTruthRefresh {
                        node: nodes[1],
                        rule: Some((rule, program)),
                    }],
                },
            )
            .map(|outcome| outcome.emit),
        Some(true)
    );
    assert_eq!(
        engine.counters().get(Counter::SelectorTruthRepairUpqueries),
        repair_upqueries_before + 1
    );
    assert_eq!(
        engine.counters().get(Counter::SelectorTruthRepairRemovals),
        repair_removals_before + 1
    );
    assert_eq!(
        engine.counters().get(Counter::RetainedMatchAnswerDeltaPatches),
        delta_patches_before + 1
    );
    assert!(matches!(engine.retained_match_answer(nodes[1]), Lookup::Known(answer) if answer.is_empty()));
}

#[test]
fn already_planned_routes_attribute_their_extent() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[3]).unwrap();
    engine.remember_retained_match_answer(nodes[3], &exact_answer);
    engine.remember_cascade_input(nodes[3], &exact_answer);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut transaction = engine.take_transaction();
    let mut regions = ImpactRegions::with_topology(&engine.tree, nodes[0]);
    regions.add(ImpactRegion::Node(nodes[3]), &mut engine.counters);
    engine.transaction_fact_view = Some(engine.transaction_fact_view_for(&mut transaction, nodes[0], &regions));
    engine.selector_truth_changes_active = true;
    let program = engine.program.rule_version(rule).selector_program.unwrap();
    let site = RoutingSite {
        subject: &[DispatchKey::Class(target)],
        subject_required: &[],
        position: SubjectPosition::UNBOUNDED,
        path: &[],
        waypoints: &[],
        in_flux: None,
        exact_entry: Some((rule, program, 0)),
        exact_tree_evaluation: None,
        refresh_rule: None,
    };
    engine.add_narrowed_region(ImpactRegion::Node(nodes[3]), &site, &mut regions);

    // The route leaves no per-node row behind: its extent attribution alone must carry the
    // rule into the node's patch union.
    assert!(engine.already_planned_selector_truth.as_slice().is_empty());
    engine.resolve_already_planned_selector_truth(&regions, None);
    engine.selector_truth_changes.consolidate(&mut engine.counters);
    assert!(engine.selector_truth_changes.deltas.as_slice().is_empty());
    assert!(engine.selector_truth_changes.refreshes.as_slice().is_empty());

    let cover = regions.compile_patch_cover(&engine.tree, Some(nodes[0]));
    let mut sweep = AttributionSweep::default();
    let mut covering = Vec::new();
    assert!(regions.covering_attributions(&cover, &mut sweep, nodes[3], &mut covering));
    assert_eq!(covering, vec![(rule, program)]);

    engine.selector_truth_changes = SelectorTruthChanges::default();
    engine.record_already_planned_selector_truth(nodes[3], &site);
    let mut coarse_regions = ImpactRegions::new();
    coarse_regions.add(ImpactRegion::Subtree(nodes[0]), &mut engine.counters);
    let coarse_cover = coarse_regions.compile_union(coarse_regions.regions(), &engine.tree, Some(nodes[0]));
    engine.resolve_already_planned_selector_truth(&coarse_regions, Some(&coarse_cover));
    assert!(engine.selector_truth_changes.deltas.as_slice().is_empty());
    assert!(engine.selector_truth_changes.refreshes.as_slice().is_empty());
    engine.transaction_fact_view = None;
    engine.release_transaction(transaction);
}

#[test]
fn routes_covered_by_an_attributed_subtree_still_attribute_their_extent() {
    let (mut engine, nodes) = nested_document();
    let rule_a = add_guard_target_rule(&mut engine, StyleAtomID(200), StyleAtomID(201));
    let rule_b = add_guard_target_rule(&mut engine, StyleAtomID(202), StyleAtomID(203));
    let program_a = engine.program.rule_version(rule_a).selector_program.unwrap();
    let program_b = engine.program.rule_version(rule_b).selector_program.unwrap();
    discard_transaction(&mut engine);
    let mut regions = ImpactRegions::with_topology(&engine.tree, nodes[0]);
    regions.add_attributed(
        ImpactRegion::Subtree(nodes[0]),
        (rule_a, program_a),
        &mut engine.counters,
    );
    engine.selector_truth_changes_active = true;

    // A covering subtree attributed to one rule narrows the patches of every node under it,
    // so a second route dropped for that coverage must still enter the attribution union.
    let site = RoutingSite {
        subject: &[DispatchKey::Class(StyleAtomID(203))],
        subject_required: &[],
        position: SubjectPosition::UNBOUNDED,
        path: &[],
        waypoints: &[],
        in_flux: None,
        exact_entry: Some((rule_b, program_b, 0)),
        exact_tree_evaluation: None,
        refresh_rule: None,
    };
    let mut routed_regions = vec![ImpactRegion::Node(nodes[3])];
    engine.discard_regions_covered_by_subtree(&mut routed_regions, &site, &mut regions);
    assert!(routed_regions.is_empty());

    let cover = regions.compile_patch_cover(&engine.tree, Some(nodes[0]));
    assert!(!regions.batch_contains_node(&cover.full, nodes[3]));
    let mut sweep = AttributionSweep::default();
    let mut covering = Vec::new();
    assert!(regions.covering_attributions(&cover, &mut sweep, nodes[3], &mut covering));
    assert_eq!(covering, vec![(rule_a, program_a), (rule_b, program_b)]);

    // A dropped route that names no rule must poison instead: a node region by refresh, a
    // wider region by joining the full re-derivation cover.
    let unnamed = RoutingSite {
        exact_entry: None,
        ..site
    };
    let mut node_region = vec![ImpactRegion::Node(nodes[3])];
    engine.discard_regions_covered_by_subtree(&mut node_region, &unnamed, &mut regions);
    assert!(node_region.is_empty());
    assert!(
        engine
            .selector_truth_changes
            .refreshes
            .as_slice()
            .iter()
            .any(|refresh| refresh.node == nodes[3] && refresh.rule.is_none())
    );
    let mut wide_region = vec![ImpactRegion::Children(nodes[1])];
    engine.discard_regions_covered_by_subtree(&mut wide_region, &unnamed, &mut regions);
    assert!(wide_region.is_empty());
    let poisoned_cover = regions.compile_patch_cover(&engine.tree, Some(nodes[0]));
    assert!(regions.batch_contains_node(&poisoned_cover.full, nodes[2]));

    // Coverage by an unattributed subtree already forces full re-derivation, so a route
    // dropped for that coverage stays silent: no attribution and no poison.
    let mut full_regions = ImpactRegions::with_topology(&engine.tree, nodes[0]);
    full_regions.add(ImpactRegion::Subtree(nodes[0]), &mut engine.counters);
    let refreshes_before = engine.selector_truth_changes.refreshes.as_slice().len();
    let mut silent_region = vec![ImpactRegion::Node(nodes[3])];
    engine.discard_regions_covered_by_subtree(&mut silent_region, &unnamed, &mut full_regions);
    assert!(silent_region.is_empty());
    assert_eq!(
        engine.selector_truth_changes.refreshes.as_slice().len(),
        refreshes_before
    );
}

#[test]
fn already_planned_routes_skip_truth_without_a_retained_answer() {
    let (mut engine, nodes) = nested_document();
    let rule = add_guard_target_rule(&mut engine, StyleAtomID(200), StyleAtomID(201));
    let program = engine.program.rule_version(rule).selector_program.unwrap();
    let site = RoutingSite {
        subject: &[],
        subject_required: &[],
        position: SubjectPosition::UNBOUNDED,
        path: &[],
        waypoints: &[],
        in_flux: None,
        exact_entry: Some((rule, program, 0)),
        exact_tree_evaluation: None,
        refresh_rule: None,
    };
    engine.selector_truth_changes_active = true;

    engine.record_already_planned_selector_truth(nodes[3], &site);

    assert!(engine.already_planned_selector_truth.as_slice().is_empty());
}

#[test]
fn cascade_matching_refuses_incomplete_declaration_inventories() {
    let (mut engine, nodes) = linear_document();
    let incomplete = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(incomplete, &[(1, false)], false);
    engine.set_rule_declared_properties(winner, &[(1, false)], true);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], incomplete, 0, None),
        concrete_rule_match(&engine, nodes[0], winner, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(compacted.len(), 2);
}

#[test]
fn cascade_matching_refuses_non_document_scopes() {
    let (mut engine, nodes) = linear_document();
    let lower = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(lower, &[(1, false)], true);
    engine.set_rule_declared_properties(winner, &[(1, false)], true);
    let mut shadow_match = concrete_rule_match(&engine, nodes[0], lower, 0, None);
    shadow_match.tree_scope = TreeScopeID(1);
    let matches = vec![shadow_match, concrete_rule_match(&engine, nodes[0], winner, 1, None)];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(compacted.len(), 2);
}

#[test]
fn cascade_matching_preserves_non_author_rules() {
    let (mut engine, nodes) = linear_document();
    let user_agent = add_target_rule_with_origin(
        &mut engine,
        StyleSheetObjectID(1),
        StyleAtomID(200),
        CascadeOrigin::UserAgent,
    );
    let author = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(user_agent, &[(1, false)], true);
    engine.set_rule_declared_properties(author, &[(1, false)], true);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], user_agent, 0, None),
        concrete_rule_match(&engine, nodes[0], author, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(compacted.len(), 2);
}

#[test]
fn cascade_state_includes_non_author_origins() {
    let (mut engine, nodes) = linear_document();
    let user_agent = add_target_rule_with_origin(
        &mut engine,
        StyleSheetObjectID(1),
        StyleAtomID(200),
        CascadeOrigin::UserAgent,
    );
    let author = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(user_agent, &[(1, true)], true);
    engine.set_rule_declared_properties(author, &[(1, false)], true);
    commit_test_setup(&mut engine);
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], user_agent, 0, None),
        concrete_rule_match(&engine, nodes[0], author, 1, None),
    ];

    let compacted = engine.matches_for_cascade(matches, false, Some(nodes[0]));

    assert_eq!(compacted.len(), 1);
    assert_eq!(compacted[0].rule, user_agent);
    let key = WinnerGroupKey::current(nodes[0], engine.program.version());
    assert!(
        matches!(engine.winner_groups.winner(key, 1), Lookup::Known(winner) if winner.source == WinnerSource::Rule(user_agent))
    );
}

#[test]
fn author_revert_retains_and_repairs_its_user_agent_continuation() {
    let (mut engine, nodes) = linear_document();
    let user_agent = add_target_rule_with_origin(
        &mut engine,
        StyleSheetObjectID(1),
        StyleAtomID(200),
        CascadeOrigin::UserAgent,
    );
    let author_value = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    let author_revert = add_target_rule(&mut engine, StyleSheetObjectID(3), StyleAtomID(202));
    let declared = |value, operator| DeclaredProperty {
        property: 1,
        important: false,
        operator,
        value: SpecifiedValueID(value),
    };
    engine.set_rule_declared_properties_with_operators(user_agent, &[declared(101, CascadeOperator::Declared)], true);
    engine.set_rule_declared_properties_with_operators(author_value, &[declared(201, CascadeOperator::Declared)], true);
    engine.set_rule_declared_properties_with_operators(author_revert, &[declared(301, CascadeOperator::Revert)], true);
    commit_test_setup(&mut engine);
    let user_agent_match = concrete_rule_match(&engine, nodes[0], user_agent, 0, None);
    let author_value_match = concrete_rule_match(&engine, nodes[0], author_value, 1, None);
    let author_revert_match = concrete_rule_match(&engine, nodes[0], author_revert, 2, None);
    let matches = vec![user_agent_match, author_value_match, author_revert_match];

    let compacted = engine.matches_for_cascade(matches, false, Some(nodes[0]));

    assert_eq!(
        compacted.iter().map(|matched| matched.rule).collect::<Vec<_>>(),
        [user_agent, author_revert]
    );
    let key = WinnerGroupKey::current(nodes[0], engine.program.version());
    let Lookup::Known(winner) = engine.winner_groups.winner(key, 1) else {
        panic!("revert winner was not retained");
    };
    assert_eq!(winner.source, WinnerSource::Rule(author_revert));
    assert_eq!(winner.key.operator, CascadeOperator::Revert);
    assert_eq!(
        engine.winner_groups.resolved_winner(winner).unwrap().source,
        WinnerSource::Rule(user_agent)
    );

    let removed = SelectorTruthDelta {
        node: nodes[0],
        rule: user_agent,
        program: user_agent_match.program,
        entry: user_agent_match.entry,
        change: SetChange::Removed,
        selector_truth_changed: true,
    };
    assert!(engine.apply_cascade_winner_match_deltas(
        nodes[0],
        &[author_value_match, author_revert_match],
        &[removed],
        &mut Vec::new()
    ));
    let Lookup::Known(repaired) = engine.winner_groups.winner(key, 1) else {
        panic!("revert winner was not repaired");
    };
    assert_eq!(repaired.source, WinnerSource::Rule(author_revert));
    assert!(engine.winner_groups.resolved_winner(repaired).is_none());
}

#[test]
fn exact_cascade_winner_repair_reduces_only_requested_properties() {
    let (mut engine, nodes) = linear_document();
    let lower = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let higher = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties_with_values(
        lower,
        &[(1, false, SpecifiedValueID(101)), (2, false, SpecifiedValueID(102))],
        true,
    );
    engine.set_rule_declared_properties_with_values(higher, &[(1, false, SpecifiedValueID(201))], true);
    commit_test_setup(&mut engine);
    let lower_match = concrete_rule_match(&engine, nodes[0], lower, 0, None);
    let higher_match = concrete_rule_match(&engine, nodes[0], higher, 1, None);

    let updates = engine
        .exact_cascade_winner_updates_for_properties(nodes[0], &[lower_match, higher_match], None, &[1, 2, 3])
        .unwrap();
    assert_eq!(updates.len(), 3);
    assert_eq!(updates[0].winner.unwrap().source, WinnerSource::Rule(higher));
    assert_eq!(updates[1].winner.unwrap().source, WinnerSource::Rule(lower));
    assert!(updates[2].winner.is_none());

    let repaired = engine
        .exact_cascade_winner_updates_for_properties(nodes[0], &[lower_match], None, &[1])
        .unwrap();
    assert_eq!(repaired[0].winner.unwrap().source, WinnerSource::Rule(lower));
}

#[test]
fn cascade_state_includes_exact_element_declarations() {
    let (mut engine, nodes) = linear_document();
    let author = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    engine.set_rule_declared_properties(author, &[(1, false), (2, false)], true);
    engine.set_element_declared_properties(
        nodes[0],
        ElementDeclarationKind::PresentationalHint,
        &[
            DeclaredProperty {
                property: 1,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(101),
            },
            DeclaredProperty {
                property: 3,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(103),
            },
        ],
        true,
    );
    engine.set_element_declared_properties(
        nodes[0],
        ElementDeclarationKind::InlineStyle,
        &[DeclaredProperty {
            property: 2,
            important: false,
            operator: CascadeOperator::Declared,
            value: SpecifiedValueID(102),
        }],
        true,
    );
    commit_test_setup(&mut engine);
    let matches = vec![concrete_rule_match(&engine, nodes[0], author, 0, None)];

    engine.matches_for_cascade(matches, false, Some(nodes[0]));

    let key = WinnerGroupKey::current(nodes[0], engine.program.version());
    assert!(
        matches!(engine.winner_groups.winner(key, 1), Lookup::Known(winner) if winner.source == WinnerSource::Rule(author))
    );
    assert!(
        matches!(engine.winner_groups.winner(key, 2), Lookup::Known(winner) if winner.source == WinnerSource::Element(ElementDeclarationKind::InlineStyle) && winner.key.value == SpecifiedValueID(102))
    );
    assert!(
        matches!(engine.winner_groups.winner(key, 3), Lookup::Known(winner) if winner.source == WinnerSource::Element(ElementDeclarationKind::PresentationalHint) && winner.key.value == SpecifiedValueID(103))
    );
}

#[test]
fn element_declaration_edits_repair_only_their_property_inventory() {
    let (mut engine, nodes) = linear_document();
    let author = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    engine.set_rule_declared_properties_with_values(
        author,
        &[(1, false, SpecifiedValueID(101)), (2, false, SpecifiedValueID(102))],
        true,
    );
    engine.set_element_declared_properties(
        nodes[0],
        ElementDeclarationKind::PresentationalHint,
        &[
            DeclaredProperty {
                property: 1,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(201),
            },
            DeclaredProperty {
                property: 3,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(203),
            },
        ],
        true,
    );
    commit_test_setup(&mut engine);
    let matches = vec![concrete_rule_match(&engine, nodes[0], author, 0, None)];
    engine.matches_for_cascade(matches.clone(), false, Some(nodes[0]));
    engine.remember_retained_match_answer(nodes[0], &matches);

    engine.set_element_declared_properties(
        nodes[0],
        ElementDeclarationKind::PresentationalHint,
        &[
            DeclaredProperty {
                property: 1,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(301),
            },
            DeclaredProperty {
                property: 3,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(303),
            },
        ],
        true,
    );

    let key = WinnerGroupKey::current(nodes[0], engine.program.version());
    assert!(
        matches!(engine.winner_groups.winner(key, 1), Lookup::Known(winner) if winner.source == WinnerSource::Rule(author) && winner.key.value == SpecifiedValueID(101))
    );
    assert!(
        matches!(engine.winner_groups.winner(key, 3), Lookup::Known(winner) if winner.source == WinnerSource::Element(ElementDeclarationKind::PresentationalHint) && winner.key.value == SpecifiedValueID(303))
    );
}

#[test]
fn rule_declaration_edits_repair_only_their_property_inventory() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    engine.set_rule_declared_properties_with_values(rule, &[(1, false, SpecifiedValueID(101))], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);

    engine.set_rule_declared_properties_with_values(rule, &[(2, false, SpecifiedValueID(202))], true);
    let mut version = engine.program.rule_version(rule);
    version.declaration_block = Some(DeclarationBlockID(2));
    engine.replace_rule_version(rule, version);
    assert_eq!(engine.pending_rule_declaration_changes.len(), 1);
    assert_eq!(engine.pending_rule_declaration_changes[0].old_properties, [1]);
    assert_eq!(engine.pending_rule_declaration_changes[0].new_properties, [2]);
    assert_eq!(engine.program.declared_properties_of(rule)[0].property, 1);
    assert_eq!(engine.current_declared_properties_of(rule)[0].property, 2);
    assert!(engine.pending_program_base_version.is_some());
    let feature_tests_before = engine.counters().get(Counter::LocalFeatureTests);

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, answers| {
        planned.extend(answers.iter().map(|answer| answer.style_node));
    }));
    assert_eq!(planned, vec![nodes[1].raw()]);
    assert_eq!(engine.program.declared_properties_of(rule)[0].property, 2);
    assert_eq!(engine.counters().get(Counter::LocalFeatureTests), feature_tests_before);
    let key = WinnerGroupKey::current(nodes[1], engine.program.version());
    assert!(matches!(engine.winner_groups.winner(key, 1), Lookup::KnownAbsent));
    assert!(
        matches!(engine.winner_groups.winner(key, 2), Lookup::Known(winner) if winner.source == WinnerSource::Rule(rule) && winner.key.value == SpecifiedValueID(202))
    );
    engine.discard_style_transaction_outputs();
}

#[test]
fn rule_declaration_repair_falls_back_when_winner_retention_is_refused() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    engine.set_rule_declared_properties_with_values(rule, &[(1, false, SpecifiedValueID(101))], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let exact_answer = engine.match_element(nodes[1]).unwrap();
    let compact_answer = engine.matches_for_cascade(exact_answer.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact_answer);
    engine.remember_cascade_input(nodes[1], &compact_answer);

    engine.set_rule_declared_properties_with_values(rule, &[(2, false, SpecifiedValueID(202))], true);
    let mut version = engine.program.rule_version(rule);
    version.declaration_block = Some(DeclarationBlockID(2));
    engine.replace_rule_version(rule, version);
    engine.memory.set_tier3_limit_for_test(0);

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, answers| {
        planned.extend(answers.iter().map(|answer| answer.style_node));
    }));
    assert_eq!(planned, vec![nodes[1].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::CascadeWinnerGroup), 0);
    engine.discard_style_transaction_outputs();
}

#[test]
fn cascade_state_omits_incomplete_element_declarations() {
    let (mut engine, nodes) = linear_document();
    let author = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    engine.set_rule_declared_properties(author, &[(1, false)], true);
    engine.set_element_declared_properties(
        nodes[0],
        ElementDeclarationKind::InlineStyle,
        &[DeclaredProperty {
            property: 1,
            important: false,
            operator: CascadeOperator::Declared,
            value: SpecifiedValueID(101),
        }],
        false,
    );
    commit_test_setup(&mut engine);
    let matches = vec![concrete_rule_match(&engine, nodes[0], author, 0, None)];

    engine.matches_for_cascade(matches, false, Some(nodes[0]));

    let key = WinnerGroupKey::current(nodes[0], engine.program.version());
    assert!(
        matches!(engine.winner_groups.winner(key, 1), Lookup::Known(winner) if winner.source == WinnerSource::Rule(author))
    );
}

#[test]
fn cascade_matching_preserves_empty_pseudo_element_presence() {
    let (mut engine, nodes) = linear_document();
    let first = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let second = add_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201));
    engine.set_rule_declared_properties(first, &[], true);
    engine.set_rule_declared_properties(second, &[], true);
    commit_test_setup(&mut engine);
    let pseudo_element = Some(PseudoElementTarget::new(PseudoElementKind(0)));
    let matches = vec![
        concrete_rule_match(&engine, nodes[0], first, 0, pseudo_element),
        concrete_rule_match(&engine, nodes[0], second, 1, pseudo_element),
    ];

    let compacted = engine.matches_for_cascade(matches, false, None);

    assert_eq!(compacted.len(), 1);
    assert_eq!(compacted[0].pseudo_element, pseudo_element);
}

#[test]
fn pseudo_winner_deltas_update_only_their_sparse_cascade_row() {
    let (mut engine, nodes) = linear_document();
    let pseudo = PseudoElementTarget::new(PseudoElementKind(0));
    let element = add_target_rule(&mut engine, StyleSheetObjectID(1), StyleAtomID(200));
    let lower = add_pseudo_target_rule(&mut engine, StyleSheetObjectID(2), StyleAtomID(201), pseudo);
    let higher = add_pseudo_target_rule(&mut engine, StyleSheetObjectID(3), StyleAtomID(202), pseudo);
    engine.set_rule_declared_properties_with_values(element, &[(1, false, SpecifiedValueID(100))], true);
    engine.set_rule_declared_properties_with_values(lower, &[(1, false, SpecifiedValueID(200))], true);
    engine.set_rule_declared_properties_with_values(higher, &[(1, false, SpecifiedValueID(300))], true);
    commit_test_setup(&mut engine);
    let element_match = concrete_rule_match(&engine, nodes[0], element, 0, None);
    let lower_match = concrete_rule_match(&engine, nodes[0], lower, 1, Some(pseudo));
    let higher_match = concrete_rule_match(&engine, nodes[0], higher, 2, Some(pseudo));
    engine.matches_for_cascade(vec![element_match, lower_match, higher_match], false, Some(nodes[0]));

    let element_key = WinnerGroupKey::current(nodes[0], engine.program.version());
    let pseudo_key = WinnerGroupKey::current_pseudo(nodes[0], pseudo, engine.program.version());
    let Lookup::Known((_, element_state)) = engine.winner_groups.token_for(element_key) else {
        panic!("element winner state was not retained");
    };
    let removed = SelectorTruthDelta {
        node: nodes[0],
        rule: higher,
        program: higher_match.program,
        entry: higher_match.entry,
        change: SetChange::Removed,
        selector_truth_changed: true,
    };

    assert!(engine.apply_cascade_winner_match_deltas(
        nodes[0],
        &[element_match, lower_match],
        &[removed],
        &mut Vec::new()
    ));
    assert!(matches!(
        engine.winner_groups.token_for(element_key),
        Lookup::Known((_, state)) if state == element_state
    ));
    assert!(matches!(
        engine.winner_groups.winner(pseudo_key, 1),
        Lookup::Known(winner) if winner.source == WinnerSource::Rule(lower)
    ));
}

#[test]
fn held_pseudo_styles_without_witnesses_force_a_recompute() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw_nodes = [0; 1];
    engine.allocate_style_nodes(&mut raw_nodes);
    let node = StyleNodeID::from_raw(raw_nodes[0]).unwrap();

    // No held pseudo styles: nothing to vouch for, the skip is safe.
    assert!(engine.pseudo_cascade_states_are_unchanged(node));

    // A held pseudo style with neither a computed cascade record nor a winner row has no
    // witness the check can judge, so it must force a recompute instead of passing
    // vacuously. This is the state a Tier-3 winner-group eviction leaves behind.
    let pseudo_kind = 2_u8;
    engine
        .computed_group_sets
        .record_pseudo_kind_for_test(node, pseudo_kind);
    assert!(!engine.pseudo_cascade_states_are_unchanged(node));

    // A current winner row plus the matching computed cascade record witness the held style
    // and prove it unchanged.
    let state = engine.intern_cascade_state(&[], None);
    let target = tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(pseudo_kind)));
    let version = engine.program.version();
    engine.winner_groups.set_pseudo(node, target, state, version);
    engine.computed_group_sets.observe_pseudo_retained_cascade_state(
        computed::ComputedStyleTarget::new(node, pseudo_kind),
        Some((engine.winner_groups.generation(), state)),
    );
    assert!(engine.pseudo_cascade_states_are_unchanged(node));

    // Losing the winner rows again, as an eviction does, must flip the answer back.
    engine.winner_groups.evict();
    assert!(!engine.pseudo_cascade_states_are_unchanged(node));
}

#[test]
fn assigned_marker_and_backdrop_winners_without_retained_states_force_a_recompute() {
    for pseudo_kind in [1_u8, 5_u8] {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut raw_nodes = [0; 1];
        engine.allocate_style_nodes(&mut raw_nodes);
        let node = StyleNodeID::from_raw(raw_nodes[0]).unwrap();
        let state = engine.intern_cascade_state(&[], None);
        let target = tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(pseudo_kind)));

        engine
            .computed_group_sets
            .record_pseudo_kind_for_test(node, pseudo_kind);
        engine
            .winner_groups
            .set_pseudo(node, target, state, engine.program.version());

        assert!(
            !engine.pseudo_cascade_states_are_unchanged(node),
            "pseudo kind {pseudo_kind} passed without a retained cascade state"
        );
    }
}

fn add_two_class_target_rule(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    target: StyleAtomID,
    required: StyleAtomID,
) {
    let program = engine.programs.add(test_selector_program(
        ".target.required",
        &[("target", target), ("required", required)],
    ));

    let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_nested_sibling_target_rule(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    guard: StyleAtomID,
    container: StyleAtomID,
    target: StyleAtomID,
) {
    let program = engine.programs.add(test_selector_program(
        ".guard ~ .container .target",
        &[("guard", guard), ("container", container), ("target", target)],
    ));

    let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_nested_sibling_target_fallback_rule(
    engine: &mut StyleEngine,
    sheet_object: StyleSheetObjectID,
    guard: StyleAtomID,
    container: StyleAtomID,
    target: StyleAtomID,
) {
    let mut builder = selector::SelectorProgramBuilder::new();
    let guard = builder.push_feature(selector::FeatureTest::Class(guard));
    let preceding = builder.push(selector::SelectorOp::PrecedingSibling(guard));
    let container = builder.push_feature(selector::FeatureTest::Class(container));
    let guarded_container = builder.push_compound(&[container, preceding]);
    let ancestor = builder.push_ancestor(guarded_container);
    let target = builder.push_feature(selector::FeatureTest::Class(target));
    let all_positions = push_all_positions(&mut builder);
    let selector = builder.push_compound(&[target, all_positions, ancestor]);
    builder.push_entry(selector);
    let program = engine.programs.add(builder.finish());

    let sheet = engine.add_sheet(sheet_object, CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_retrying_selector_list_rule(
    engine: &mut StyleEngine,
    guard: StyleAtomID,
    target: StyleAtomID,
    extra: StyleAtomID,
) {
    let mut builder = selector::SelectorProgramBuilder::new();
    let target = builder.push_feature(selector::FeatureTest::Class(target));
    builder.push_entry(target);
    let extra = builder.push_feature(selector::FeatureTest::Class(extra));
    let guard = builder.push_feature(selector::FeatureTest::Class(guard));
    let preceding = builder.push(selector::SelectorOp::PrecedingSibling(guard));
    let all_positions = push_all_positions(&mut builder);
    let more_specific = builder.push_compound(&[target, extra, all_positions, preceding]);
    let more_specific_entry = builder.push_entry(more_specific);
    builder.set_entry_specificity(
        more_specific_entry,
        Specificity {
            classes: 3,
            ..Specificity::default()
        },
    );
    let program = engine.programs.add(builder.finish());

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_nth_of_type_target_rule(engine: &mut StyleEngine, target: StyleAtomID, step: i32, offset: i32) {
    let selector_text = format!(".target:nth-of-type({step}n{offset:+})");
    let program = engine
        .programs
        .add(test_selector_program(&selector_text, &[("target", target)]));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_nth_target_rule(engine: &mut StyleEngine, target: StyleAtomID, step: i32, offset: i32) {
    let selector_text = format!(".target:nth-child({step}n{offset:+})");
    let program = engine
        .programs
        .add(test_selector_program(&selector_text, &[("target", target)]));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

fn add_guarded_nth_target_rule(engine: &mut StyleEngine, guard: StyleAtomID, also: StyleAtomID, target: StyleAtomID) {
    let program = engine.programs.add(test_selector_program(
        ".guard.also:nth-child(2n+1) .target",
        &[("guard", guard), ("also", also), ("target", target)],
    ));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

/// `.guard.also + .target`, whose predecessor is found by one class and required to carry two.
fn add_two_class_adjacent_target_rule(
    engine: &mut StyleEngine,
    guard: StyleAtomID,
    also: StyleAtomID,
    target: StyleAtomID,
) {
    let program = engine.programs.add(test_selector_program(
        ".guard.also + .target",
        &[("guard", guard), ("also", also), ("target", target)],
    ));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

/// `.guard + .guard + .target`.
fn add_double_guard_adjacent_target_rule(engine: &mut StyleEngine, guard: StyleAtomID, target: StyleAtomID) {
    let program = engine.programs.add(test_selector_program(
        ".guard + .guard + .target",
        &[("guard", guard), ("target", target)],
    ));

    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
}

#[test]
fn structural_deltas_maintain_the_relation_columns() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);
    assert_eq!(
        engine.tree().children(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[1], nodes[2], nodes[3]]
    );
    assert_eq!(
        engine.tree().preorder(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[0], nodes[1], nodes[2], nodes[3]]
    );
    assert_eq!(engine.tree().connected_element_count(), 4);
}

#[test]
fn candidate_narrowing_requires_the_whole_subject_compound() {
    let (mut engine, nodes) = linear_document();
    let tag_span = StyleAtomID(100);
    let tag_div = StyleAtomID(101);
    let class_target = StyleAtomID(200);
    for (node, tag) in [(nodes[1], tag_span), (nodes[2], tag_div)] {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
        add_feature(&mut engine, node, FeatureKey::Class(class_target));
    }
    discard_transaction(&mut engine);
    prepare_empty_transaction_fact_view(&mut engine, nodes[0]);

    let mut regions = ImpactRegions::new();
    engine.add_narrowed_region(
        ImpactRegion::Subtree(nodes[0]),
        &RoutingSite {
            subject: &[DispatchKey::Class(class_target)],
            subject_required: &[DispatchKey::TagName(tag_span)],
            position: SubjectPosition::UNBOUNDED,
            path: &[],
            waypoints: &[],
            in_flux: None,
            exact_entry: None,
            exact_tree_evaluation: None,
            refresh_rule: None,
        },
        &mut regions,
    );

    assert_eq!(regions.regions(), &[ImpactRegion::Node(nodes[1])]);
}

#[test]
fn exact_node_narrowing_does_not_enumerate_the_subject_posting() {
    let (mut engine, nodes) = linear_document();
    let class_target = StyleAtomID(200);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(class_target));
    add_feature(&mut engine, nodes[2], FeatureKey::Class(class_target));
    discard_transaction(&mut engine);
    prepare_empty_transaction_fact_view(&mut engine, nodes[0]);

    let posting_builds = engine.counters().get(Counter::RemainingPostingBuilds);
    let mut regions = ImpactRegions::new();
    engine.add_narrowed_region(
        ImpactRegion::Node(nodes[1]),
        &RoutingSite {
            subject: &[DispatchKey::Class(class_target)],
            subject_required: &[],
            position: SubjectPosition::UNBOUNDED,
            path: &[],
            waypoints: &[],
            in_flux: None,
            exact_entry: None,
            exact_tree_evaluation: None,
            refresh_rule: None,
        },
        &mut regions,
    );

    assert_eq!(regions.regions(), &[ImpactRegion::Node(nodes[1])]);
    assert_eq!(engine.counters().get(Counter::RemainingPostingBuilds), posting_builds);
}

#[test]
fn exact_node_narrowing_preserves_posting_history_for_truth_patches() {
    let (mut engine, nodes) = linear_document();
    let class_target = StyleAtomID(200);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(class_target));
    add_feature(&mut engine, nodes[2], FeatureKey::Class(class_target));
    discard_transaction(&mut engine);
    prepare_empty_transaction_fact_view(&mut engine, nodes[0]);
    engine.selector_truth_changes_active = true;

    let posting_builds = engine.counters().get(Counter::RemainingPostingBuilds);
    let mut regions = ImpactRegions::new();
    engine.add_narrowed_region(
        ImpactRegion::Node(nodes[1]),
        &RoutingSite {
            subject: &[DispatchKey::Class(class_target)],
            subject_required: &[],
            position: SubjectPosition::UNBOUNDED,
            path: &[],
            waypoints: &[],
            in_flux: None,
            exact_entry: None,
            exact_tree_evaluation: None,
            refresh_rule: None,
        },
        &mut regions,
    );

    assert_eq!(regions.regions(), &[ImpactRegion::Node(nodes[1])]);
    // Truth patches also consume the posting's accumulated already-planned history, so this path
    // must not replace the posting walk with local membership tests.
    assert_eq!(
        engine.counters().get(Counter::RemainingPostingBuilds),
        posting_builds + 1
    );
}

#[test]
fn a_required_key_in_flux_admits_both_sides_of_the_change() {
    let (mut engine, nodes) = linear_document();
    let class_target = StyleAtomID(200);
    discard_transaction(&mut engine);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(class_target));
    let mut transaction = engine.take_transaction();
    let view = engine.transaction_fact_view_for(&mut transaction, nodes[0], &ImpactRegions::new());
    engine.release_transaction(transaction);
    engine.transaction_fact_view = Some(view);

    let mut regions = ImpactRegions::new();
    engine.add_narrowed_region(
        ImpactRegion::Node(nodes[1]),
        &RoutingSite {
            subject: &[DispatchKey::Class(class_target)],
            subject_required: &[DispatchKey::Class(class_target)],
            position: SubjectPosition::UNBOUNDED,
            path: &[],
            waypoints: &[],
            in_flux: Some((nodes[1], DispatchKey::Class(class_target))),
            exact_entry: None,
            exact_tree_evaluation: None,
            refresh_rule: None,
        },
        &mut regions,
    );

    assert_eq!(regions.regions(), &[ImpactRegion::Node(nodes[1])]);
}

#[test]
fn an_attribute_in_flux_includes_every_name_form() {
    let (mut engine, nodes) = linear_document();
    let qualified_name = StyleAtomID(200);
    let local_name = StyleAtomID(201);
    let folded_qualified_name = StyleAtomID(202);
    let folded_local_name = StyleAtomID(203);
    engine.note_attribute_name_forms(
        qualified_name,
        index::AttributeNameForms {
            local: local_name,
            folded_name: folded_qualified_name,
            folded_local: folded_local_name,
        },
    );
    add_feature(&mut engine, nodes[1], FeatureKey::Attribute(qualified_name));
    discard_transaction(&mut engine);
    remove_feature(&mut engine, nodes[1], FeatureKey::Attribute(qualified_name));
    let transaction = engine.take_transaction();
    let features = engine.feature_delta_for(&transaction);

    for name in [qualified_name, local_name, folded_qualified_name, folded_local_name] {
        assert!(
            features
                .keys_of_node(nodes[1])
                .contains(&DispatchKey::AttributeName(name)),
            "missing {name:?}"
        );
    }
    engine.transaction_fact_view = Some(TransactionFactView {
        root: nodes[0],
        moved_features: features,
        before_sibling_geometry: SiblingSequenceGeometry::default(),
        before_sibling_sequence_by_parent: Vec::new(),
        before_sibling_parents_by_sequence: Vec::new(),
        before_absent_nodes: Vec::new(),
        before_sibling_relations_available: false,
        prefix: None,
        retained_truth_available: false,
        resident_side: TransactionFactSide::After,
        local_facts_are_shared: false,
        before: None,
        after: None,
        opposite_fully_materialized: false,
    });
    assert!(!engine.moved_features_of(nodes[1]).is_empty());
    assert!(engine.moved_features_of(nodes[2]).is_empty());
    engine.release_transaction(transaction);
}

#[test]
fn an_alternate_ancestor_witness_keeps_a_candidate_out_of_the_plan() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for node in [nodes[1], nodes[2]] {
        add_feature(&mut engine, node, FeatureKey::Class(guard));
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    engine.end_cold_matching_batch();

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.is_empty());
    assert_eq!(engine.counters().get(Counter::PrefixConvergencePasses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixConvergenceNodes), 2);
    assert_eq!(engine.counters().get(Counter::PrefixConvergenceStops), 1);
}

#[test]
fn an_empty_sparse_route_does_not_compile_its_region() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    discard_transaction(&mut engine);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.is_empty());
}

#[test]
fn overlapping_prefix_changes_are_evaluated_once() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    engine.end_cold_matching_batch();

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert_eq!(engine.counters().get(Counter::PrefixConvergencePasses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixConvergenceNodes), 3);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn retained_prefix_transitions_supply_invalidation_and_matching() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    engine.end_cold_matching_batch();
    assert!(engine.memory().bytes_in_category(MemoryCategory::PrefixTransitionCache) > 0);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    assert!(engine.match_element(nodes[3]).unwrap().is_empty());
    engine.end_cold_matching_batch();

    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    planned.clear();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn covered_prefix_changes_forget_only_the_covered_subtree() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    assert_eq!(engine.match_element_for_cascade(nodes[3]).unwrap().len(), 1);
    engine.end_cold_matching_batch();
    assert!(engine.memory().bytes_in_category(MemoryCategory::PrefixTransitionCache) > 0);
    assert!(engine.memory().bytes_in_category(MemoryCategory::PrefixAnswerCache) > 0);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut transaction = engine.take_transaction();
    let mut regions = ImpactRegions::new();
    engine.transaction_fact_view = Some(engine.transaction_fact_view_for(&mut transaction, nodes[0], &regions));
    let fact_view_bytes = engine.transaction_fact_view.as_ref().unwrap().capacity_bytes();
    engine
        .memory
        .reserve_required(MemoryCategory::BatchScratch, fact_view_bytes);
    regions.add(ImpactRegion::Subtree(nodes[1]), &mut engine.counters);

    let route = engine.routing.routes_for(RoutingKey::Class(guard))[0];
    let mut pending = PendingRoutes::new();
    pending
        .regions_for(PendingRoute {
            route,
            exact_tree_evaluation: None,
        })
        .push(ImpactRegion::Subtree(nodes[1]));
    pending.finish();
    let pending_route_scratch_bytes = pending
        .values()
        .map(|routes| (routes.capacity() * size_of::<ImpactRegion>()) as u64)
        .sum();
    engine
        .memory
        .reserve_required(MemoryCategory::BatchScratch, pending_route_scratch_bytes);

    let outcome = engine.add_prefix_convergence_regions(
        &mut pending,
        &mut regions,
        false,
        &transaction,
        &SequenceChanges::new(),
        &[],
    );
    assert!(engine.prefix_caches.borrow().states.is_current());
    assert!(!outcome.sibling_routes_are_covered);
    assert!(pending.is_empty());
    // The walk skips the covered subtree but keeps the cache warm: only the transitions
    // that depend on the skipped node are forgotten.
    let (scope_program, _) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    let prefix_caches = Rc::clone(&engine.prefix_caches);
    let mut caches = prefix_caches.borrow_mut();
    let Lookup::Known(states) = caches.states.lookup_mut(scope_program) else {
        panic!("the document program's states survive a covered skip");
    };
    assert!(states.has_transition(nodes[0]));
    assert!(!states.has_transition(nodes[1]));
    assert!(!states.has_transition(nodes[3]));
    drop(states);
    drop(caches);

    let fact_view_bytes = engine.transaction_fact_view.as_ref().unwrap().capacity_bytes();
    engine.transaction_fact_view = None;
    engine.memory.release(MemoryCategory::BatchScratch, fact_view_bytes);
    engine.release_transaction(transaction);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    assert!(engine.match_element_for_cascade(nodes[3]).unwrap().is_empty());
    engine.end_cold_matching_batch();
}

#[test]
fn selective_matching_completes_a_bounded_prefix_transition_window() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = vec![0_u32; PREFIX_TRANSITION_CACHE_COMPLETION_BUDGET + 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(nodes[index - 1].raw()), None, None)),
        );
    }
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }

    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[0], guard), (*nodes.last().unwrap(), target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.force_bounded_prefix_completion = true;
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    engine.match_element(nodes[0]).unwrap();
    engine.end_cold_matching_batch();
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    assert_eq!(engine.match_element(*nodes.last().unwrap()).unwrap().len(), 1);
    engine.end_cold_matching_batch();

    remove_feature(&mut engine, *nodes.last().unwrap(), FeatureKey::Class(target));
    let cache_hits_before = engine.counters().get(Counter::PrefixTransitionCacheHits);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes.last().unwrap().raw()]);
    assert_eq!(
        engine.counters().get(Counter::PrefixTransitionCacheHits) - cache_hits_before,
        1,
        "invalidation reuses the retained partial prefix transition cache"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_prefix_upquery_retains_every_transition_on_its_ancestor_chain() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    assert_eq!(engine.match_element(nodes[3]).unwrap().len(), 1);
    let (scope_program, _) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    let prefix_caches = Rc::clone(&engine.batch_matching_traversal.as_ref().unwrap().prefix_caches);
    let mut caches = prefix_caches.borrow_mut();
    let states = match caches.states.lookup_mut(scope_program) {
        Lookup::Known(states) => states,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a retained prefix program"),
    };
    assert!(nodes.iter().all(|&node| states.has_transition(node)));
    drop(states);
    drop(caches);
    engine.end_cold_matching_batch();

    let mut caches = engine.prefix_caches.borrow_mut();
    caches.states.make_scratch(&mut engine.memory);
    assert!(matches!(caches.states.lookup_mut(scope_program), Lookup::Known(_)));
    caches.states.release();
    assert!(matches!(
        caches.states.lookup_mut(scope_program),
        Lookup::Missing(prefix::PrefixStateCacheGap::MissingProgram(gap)) if gap == scope_program
    ));
}

#[test]
fn partial_match_answer_completion_shares_prefix_states_between_nodes() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    for &node in &nodes {
        for kind in ElementDeclarationKind::ALL {
            engine.set_element_declared_properties(node, kind, &[], true);
        }
    }
    for (node, class) in [(nodes[1], guard), (nodes[2], target), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.begin_published_match_answer_completion_batch(nodes[0], false);
    assert!(engine.batch_matching_traversal.as_ref().unwrap().batch.is_none());
    let answer = engine.complete_published_match_answer(nodes[2], None).unwrap();
    assert!(answer.cascade_winners_are_complete);
    assert_eq!(engine.match_element(nodes[3]).unwrap().len(), 1);
    engine.end_published_match_answer_completion_batch();
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_cached_prefix_answer_is_returned_in_cascade_order() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let specific = add_guard_target_rule(&mut engine, guard, target);
    let general = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(specific, &[(1, false)], true);
    engine.set_rule_declared_properties(general, &[(2, false)], true);
    for &node in &nodes {
        for kind in ElementDeclarationKind::ALL {
            engine.set_element_declared_properties(node, kind, &[], true);
        }
    }
    for (node, class) in [(nodes[1], guard), (nodes[2], target), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    let first = engine.match_element_for_cascade(nodes[2]).unwrap();
    let second = engine.match_element_for_cascade(nodes[3]).unwrap();
    engine.end_cold_matching_batch();

    for matches in [first, second] {
        assert_eq!(
            matches.iter().map(|matched| matched.rule).collect::<Vec<_>>(),
            [general, specific]
        );
    }
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheMisses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheHits), 1);
}

#[test]
fn an_identity_only_published_prefix_answer_is_returned_in_cascade_order() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let specific = add_guard_target_rule(&mut engine, guard, target);
    let general = add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    engine.set_rule_declared_properties(specific, &[(1, false)], true);
    engine.set_rule_declared_properties(general, &[(2, false)], true);
    for &node in &nodes {
        for kind in ElementDeclarationKind::ALL {
            engine.set_element_declared_properties(node, kind, &[], true);
        }
    }
    for (node, class) in [(nodes[1], guard), (nodes[2], target), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.begin_published_match_answer_completion_batch(nodes[0], false);
    assert_eq!(
        engine
            .match_element_for_cascade(nodes[2])
            .unwrap()
            .iter()
            .map(|matched| matched.rule)
            .collect::<Vec<_>>(),
        [general, specific]
    );
    let published = engine.complete_published_match_answer(nodes[3], None).unwrap();
    assert!(published.cascade_input.is_some());
    assert!(published.matches.is_none());
    engine.end_published_match_answer_completion_batch();
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheMisses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheHits), 1);

    engine
        .published_match_answers
        .push(published, &mut engine.memory, &mut engine.counters);
    engine.published_match_answers.sort();

    let matches = engine.consume_published_match_answer(nodes[3]).unwrap();
    assert_eq!(
        matches.iter().map(|matched| matched.rule).collect::<Vec<_>>(),
        [general, specific]
    );

    let mut streamed = Vec::new();
    assert_eq!(
        engine.consume_published_match_answer_with(nodes[3], 0, |_, _, rule, _, _, _, _| streamed.push(rule)),
        Some(2)
    );
    assert!(streamed.is_empty());
    assert_eq!(
        engine.consume_published_match_answer_with(nodes[3], 2, |index, _, rule, _, _, _, _| {
            assert_eq!(index, streamed.len());
            streamed.push(rule);
        }),
        Some(2)
    );
    assert_eq!(streamed, [general, specific]);
}

#[test]
fn shared_retained_answer_completion_reuses_compact_cascade_state() {
    let (mut engine, nodes) = nested_document();
    let target = StyleAtomID(200);
    for index in 0..10 {
        let rule = add_target_rule(&mut engine, StyleSheetObjectID(index + 1), target);
        engine.set_rule_declared_properties(rule, &[(1, false)], true);
    }
    for &node in &nodes {
        for kind in ElementDeclarationKind::ALL {
            engine.set_element_declared_properties(node, kind, &[], true);
        }
    }
    for node in [nodes[2], nodes[3]] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    discard_transaction(&mut engine);

    let first_matches = engine.match_element(nodes[2]).unwrap();
    let second_matches = engine.match_element(nodes[3]).unwrap();
    assert_eq!(first_matches.len(), 10);
    assert_eq!(second_matches.len(), 10);
    engine.remember_retained_match_answer(nodes[2], &first_matches);
    engine.remember_retained_match_answer(nodes[3], &second_matches);
    let first_identity = engine.retained_match_answers.answer_identity(nodes[2]).unwrap();
    assert_eq!(
        engine.retained_match_answers.answer_identity(nodes[3]),
        Some(first_identity)
    );

    let (_, dispatch) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    let orders = RetainedAnswerCascadeOrders::Dispatch(&dispatch);
    let first = engine.complete_published_match_answer(nodes[2], Some(orders)).unwrap();
    let cascade_input = first.cascade_input.unwrap();
    assert!(engine.shared_cascade_completion_is_profitable(first_identity, cascade_input));
    let compaction_rows = engine.counters().get(Counter::CascadeMatchesBeforeCompaction);

    let second = engine
        .complete_published_match_answer_from_cascade_input(
            nodes[3],
            nodes[2],
            cascade_input,
            orders,
            first.cascade_winners_are_complete,
        )
        .unwrap();

    assert_eq!(second.cascade_input, Some(cascade_input));
    assert_eq!(second.matches.as_ref().unwrap().len(), 1);
    assert_eq!(
        engine.counters().get(Counter::CascadeMatchesBeforeCompaction),
        compaction_rows
    );
    let program_version = engine.program.version();
    assert_eq!(
        engine
            .winner_groups
            .token_for(WinnerGroupKey::current(nodes[2], program_version)),
        engine
            .winner_groups
            .token_for(WinnerGroupKey::current(nodes[3], program_version))
    );
}

#[test]
fn closure_identity_stop_verification_is_observer_only() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    for &node in &nodes {
        for kind in ElementDeclarationKind::ALL {
            engine.set_element_declared_properties(node, kind, &[], true);
        }
    }
    for (node, class) in [(nodes[1], guard), (nodes[2], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.begin_published_match_answer_completion_batch(nodes[0], false);
    let answer = engine.complete_published_match_answer(nodes[2], None).unwrap();
    let cascade_input = answer.cascade_input.unwrap();
    let counters_before: Vec<_> = engine.counters.iter().collect();
    let memory_before: Vec<_> = memory::MEMORY_CATEGORIES
        .iter()
        .map(|&category| engine.memory.bytes_in_category(category))
        .collect();
    let winner_groups_before = engine.winner_groups.verification_copy();
    let catalog_entry_count_before = engine.match_answers.answers.live_len();
    let retained_answer_column_before = engine.retained_match_answers.column.clone();
    let retained_cascade_column_before = engine.retained_match_answers.cascade_input_column.clone();

    engine.verify_retained_cascade_input(nodes[2], cascade_input);

    assert_eq!(engine.counters.iter().collect::<Vec<_>>(), counters_before);
    assert_eq!(
        memory::MEMORY_CATEGORIES
            .iter()
            .map(|&category| engine.memory.bytes_in_category(category))
            .collect::<Vec<_>>(),
        memory_before
    );
    assert!(engine.winner_groups.node_rows_are_semantically_equal(
        &winner_groups_before,
        nodes[2],
        engine.program.version()
    ));
    assert_eq!(engine.match_answers.answers.live_len(), catalog_entry_count_before);
    assert_eq!(engine.retained_match_answers.column, retained_answer_column_before);
    assert_eq!(
        engine.retained_match_answers.cascade_input_column,
        retained_cascade_column_before
    );
    engine.end_published_match_answer_completion_batch();
}

#[test]
fn a_cached_prefix_answer_preserves_incomplete_cascade_winners() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    engine.set_rule_gated_by_container_query(rule);
    for (node, class) in [(nodes[1], guard), (nodes[2], target), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    let mut first_compact = None;
    let mut first_complete = false;
    engine
        .match_element_for_purpose_with_compact_answer(
            nodes[2],
            true,
            Some(&mut first_compact),
            Some(&mut first_complete),
        )
        .unwrap();
    let mut second_compact = None;
    let mut second_complete = false;
    engine
        .match_element_for_purpose_with_compact_answer(
            nodes[3],
            true,
            Some(&mut second_compact),
            Some(&mut second_complete),
        )
        .unwrap();
    assert!(!first_complete);
    assert!(!second_complete);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheMisses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheHits), 1);
    engine.end_cold_matching_batch();
}

#[test]
fn retained_prefix_convergence_amortizes_missing_subtrees() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let node_count = 2 * PREFIX_TRANSITION_CACHE_COMPLETION_BUDGET + 4;
    let mut raw = vec![0_u32; node_count];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(nodes[index - 1].raw()), None, None)),
        );
    }
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }

    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[0], guard), (*nodes.last().unwrap(), target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.force_bounded_prefix_completion = true;
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    engine.match_element(nodes[0]).unwrap();
    engine.end_cold_matching_batch();

    remove_feature(&mut engine, nodes[0], FeatureKey::Class(guard));
    let upqueries_before = engine.counters().get(Counter::PrefixConvergenceUpqueries);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(!planned.is_empty());
    assert_eq!(
        engine.counters().get(Counter::PrefixConvergenceUpqueries) - upqueries_before,
        PREFIX_TRANSITION_CACHE_COMPLETION_BUDGET as u64
    );

    add_feature(&mut engine, nodes[0], FeatureKey::Class(guard));
    let upqueries_before = engine.counters().get(Counter::PrefixConvergenceUpqueries);
    planned.clear();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(
        engine.counters().get(Counter::PrefixConvergenceUpqueries) - upqueries_before,
        0,
        "publishing the first plan completes the retained prefix workspace"
    );
}

#[test]
fn equivalent_prefix_contributions_share_a_cascade_answer() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    for (node, class) in [(nodes[1], guard), (nodes[2], target), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    let first = engine.match_element_for_cascade(nodes[2]).unwrap();
    let first_signature = engine.match_element_signature(nodes[2]).unwrap();
    let second = engine.match_element_for_cascade(nodes[3]).unwrap();
    let second_signature = engine.match_element_signature(nodes[3]).unwrap();
    assert_eq!(first.len(), 1);
    assert_eq!(first[0].rule, rule);
    assert_eq!(second.len(), 1);
    assert_eq!(second[0].rule, rule);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheMisses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheHits), 1);
    assert_eq!(first_signature, second_signature);
    assert_eq!(engine.counters().get(Counter::MatchAnswerSignatures), 1);
    assert_eq!(engine.counters().get(Counter::MatchAnswerSignatureReuses), 1);
    engine.end_cold_matching_batch();
}

#[test]
fn element_declarations_refuse_selector_only_prefix_answer_reuse() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    for (node, class) in [(nodes[1], guard), (nodes[2], target), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    for (node, value) in [(nodes[2], SpecifiedValueID(102)), (nodes[3], SpecifiedValueID(103))] {
        engine.set_element_declared_properties(
            node,
            ElementDeclarationKind::InlineStyle,
            &[DeclaredProperty {
                property: 2,
                important: false,
                operator: CascadeOperator::Declared,
                value,
            }],
            true,
        );
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    engine.match_element_for_cascade(nodes[2]).unwrap();
    engine.match_element_for_cascade(nodes[3]).unwrap();

    for (node, value) in [(nodes[2], SpecifiedValueID(102)), (nodes[3], SpecifiedValueID(103))] {
        let key = WinnerGroupKey::current(node, engine.program.version());
        assert!(
            matches!(engine.winner_groups.winner(key, 2), Lookup::Known(winner) if winner.source == WinnerSource::Element(ElementDeclarationKind::InlineStyle) && winner.key.value == value)
        );
    }
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheMisses), 0);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheHits), 0);
    engine.end_cold_matching_batch();
}

#[test]
fn retained_prefix_transitions_are_discarded_under_pressure() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.memory.set_tier3_limit_for_test(0);
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    engine.end_cold_matching_batch();

    assert_eq!(
        engine.memory().bytes_in_category(MemoryCategory::PrefixTransitionCache),
        0
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
}

#[test]
fn program_changes_discard_retained_prefix_transitions() {
    let (mut engine, nodes) = nested_document();
    add_guard_target_rule(&mut engine, StyleAtomID(200), StyleAtomID(201));
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element_for_cascade(node).unwrap();
    }
    engine.end_cold_matching_batch();
    assert!(engine.memory().bytes_in_category(MemoryCategory::PrefixTransitionCache) > 0);
    assert!(engine.memory().bytes_in_category(MemoryCategory::PrefixAnswerCache) > 0);

    // A program change keeps the outgoing program's prefix work interned for one invalidation
    // generation, so a scope whose effective sheet set is unchanged re-adopts it without a rebuild.
    let sheet = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    assert!(engine.memory().bytes_in_category(MemoryCategory::PrefixTransitionCache) > 0);

    // A second generation in which no scope re-adopts the program discards it and its caches.
    // What stays is the shared dispatch template's prefix machinery: its shape names only live
    // immutable selector programs, and the selector-program sweep drops it before an identity
    // it references could ever be recycled.
    let another = engine.add_sheet(StyleSheetObjectID(3), CascadeOrigin::Author);
    engine.attach_sheet(another, TreeScopeID::DOCUMENT);
    assert_eq!(engine.scope_programs.iter().flatten().count(), 0);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::PrefixAnswerCache), 0);
    assert_eq!(engine.scope_dispatch_templates.len(), 1);
    assert!(
        engine.memory().bytes_in_category(MemoryCategory::PrefixTransitionCache) > 0,
        "the shared dispatch template keeps its prefix states"
    );
}

#[test]
fn reused_element_identity_does_not_inherit_a_prefix_transition() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    engine.end_cold_matching_batch();

    let old_relations = relations(Some(nodes[2].raw()), None, None);
    engine.record_tree_delta(nodes[3], Some(old_relations), None);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));

    engine.tree.release_retired_identities(&mut engine.memory);
    let mut reused_raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut reused_raw);
    assert_eq!(reused_raw[0], nodes[3].raw());
    let reused = StyleNodeID::from_raw(reused_raw[0]).unwrap();
    engine.record_tree_delta(reused, None, Some(old_relations));
    add_feature(&mut engine, reused, FeatureKey::Class(target));

    planned.clear();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![reused.raw()]);
}

#[test]
fn unobserved_inputs_do_not_seed_prefix_convergence() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    discard_transaction(&mut engine);

    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[2], FeatureKey::Class(StyleAtomID(202)));
    let mut transaction = engine.take_transaction();
    let view = engine.transaction_fact_view_for(&mut transaction, nodes[0], &ImpactRegions::new());
    assert_eq!(view.before.as_ref().unwrap().row_count(), 2);
    let transition = view.prefix.unwrap();
    assert_eq!(transition.roots, vec![nodes[1]]);
    engine.release_transaction(transaction);
}

#[test]
fn state_input_commits_after_its_old_row_is_snapshotted() {
    let (mut engine, nodes) = linear_document();
    let class = StyleAtomID(200);
    discard_transaction(&mut engine);

    add_feature(&mut engine, nodes[1], FeatureKey::Class(class));
    engine.record_input(
        InputKey::State(nodes[1], StateFact::Hover),
        InputValue::State(false),
        InputValue::State(true),
    );
    assert!(engine.facts.states_of_node(nodes[1]).contains(StateFact::Hover));
    let committed = engine.facts.before_pending_facts();
    let committed_row = committed.row_of(nodes[1]).unwrap();
    assert!(!committed.states_of(committed_row).contains(StateFact::Hover));
    assert!(!committed.carries_dispatch_key(committed_row, DispatchKey::Class(class), false));

    let mut transaction = engine.take_transaction();
    assert!(engine.facts.states_of_node(nodes[1]).contains(StateFact::Hover));
    let view = engine.transaction_fact_view_for(&mut transaction, nodes[0], &ImpactRegions::new());
    engine.transaction_fact_view = Some(view);
    engine.ensure_transaction_fact_rows(&[nodes[1]]);
    let before = engine.transaction_fact_view.as_ref().unwrap().before.as_ref().unwrap();
    let row = before.row_of(nodes[1]).unwrap();
    assert!(!before.states_of(row).contains(StateFact::Hover));
    assert!(!before.carries_dispatch_key(row, DispatchKey::Class(class), false));
    engine.release_transaction(transaction);
}

#[test]
fn selector_queries_advance_current_facts_without_losing_the_transaction_before_side() {
    let (mut engine, nodes) = linear_document();
    let class = StyleAtomID(200);
    discard_transaction(&mut engine);

    add_feature(&mut engine, nodes[1], FeatureKey::Class(class));
    engine.prepare_selector_query();
    let current = engine.facts.primary();
    let current_row = current.row_of(nodes[1]).unwrap();
    assert!(current.carries_dispatch_key(current_row, DispatchKey::Class(class), false));

    engine.record_input(
        InputKey::State(nodes[1], StateFact::Hover),
        InputValue::State(false),
        InputValue::State(true),
    );
    engine.prepare_selector_query();
    let current = engine.facts.primary();
    let current_row = current.row_of(nodes[1]).unwrap();
    assert!(current.states_of(current_row).contains(StateFact::Hover));

    let transaction = engine.take_transaction();
    let before = transaction.before_facts.as_ref().unwrap();
    let before_row = before.row_of(nodes[1]).unwrap();
    assert!(!before.carries_dispatch_key(before_row, DispatchKey::Class(class), false));
    assert!(!before.states_of(before_row).contains(StateFact::Hover));
    engine.release_transaction(transaction);
}

#[test]
fn prefix_transition_uses_arrival_region_coverage() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let arrival = StyleNodeID::from_raw(raw[0]).unwrap();
    let nested_arrival = StyleNodeID::from_raw(raw[1]).unwrap();
    engine.record_tree_delta(
        arrival,
        None,
        Some(relations(Some(nodes[2].raw()), Some(nodes[3].raw()), None)),
    );
    engine.record_tree_delta(nested_arrival, None, Some(relations(Some(arrival.raw()), None, None)));
    add_feature(&mut engine, nested_arrival, FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));

    let transaction = engine.take_transaction();
    let mut regions = ImpactRegions::with_topology(&engine.tree, nodes[0]);
    regions.add(ImpactRegion::Subtree(arrival), &mut Counters::default());
    let transition = engine
        .classify_transaction_facts(&transaction, &regions)
        .prefix
        .unwrap();
    assert_eq!(transition.roots, vec![nodes[1]]);
    engine.release_transaction(transaction);
}

#[test]
fn prefix_convergence_skips_an_already_dirty_arrival() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    engine.end_cold_matching_batch();

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let arrival = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        arrival,
        None,
        Some(relations(Some(nodes[2].raw()), Some(nodes[3].raw()), None)),
    );
    add_feature(&mut engine, arrival, FeatureKey::ArrivingFacts);
    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw(), arrival.raw()]);
    assert_eq!(engine.counters().get(Counter::PrefixConvergencePasses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixConvergenceNodes), 3);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_parent_change_has_no_prefix_fact_transition() {
    let (mut engine, nodes) = nested_document();
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[3],
        Some(relations(Some(nodes[2].raw()), None, None)),
        Some(relations(Some(nodes[0].raw()), Some(nodes[1].raw()), None)),
    );
    add_feature(&mut engine, nodes[1], FeatureKey::Class(StyleAtomID(200)));
    let transaction = engine.take_transaction();
    assert!(
        engine
            .classify_transaction_facts(&transaction, &ImpactRegions::new())
            .prefix
            .is_none()
    );
    engine.release_transaction(transaction);
}

#[test]
fn an_alternate_preceding_sibling_keeps_a_candidate_out_of_the_plan() {
    let (mut engine, nodes) = linear_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);
    for node in [nodes[1], nodes[2]] {
        add_feature(&mut engine, node, FeatureKey::Class(guard));
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[2],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[3].raw()),
        )),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.is_empty());
}

#[test]
fn an_adjacent_replacement_that_preserves_truth_stays_out_of_the_plan() {
    let (mut engine, nodes) = linear_document();
    let guard = StyleAtomID(200);
    let also = StyleAtomID(201);
    let target = StyleAtomID(202);
    add_two_class_adjacent_target_rule(&mut engine, guard, also, target);
    for node in [nodes[1], nodes[2]] {
        for class in [guard, also] {
            add_feature(&mut engine, node, FeatureKey::Class(class));
        }
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[2],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[3].raw()),
        )),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.is_empty());
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn an_old_adjacent_chain_is_compared_with_its_replacement_chain() {
    let (mut engine, nodes) = linear_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_double_guard_adjacent_target_rule(&mut engine, guard, target);
    for node in [nodes[1], nodes[2]] {
        add_feature(&mut engine, node, FeatureKey::Class(guard));
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[2],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[3].raw()),
        )),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

/// Admitted plain an+b entries answer through the prefix automaton, whose positional truth
/// bits must stay honest across every structural mutation shape: a trailing arrival flips
/// from-end answers of untouched preceding siblings, a leading arrival flips forward parity
/// of untouched following siblings, and removals and same-parent moves shift both.
#[test]
fn positional_answers_stay_cold_equivalent_across_sequence_mutations() {
    let tag_a = StyleAtomID(100);
    let tag_b = StyleAtomID(101);
    let class_item = StyleAtomID(200);
    let class_theme = StyleAtomID(201);

    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = vec![0_u32; 11];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let root = nodes[0];
    let container = nodes[1];

    let add_position_rule = |engine: &mut StyleEngine,
                             index: usize,
                             step: i32,
                             offset: i32,
                             from_end: bool,
                             chained: bool,
                             of_type: bool| {
        {
            let mut builder = selector::SelectorProgramBuilder::new();
            let item = builder.push_feature(selector::FeatureTest::Class(class_item));
            let position = builder.push(selector::SelectorOp::NthPosition(selector::NthPosition {
                step,
                offset,
                from_end,
                of_selector: None,
                of_type,
            }));
            let selector = match chained {
                true => {
                    let theme = builder.push_feature(selector::FeatureTest::Class(class_theme));
                    let ancestor = builder.push_ancestor(theme);
                    builder.push_compound(&[item, position, ancestor])
                }
                false => builder.push_compound(&[item, position]),
            };
            builder.push_entry(selector);
            let program = engine.programs.add(builder.finish());
            let sheet = engine.add_sheet(StyleSheetObjectID(1000 + index as u32), CascadeOrigin::Author);
            engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
            let rule = engine.append_rule(sheet, None, RuleKind::Style);
            engine.add_routing_rule(rule, program);
            let mut version = engine.program.rule_version(rule);
            version.selector_program = Some(program);
            version.declaration_block = Some(DeclarationBlockID(1000 + index as u32));
            engine.replace_rule_version(rule, version);
        }
    };
    add_position_rule(&mut engine, 0, 2, 1, false, false, false); // .item:nth-child(odd)
    add_position_rule(&mut engine, 1, 0, 1, true, false, false); // .item:last-child
    add_position_rule(&mut engine, 2, 0, 2, true, false, false); // .item:nth-last-child(2)
    add_position_rule(&mut engine, 3, 0, 2, false, true, false); // .theme .item:nth-child(2)
    add_position_rule(&mut engine, 4, 2, 0, false, false, true); // .item:nth-of-type(2n)
    {
        // .item:empty
        let mut builder = selector::SelectorProgramBuilder::new();
        let item = builder.push_feature(selector::FeatureTest::Class(class_item));
        let empty = builder.push(selector::SelectorOp::Empty);
        let selector = builder.push_compound(&[item, empty]);
        builder.push_entry(selector);
        let program = engine.programs.add(builder.finish());
        let sheet = engine.add_sheet(StyleSheetObjectID(1005), CascadeOrigin::Author);
        engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        let rule = engine.append_rule(sheet, None, RuleKind::Style);
        engine.add_routing_rule(rule, program);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(program);
        version.declaration_block = Some(DeclarationBlockID(1005));
        engine.replace_rule_version(rule, version);
    }

    let mut model: Vec<StyleNodeID> = nodes[2..8].to_vec();
    engine.record_tree_delta(root, None, Some(relations(None, None, None)));
    engine.record_tree_delta(container, None, Some(relations(Some(root.raw()), None, None)));
    for (index, &child) in model.iter().enumerate() {
        let previous = (index > 0).then(|| model[index - 1].raw());
        let next = model.get(index + 1).map(|node| node.raw());
        engine.record_tree_delta(child, None, Some(relations(Some(container.raw()), previous, next)));
    }
    // Tags alternate by identity so the of-type sequences decorrelate from the child
    // sequence as mutations shuffle positions.
    let record_facts = |engine: &mut StyleEngine, node: StyleNodeID, class: StyleAtomID| {
        let tag = match node.raw() % 2 {
            0 => tag_a,
            _ => tag_b,
        };
        set_atom_feature(engine, node, FeatureKey::TagName, tag);
        add_feature(engine, node, FeatureKey::Class(class));
    };
    record_facts(&mut engine, root, class_theme);
    record_facts(&mut engine, container, class_theme);
    for &child in &model {
        record_facts(&mut engine, child, class_item);
    }

    let flush_and_check = |engine: &mut StyleEngine, model: &[StyleNodeID], stage: &str| {
        let mut planned: Vec<StyleNodeID> = Vec::new();
        let scoped = engine.take_style_transaction(root, |_, _, reactions| {
            planned.extend(
                reactions
                    .iter()
                    .map(|reaction| StyleNodeID::from_raw(reaction.style_node).unwrap()),
            );
        });
        let mut live: Vec<StyleNodeID> = vec![root, container];
        live.extend_from_slice(model);
        let targets: &[StyleNodeID] = match scoped {
            true => &planned,
            false => &live,
        };
        for &node in targets {
            let _ = engine.match_element_for_purpose(node, true);
        }
        let (_, dispatch) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
        let mut orders: Vec<(RuleID, SelectorProgramID, u32, u32)> = dispatch
            .entries()
            .iter()
            .map(|entry| (entry.rule, entry.program, entry.entry, entry.cascade_order))
            .collect();
        orders.sort_unstable_by_key(|&(rule, program, entry, _)| (rule, program, entry));
        orders.dedup_by(|left, right| (left.0, left.1, left.2) == (right.0, right.1, right.2));
        for &node in &live {
            let Some(retained) = engine.retained_match_answer(node).sparse().ok().map(Rc::clone) else {
                continue;
            };
            if !matches!(
                engine.retained_match_answers.cascade_input_lookup(node),
                Lookup::Known(_)
            ) || !engine.match_answer_is_comparable_across_elements(node)
            {
                continue;
            }
            let materialized: Option<Vec<RuleMatch>> = retained
                .iter()
                .copied()
                .map(|entry| {
                    let index = orders
                        .binary_search_by_key(&(entry.rule, entry.program, entry.entry), |&(rule, program, e, _)| {
                            (rule, program, e)
                        })
                        .ok()?;
                    entry.materialize(node, &engine.programs, orders[index].3)
                })
                .collect();
            let Some(materialized) = materialized else {
                continue;
            };
            let expected = engine.in_cascade_order(materialized, false);
            let counters = engine.counters.clone();
            let cold = engine.match_element_for_purpose(node, false);
            engine.counters = counters;
            let cold = cold.expect("cold matching must answer for a resident node");
            assert_eq!(
                expected, cold,
                "retained answer diverged from cold matching for {node:?} after {stage}"
            );
        }
    };
    flush_and_check(&mut engine, &model, "the initial build");
    let (_, dispatch) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    assert_eq!(
        dispatch.prefixes().positional_tests().len(),
        4,
        "the step-free structural tests are admitted while the step-bearing rules stay routed"
    );

    // A trailing arrival: the previously last child keeps its facts but loses
    // `:last-child`, and the previous `:nth-last-child(2)` holder loses that too.
    let trailing = nodes[8];
    let old_last = *model.last().unwrap();
    model.push(trailing);
    engine.record_tree_delta(
        trailing,
        None,
        Some(relations(Some(container.raw()), Some(old_last.raw()), None)),
    );
    record_facts(&mut engine, trailing, class_item);
    flush_and_check(&mut engine, &model, "a trailing arrival");

    // A middle removal: forward parity flips for every following sibling.
    let removed_index = 2;
    let removed = model[removed_index];
    let before = model.clone();
    model.remove(removed_index);
    engine.record_tree_delta(
        removed,
        Some(relations(
            Some(container.raw()),
            Some(before[removed_index - 1].raw()),
            Some(before[removed_index + 1].raw()),
        )),
        None,
    );
    flush_and_check(&mut engine, &model, "a middle removal");

    // A leading arrival: forward parity flips for the whole sequence while no existing
    // child records any input of its own.
    let leading = nodes[9];
    let old_first = model[0];
    model.insert(0, leading);
    engine.record_tree_delta(
        leading,
        None,
        Some(relations(Some(container.raw()), None, Some(old_first.raw()))),
    );
    record_facts(&mut engine, leading, class_item);
    flush_and_check(&mut engine, &model, "a leading arrival");

    // A same-parent move of the first child to the end shifts every position at once.
    let moved = model[0];
    let before = model.clone();
    model.remove(0);
    model.push(moved);
    let new_last_index = model.len() - 1;
    engine.record_tree_delta(
        moved,
        Some(relations(Some(container.raw()), None, Some(before[1].raw()))),
        Some(relations(
            Some(container.raw()),
            Some(model[new_last_index - 1].raw()),
            None,
        )),
    );
    flush_and_check(&mut engine, &model, "a same-parent move");

    // A pure fact flush for good measure: dropping `.item` changes the node's own answers
    // without moving any position.
    remove_feature(&mut engine, model[1], FeatureKey::Class(class_item));
    flush_and_check(&mut engine, &model, "a class removal");

    // A grandchild arrival flips its parent's `:empty` while the parent records no input
    // of its own; the removal flips it back.
    let grandchild = nodes[10];
    let child = model[2];
    engine.record_tree_delta(grandchild, None, Some(relations(Some(child.raw()), None, None)));
    record_facts(&mut engine, grandchild, class_item);
    flush_and_check(&mut engine, &model, "a grandchild arrival");
    engine.record_tree_delta(grandchild, Some(relations(Some(child.raw()), None, None)), None);
    flush_and_check(&mut engine, &model, "a grandchild departure");
}

/// An automaton refuses chains whose positional tests would overflow its 32 truth bits;
/// the refused entries stay with the exact evaluator and keep answering correctly.
#[test]
fn positional_test_overflow_refuses_admission_without_erasing_answers() {
    const CHILDREN: usize = 40;
    let tag = StyleAtomID(100);
    let class_item = StyleAtomID(200);

    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = vec![0_u32; 2 + CHILDREN];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let root = nodes[0];
    let container = nodes[1];
    let children = &nodes[2..];

    for position in 1..=CHILDREN {
        let mut builder = selector::SelectorProgramBuilder::new();
        let item = builder.push_feature(selector::FeatureTest::Class(class_item));
        let exact = builder.push(selector::SelectorOp::NthPosition(selector::NthPosition {
            step: 0,
            offset: i32::try_from(position).unwrap(),
            from_end: false,
            of_selector: None,
            of_type: false,
        }));
        let selector = builder.push_compound(&[item, exact]);
        builder.push_entry(selector);
        let program = engine.programs.add(builder.finish());
        let sheet = engine.add_sheet(StyleSheetObjectID(1000 + position as u32), CascadeOrigin::Author);
        engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        let rule = engine.append_rule(sheet, None, RuleKind::Style);
        engine.add_routing_rule(rule, program);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(program);
        version.declaration_block = Some(DeclarationBlockID(1000 + position as u32));
        engine.replace_rule_version(rule, version);
    }

    engine.record_tree_delta(root, None, Some(relations(None, None, None)));
    engine.record_tree_delta(container, None, Some(relations(Some(root.raw()), None, None)));
    for (index, &child) in children.iter().enumerate() {
        let previous = (index > 0).then(|| children[index - 1].raw());
        let next = children.get(index + 1).map(|node| node.raw());
        engine.record_tree_delta(child, None, Some(relations(Some(container.raw()), previous, next)));
    }
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for &child in children {
        add_feature(&mut engine, child, FeatureKey::Class(class_item));
    }
    discard_transaction(&mut engine);

    let (_, dispatch) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    assert_eq!(
        dispatch.prefixes().positional_tests().len(),
        32,
        "the automaton fills its bit space and refuses the rest"
    );
    for (index, &child) in children.iter().enumerate() {
        let matches = engine.match_element(child).unwrap();
        assert_eq!(
            matches.len(),
            1,
            "child {index} matches exactly its own exact-position rule"
        );
    }
}

#[test]
fn consecutive_departures_reconstruct_one_old_sibling_sequence() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_double_guard_adjacent_target_rule(&mut engine, guard, target);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(
                Some(nodes[0].raw()),
                (index > 1).then(|| nodes[index - 1].raw()),
                None,
            )),
        );
    }
    for node in &nodes[1..4] {
        add_feature(&mut engine, *node, FeatureKey::Class(guard));
    }
    add_feature(&mut engine, nodes[4], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[2],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[3].raw()),
        )),
        None,
    );
    engine.record_tree_delta(
        nodes[3],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[4].raw()),
        )),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[4].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn converging_departure_routes_are_folded_before_exact_tree_evaluation() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(
                Some(nodes[0].raw()),
                (index > 1).then(|| nodes[index - 1].raw()),
                None,
            )),
        );
    }
    for node in &nodes[1..4] {
        add_feature(&mut engine, *node, FeatureKey::Class(guard));
    }
    add_feature(&mut engine, nodes[4], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
        None,
    );
    engine.record_tree_delta(
        nodes[2],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[3].raw()))),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.is_empty());
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn cyclic_departure_snapshots_do_not_form_before_sibling_relations() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let mut transaction = StyleTransaction::default();
    transaction.inputs.extend([
        NormalizedInput {
            key: InputKey::TreeRelations(nodes[1]),
            old: InputValue::TreeRelations(Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw())))),
            new: InputValue::TreeRelations(None),
        },
        NormalizedInput {
            key: InputKey::TreeRelations(nodes[2]),
            old: InputValue::TreeRelations(Some(relations(Some(nodes[0].raw()), None, Some(nodes[1].raw())))),
            new: InputValue::TreeRelations(None),
        },
    ]);

    let mut view = engine.transaction_fact_view_for(&mut transaction, nodes[0], &ImpactRegions::new());
    assert!(!engine.populate_before_sibling_relations(&mut view, &transaction, &[]));
}

#[test]
fn ambiguous_departure_snapshots_do_not_guess_an_old_sibling_order() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let mut transaction = StyleTransaction::default();
    transaction
        .inputs
        .extend(nodes[1..].iter().map(|&node| NormalizedInput {
            key: InputKey::TreeRelations(node),
            old: InputValue::TreeRelations(Some(relations(Some(nodes[0].raw()), None, None))),
            new: InputValue::TreeRelations(None),
        }));

    let mut view = engine.transaction_fact_view_for(&mut transaction, nodes[0], &ImpactRegions::new());
    assert!(!engine.populate_before_sibling_relations(&mut view, &transaction, &[]));
}

#[test]
fn a_departed_sibling_reaches_only_the_following_sibling_forest() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 8];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let guard = StyleAtomID(200);
    let container = StyleAtomID(201);
    let target = StyleAtomID(202);
    add_nested_sibling_target_rule(&mut engine, StyleSheetObjectID(1), guard, container, target);

    // root -> [before -> before_target, guard, after -> after_target,
    //          later -> later_target]
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for (node, previous) in [
        (nodes[1], None),
        (nodes[3], Some(nodes[1])),
        (nodes[4], Some(nodes[3])),
        (nodes[6], Some(nodes[4])),
    ] {
        engine.record_tree_delta(
            node,
            None,
            Some(relations(Some(nodes[0].raw()), previous.map(StyleNodeID::raw), None)),
        );
    }
    for (target_node, parent) in [(nodes[2], nodes[1]), (nodes[5], nodes[4]), (nodes[7], nodes[6])] {
        engine.record_tree_delta(target_node, None, Some(relations(Some(parent.raw()), None, None)));
    }
    for node in [nodes[1], nodes[4], nodes[6]] {
        add_feature(&mut engine, node, FeatureKey::Class(container));
    }
    for node in [nodes[2], nodes[5], nodes[7]] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(guard));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[3],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[4].raw()),
        )),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[5].raw(), nodes[7].raw()]);
}

#[test]
fn a_stationary_general_sibling_ignores_its_new_immediate_neighbour() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(
        nodes[1],
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
    );
    engine.record_tree_delta(
        nodes[2],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[1].raw()), None)),
    );
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[2], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    // The unrelated middle node arrives immediately before the target. The guard remains in
    // its preceding-sibling set, so `guard ~ target` has exactly the same answer.
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[2].raw()),
        )),
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
}

#[test]
fn a_stationary_predecessor_is_not_a_moved_place() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(
        nodes[1],
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
    );
    engine.record_tree_delta(
        nodes[2],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[1].raw()), None)),
    );
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[2], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[2].raw()),
        )),
    );
    set_atom_feature(&mut engine, inserted, FeatureKey::TagName, StyleAtomID(100));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
}

#[test]
fn an_arriving_adjacent_sibling_that_fails_its_compound_changes_nothing() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(
        nodes[1],
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
    );
    engine.record_tree_delta(
        nodes[2],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[1].raw()), None)),
    );
    let guard = StyleAtomID(200);
    let also = StyleAtomID(201);
    let target = StyleAtomID(202);
    add_two_class_adjacent_target_rule(&mut engine, guard, also, target);
    // The arriving predecessor carries the class the entry dispatches on and not the one it
    // also requires, so it cannot satisfy the compound and the target's answer cannot move.
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[2].raw()),
        )),
    );
    add_feature(&mut engine, inserted, FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
}

#[test]
fn an_arriving_sibling_with_an_existing_witness_restyles_only_itself() {
    let (mut engine, nodes) = linear_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    add_feature(&mut engine, inserted, FeatureKey::Class(guard));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[2].raw()),
            Some(nodes[3].raw()),
        )),
    );
    add_feature(&mut engine, inserted, FeatureKey::ArrivingFacts);

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn exact_candidate_checks_restore_every_old_fact_in_the_transaction() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for node in [nodes[1], nodes[2]] {
        add_feature(&mut engine, node, FeatureKey::Class(guard));
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    for node in [nodes[1], nodes[2]] {
        remove_feature(&mut engine, node, FeatureKey::Class(guard));
    }
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
}

#[test]
fn a_sibling_entry_ask_seeds_its_left_context() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for (node, class) in [(nodes[2], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let evaluations_before = engine.counters().get(Counter::ColdNodesEvaluated);
    let checks_before = engine.counters().get(Counter::CandidateChecks);
    assert_eq!(engine.match_element(nodes[3]).unwrap().len(), 1);
    assert_eq!(
        engine.counters().get(Counter::ColdNodesEvaluated) - evaluations_before,
        1,
        "a seeded left context lets the automaton answer in one pass"
    );
    assert_eq!(
        engine.counters().get(Counter::CandidateChecks) - checks_before,
        0,
        "the sibling entry is answered by the automaton, not the fallback"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_general_sibling_fact_miss_is_batched_before_matching_restarts() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_fallback_rule(&mut engine, guard, target);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for (node, class) in [(nodes[2], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let evaluations_before = engine.counters().get(Counter::ColdNodesEvaluated);
    assert_eq!(engine.match_element(nodes[3]).unwrap().len(), 1);
    assert_eq!(
        engine.counters().get(Counter::ColdNodesEvaluated) - evaluations_before,
        2,
        "the first pass requests the sequence and the second completes"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_descendant_fact_miss_is_batched_before_matching_restarts() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 65];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let tag = StyleAtomID(100);
    let anchor = StyleAtomID(200);
    let absent_witness = StyleAtomID(201);
    add_has_descendant_rule(&mut engine, anchor, absent_witness);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        let previous = (index > 1).then(|| nodes[index - 1].raw());
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(nodes[0].raw()), previous, None)),
        );
    }
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    add_feature(&mut engine, nodes[0], FeatureKey::Class(anchor));
    discard_transaction(&mut engine);

    let evaluations_before = engine.counters().get(Counter::ColdNodesEvaluated);
    let feature_tests_before = engine.counters().get(Counter::LocalFeatureTests);
    assert!(engine.match_element(nodes[0]).unwrap().is_empty());
    assert_eq!(
        engine.counters().get(Counter::ColdNodesEvaluated) - evaluations_before,
        5,
        "doubling windows cover 64 descendants in four retries"
    );
    assert!(
        engine.counters().get(Counter::LocalFeatureTests) - feature_tests_before < 200,
        "restarts do linear rather than triangular feature-test work"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_broad_matching_batch_shares_facts_between_element_asks() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_sibling_target_rule(&mut engine, guard, target);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for (node, class) in [(nodes[2], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let ordinary = engine.match_element(nodes[3]).unwrap();
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    let batch_bytes = engine.memory().bytes_in_category(MemoryCategory::BatchScratch);
    assert!(batch_bytes > 0);
    assert_eq!(engine.counters().get(Counter::ColdMatchingBatchRows), 4);

    let evaluations_before = engine.counters().get(Counter::ColdNodesEvaluated);
    let batched = engine.match_element(nodes[3]).unwrap();
    assert_eq!(batched, ordinary);
    assert_eq!(
        engine.counters().get(Counter::ColdNodesEvaluated) - evaluations_before,
        1,
        "the shared batch already holds the preceding sibling's facts"
    );

    let ask_bytes = engine.memory().bytes_in_category(MemoryCategory::BatchScratch);
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    // Only the retained prefix caches carry over into the new traversal, so even with the
    // fresh batch just charged the footprint sits below the previous ask-time peak.
    assert!(
        engine.memory().bytes_in_category(MemoryCategory::BatchScratch) < ask_bytes,
        "starting another batch releases the previous one first"
    );
    engine.end_cold_matching_batch();
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_broad_matching_batch_includes_shadow_scope_roots() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let [document_root, host, shadow_root, child] = nodes.as_slice() else {
        unreachable!()
    };
    let scope = TreeScopeID(1);

    engine.record_tree_delta(*document_root, None, Some(relations(None, None, None)));
    engine.record_tree_delta(*host, None, Some(relations(Some(document_root.raw()), None, None)));
    engine.record_tree_delta(*shadow_root, None, Some(TreeRelations::detached(scope)));
    engine.record_tree_delta(
        *child,
        None,
        Some(TreeRelations {
            parent: Some(*shadow_root),
            tree_scope: scope,
            ..TreeRelations::detached(scope)
        }),
    );
    engine.set_shadow_root(*host, *shadow_root);
    engine.set_tree_scope_root(scope, *shadow_root);
    for node in [*document_root, *host, *child] {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }
    discard_transaction(&mut engine);

    let batch = engine.materialize_cold_matching_batch(*document_root, None).unwrap();
    assert_eq!(batch.row_count(), 4);
    assert!(batch.row_of(*shadow_root).is_some());
}

#[test]
fn departing_scope_roots_are_removed_from_the_reverse_scope_index() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let first_root = StyleNodeID::from_raw(raw[0]).unwrap();
    let second_root = StyleNodeID::from_raw(raw[1]).unwrap();
    let scope = TreeScopeID(1);

    engine.record_tree_delta(first_root, None, Some(TreeRelations::detached(scope)));
    engine.set_tree_scope_root(scope, first_root);
    assert_eq!(engine.scope_by_root.get(&first_root), Some(&scope));

    engine.record_tree_delta(second_root, None, Some(TreeRelations::detached(scope)));
    engine.set_tree_scope_root(scope, second_root);
    assert!(!engine.scope_by_root.contains_key(&first_root));
    assert_eq!(engine.scope_by_root.get(&second_root), Some(&scope));
    discard_transaction(&mut engine);

    engine.record_tree_delta(second_root, Some(TreeRelations::detached(scope)), None);
    discard_transaction(&mut engine);
    assert!(!engine.scope_by_root.contains_key(&second_root));
    assert_eq!(engine.scope_roots[scope.0 as usize], None);
}

#[test]
fn independent_sibling_paths_request_their_fact_ranges_together() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let tag = StyleAtomID(100);
    let outer_guard = StyleAtomID(200);
    let container = StyleAtomID(201);
    let inner_guard = StyleAtomID(202);
    let target = StyleAtomID(203);
    add_guard_sibling_target_fallback_rule(&mut engine, inner_guard, target);
    add_nested_sibling_target_fallback_rule(&mut engine, StyleSheetObjectID(2), outer_guard, container, target);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(nodes[0].raw()), None, None)));
    engine.record_tree_delta(
        nodes[2],
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[1].raw()), None)),
    );
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(nodes[2].raw()), None, None)));
    engine.record_tree_delta(
        nodes[4],
        None,
        Some(relations(Some(nodes[2].raw()), Some(nodes[3].raw()), None)),
    );
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for (node, class) in [
        (nodes[1], outer_guard),
        (nodes[2], container),
        (nodes[3], inner_guard),
        (nodes[4], target),
    ] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let evaluations_before = engine.counters().get(Counter::ColdNodesEvaluated);
    let checks_before = engine.counters().get(Counter::CandidateChecks);
    assert_eq!(engine.match_element(nodes[4]).unwrap().len(), 2);
    assert_eq!(
        engine.counters().get(Counter::ColdNodesEvaluated) - evaluations_before,
        2,
        "both sibling sequences are requested by the first pass"
    );
    assert_eq!(engine.counters().get(Counter::CandidateChecks) - checks_before, 4);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn completed_cold_candidates_are_not_replayed_after_a_fact_miss() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let missing = StyleAtomID(202);
    add_target_rule(&mut engine, StyleSheetObjectID(2), target);
    add_two_class_target_rule(&mut engine, StyleSheetObjectID(3), target, missing);
    add_guard_sibling_target_fallback_rule(&mut engine, guard, target);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for (node, class) in [(nodes[2], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let checks_before = engine.counters().get(Counter::CandidateChecks);
    let matches_before = engine.counters().get(Counter::RuleMatchesEmitted);
    assert_eq!(engine.match_element(nodes[3]).unwrap().len(), 2);
    assert_eq!(
        engine.counters().get(Counter::CandidateChecks) - checks_before,
        2,
        "prefix matching settles the local rule and completed rules are skipped on retry"
    );
    assert_eq!(
        engine.counters().get(Counter::RuleMatchesEmitted) - matches_before,
        2,
        "a retained match is emitted once"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_selector_list_merges_matches_retained_across_retries() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let extra = StyleAtomID(202);
    add_retrying_selector_list_rule(&mut engine, guard, target, extra);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    for (node, class) in [(nodes[2], guard), (nodes[3], target), (nodes[3], extra)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    let checks_before = engine.counters().get(Counter::CandidateChecks);
    let emitted_before = engine.counters().get(Counter::RuleMatchesEmitted);
    let matches = engine.match_element(nodes[3]).unwrap();
    assert_eq!(matches.len(), 1);
    assert_eq!(matches[0].entry, 1, "the more specific selector-list entry wins");
    assert_eq!(engine.counters().get(Counter::CandidateChecks) - checks_before, 2);
    assert_eq!(
        engine.counters().get(Counter::RuleMatchesEmitted) - emitted_before,
        1,
        "the retained lower-specificity match is updated rather than emitted again"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_positional_fact_miss_is_batched_before_matching_restarts() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let target = StyleAtomID(201);
    add_nth_of_type_target_rule(&mut engine, target, 0, 2);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
    }
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let evaluations_before = engine.counters().get(Counter::ColdNodesEvaluated);
    assert_eq!(engine.match_element(nodes[2]).unwrap().len(), 1);
    assert_eq!(
        engine.counters().get(Counter::ColdNodesEvaluated) - evaluations_before,
        2,
        "the first pass requests the sequence and the second completes"
    );
}

/// A sibling request names the whole rest of a sequence, but taking all of it at once costs a
/// row per preceding sibling, which is quadratic over a long list. The window bounds what one
/// retry takes and doubles so a scan that really reads its whole sequence still converges.
#[test]
fn a_sibling_fact_request_is_taken_a_doubling_window_at_a_time() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 65];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..nodes.len() {
        let previous = (index > 1).then(|| nodes[index - 1].raw());
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(Some(nodes[0].raw()), previous, None)),
        );
    }
    discard_transaction(&mut engine);

    // Each retry resumes at the first sibling the previous window did not reach, so the batch
    // grows by 8, then 16, then 32 rather than taking all 64 at the first miss.
    let mut covered: Vec<StyleNodeID> = Vec::new();
    let mut window = INITIAL_SIBLING_FACT_WINDOW;
    for expected in [8, 24, 56] {
        let first_uncovered = *nodes[1..].iter().find(|node| !covered.contains(*node)).unwrap();
        let request = Incomplete::MissingSiblingFacts {
            first: first_uncovered,
            last_exclusive: None,
        };
        assert!(engine.widen_fact_coverage(&mut covered, request, &mut window).is_ok());
        assert_eq!(covered.len(), expected);
    }

    // A row the store cannot supply is asked for once and then reported, so a retry that cannot
    // make progress ends rather than looping.
    let repeated = Incomplete::MissingSiblingFacts {
        first: nodes[1],
        last_exclusive: None,
    };
    assert_eq!(
        engine.widen_fact_coverage(&mut covered, repeated, &mut window),
        Err(nodes[1])
    );
}

fn typed_nth_of_type_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let first_namespace = StyleAtomID(300);
    let other_namespace = StyleAtomID(301);
    let target = StyleAtomID(201);
    add_nth_of_type_target_rule(&mut engine, target, 0, 2);

    for (node, namespace) in [
        (nodes[1], first_namespace),
        (nodes[2], other_namespace),
        (nodes[3], first_namespace),
    ] {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, tag);
        engine.set_element_namespace(node, namespace);
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);
    (engine, nodes)
}

#[test]
fn another_type_leaving_does_not_route_an_of_type_position() {
    let (mut engine, nodes) = typed_nth_of_type_document();
    engine.record_tree_delta(
        nodes[2],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[3].raw()),
        )),
        None,
    );

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert!(planned.is_empty());
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn the_same_type_leaving_routes_an_of_type_position() {
    let (mut engine, nodes) = typed_nth_of_type_document();
    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
        None,
    );

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

fn insert_typed_child(engine: &mut StyleEngine, nodes: &[StyleNodeID], namespace: StyleAtomID) -> StyleNodeID {
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[2].raw()),
            Some(nodes[3].raw()),
        )),
    );
    set_atom_feature(engine, inserted, FeatureKey::TagName, StyleAtomID(100));
    engine.set_element_namespace(inserted, namespace);
    inserted
}

#[test]
fn another_type_arriving_does_not_route_an_of_type_position() {
    let (mut engine, nodes) = typed_nth_of_type_document();
    let inserted = insert_typed_child(&mut engine, &nodes, StyleAtomID(301));

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
}

#[test]
fn the_same_type_arriving_routes_an_of_type_position() {
    let (mut engine, nodes) = typed_nth_of_type_document();
    let inserted = insert_typed_child(&mut engine, &nodes, StyleAtomID(300));

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw(), inserted.raw()]);
}

#[test]
fn an_arrival_rejects_an_unchanged_of_type_position() {
    let (mut engine, nodes) = linear_document();
    let tag = StyleAtomID(100);
    let namespace = StyleAtomID(300);
    let target = StyleAtomID(201);
    add_nth_of_type_target_rule(&mut engine, target, 4, 0);
    for &(node, node_tag) in &[(nodes[1], tag), (nodes[2], StyleAtomID(101)), (nodes[3], tag)] {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, node_tag);
        engine.set_element_namespace(node, namespace);
    }
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    set_atom_feature(&mut engine, inserted, FeatureKey::TagName, tag);
    engine.set_element_namespace(inserted, namespace);
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[2].raw()),
            Some(nodes[3].raw()),
        )),
    );
    add_feature(&mut engine, inserted, FeatureKey::ArrivingFacts);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn an_arrival_with_no_type_facts_keeps_of_type_routing_conservative() {
    let (mut engine, nodes) = typed_nth_of_type_document();
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[2].raw()),
            Some(nodes[3].raw()),
        )),
    );

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw(), inserted.raw()]);
}

fn nth_target_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(201);
    add_nth_target_rule(&mut engine, target, 3, 0);
    for &node in &nodes[1..] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    discard_transaction(&mut engine);
    (engine, nodes)
}

#[test]
fn a_departure_routes_only_an_plus_b_positions_that_can_flip() {
    let (mut engine, nodes) = nth_target_document();
    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
        None,
    );

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
}

#[test]
fn an_arrival_routes_only_an_plus_b_positions_that_can_flip() {
    let (mut engine, nodes) = nth_target_document();
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[1].raw()))),
    );

    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[2].raw(), nodes[3].raw(), inserted.raw()]);
}

#[test]
fn cancelling_mutations_route_only_an_plus_b_positions_that_flipped() {
    let (mut engine, nodes) = nth_target_document();
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    // The new head arrives while the old head departs, so every following sibling keeps
    // its position. The aggregate arrival-and-departure range cannot prove that, but the
    // retained before-side positions can, and only the arrival itself is planned.
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[1].raw()))),
    );
    engine.record_tree_delta(
        nodes[1],
        Some(relations(
            Some(nodes[0].raw()),
            Some(inserted.raw()),
            Some(nodes[2].raw()),
        )),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![inserted.raw()]);
}

#[test]
fn exact_planning_shares_current_relation_indexes_with_matching() {
    let (mut engine, nodes) = nth_target_document();
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let inserted = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(
        inserted,
        None,
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[1].raw()))),
    );
    set_atom_feature(&mut engine, inserted, FeatureKey::TagName, StyleAtomID(100));

    assert!(engine.take_style_transaction(nodes[0], |_, _, _| {}));
    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    assert_eq!(engine.match_element(nodes[2]).unwrap().len(), 1);
    engine.end_cold_matching_batch();
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn scoped_planning_does_not_prepare_a_complete_matching_batch() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(201);
    add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    discard_transaction(&mut engine);

    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));

    let inspected_before = engine
        .counters()
        .get(Counter::PreparedMatchingBatchCompletenessRowsInspected);
    let cloned_before = engine.counters().get(Counter::PreparedMatchingBatchRowsCloned);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert_eq!(planned, vec![nodes[2].raw()]);
    assert_eq!(
        engine
            .counters()
            .get(Counter::PreparedMatchingBatchCompletenessRowsInspected),
        inspected_before
    );
    assert_eq!(
        engine.counters().get(Counter::PreparedMatchingBatchRowsCloned),
        cloned_before
    );
}

#[test]
fn a_published_local_reaction_names_its_semantic_provenance() {
    let (mut engine, nodes) = nested_document();
    let target = StyleAtomID(201);
    add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    discard_transaction(&mut engine);
    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    assert_eq!(engine.match_element_for_cascade(nodes[2]).unwrap().len(), 1);
    engine.end_cold_matching_batch();

    remove_feature(&mut engine, nodes[2], FeatureKey::Class(target));

    let direct_before = engine.counters().get(Counter::PlannedNodesWithDirectAction);
    let signed_before = engine.counters().get(Counter::PlannedNodesWithSignedDelta);
    let output_before = engine.counters().get(Counter::PlannedNodesWithOutputChange);
    let upquery_before = engine.counters().get(Counter::PlannedNodesWithUpquery);
    let unattributed_before = engine.counters().get(Counter::PlannedNodesUnattributed);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert_eq!(planned, vec![nodes[2].raw()]);
    assert_eq!(
        engine.counters().get(Counter::PlannedNodesWithDirectAction),
        direct_before + 1
    );
    assert_eq!(
        engine.counters().get(Counter::PlannedNodesWithSignedDelta),
        signed_before + 1
    );
    assert_eq!(
        engine.counters().get(Counter::PlannedNodesWithOutputChange),
        output_before + 1
    );
    assert_eq!(engine.counters().get(Counter::PlannedNodesWithUpquery), upquery_before);
    assert_eq!(
        engine.counters().get(Counter::PlannedNodesUnattributed),
        unattributed_before
    );
}

#[test]
fn an_element_style_input_publishes_an_exact_reaction_without_matching() {
    let (mut engine, nodes) = nested_document();
    discard_transaction(&mut engine);
    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    assert!(engine.match_element_for_cascade(nodes[2]).unwrap().is_empty());
    engine.end_cold_matching_batch();

    engine.record_input(
        InputKey::ElementStyleInput(nodes[2]),
        InputValue::ElementStyleInput {
            reaction: 0,
            inherited_style_groups: 0,
        },
        InputValue::ElementStyleInput {
            reaction: transaction::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0b101,
        },
    );

    let match_calls_before = engine
        .counters()
        .get(Counter::MatchElementCallsDuringPublishedStyleTransaction);
    let upqueries_before = engine.counters().get(Counter::PlannedNodesWithUpquery);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(
            reactions
                .iter()
                .map(|reaction| (reaction.style_node, reaction.inherited_style_groups)),
        );
    }));
    assert_eq!(planned, vec![(nodes[2].raw(), 0b101)]);
    assert_eq!(
        engine
            .counters()
            .get(Counter::MatchElementCallsDuringPublishedStyleTransaction),
        match_calls_before
    );
    assert_eq!(
        engine.counters().get(Counter::PlannedNodesWithUpquery),
        upqueries_before
    );
}

#[test]
fn published_match_answers_name_transaction_program_and_identity() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(201);
    let rule = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    discard_transaction(&mut engine);

    add_feature(&mut engine, nodes[1], FeatureKey::Class(target));

    let expected_program_version = engine.program.version();
    let mut published = Vec::new();
    assert!(
        engine.take_style_transaction(nodes[0], |transaction_version, program_version, reactions| {
            assert_eq!(transaction_version, StyleTransactionVersion(1));
            assert_eq!(program_version, expected_program_version);
            published.extend_from_slice(reactions);
        },)
    );
    assert_eq!(published.len(), 1);
    assert_eq!(published[0].style_node, nodes[1].raw());
    assert_ne!(published[0].match_answer, 0);
    assert_eq!(engine.counters().get(Counter::PublishedMatchAnswerRecords), 1);

    engine.begin_adaptive_cold_matching_batch(nodes[0]);
    assert_eq!(engine.match_element_for_cascade(nodes[1]).unwrap().len(), 1);
    assert_eq!(
        engine.match_element_signature(nodes[1]),
        Some(published[0].match_answer)
    );
    engine.end_cold_matching_batch();

    engine.record_input(
        InputKey::ElementStyleInput(nodes[1]),
        InputValue::ElementStyleInput {
            reaction: 0,
            inherited_style_groups: 0,
        },
        InputValue::ElementStyleInput {
            reaction: transaction::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        },
    );
    let mut next_transaction_version = None;
    assert!(
        engine.take_style_transaction(nodes[0], |transaction_version, program_version, reactions| {
            next_transaction_version = Some(transaction_version);
            assert_eq!(program_version, expected_program_version);
            assert_eq!(reactions.len(), 1);
            assert_ne!(reactions[0].match_answer, 0);
        },)
    );
    assert_eq!(next_transaction_version, Some(StyleTransactionVersion(2)));
    assert_eq!(engine.counters().get(Counter::PublishedMatchAnswerRecords), 2);
    assert_eq!(
        engine
            .counters()
            .get(Counter::PublishedMatchAnswerRetainedIdentityComparisons),
        1
    );
    assert_eq!(
        engine
            .counters()
            .get(Counter::PublishedMatchAnswerRetainedIdentityMatches),
        1
    );
}

#[test]
fn a_departed_node_routes_from_retained_facts_without_being_published() {
    let (mut engine, nodes) = nested_document();
    discard_transaction(&mut engine);

    engine.record_input(
        InputKey::ElementStyleInput(nodes[3]),
        InputValue::ElementStyleInput {
            reaction: 0,
            inherited_style_groups: 0,
        },
        InputValue::ElementStyleInput {
            reaction: transaction::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        },
    );
    engine.record_tree_delta(nodes[3], Some(relations(Some(nodes[2].raw()), None, None)), None);

    let mut published = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        published.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert!(!published.contains(&nodes[3].raw()));
}

#[test]
fn a_shadow_root_routes_without_being_published_as_a_style_output() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let host = StyleNodeID::from_raw(raw[0]).unwrap();
    let shadow_root = StyleNodeID::from_raw(raw[1]).unwrap();
    engine.record_tree_delta(host, None, Some(relations(None, None, None)));
    engine.record_tree_delta(shadow_root, None, Some(relations(Some(host.raw()), None, None)));
    engine.set_tree_scope_root(TreeScopeID(1), shadow_root);
    for node in [host, shadow_root] {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }
    discard_transaction(&mut engine);

    engine.record_input(
        InputKey::ElementStyleInput(shadow_root),
        InputValue::ElementStyleInput {
            reaction: 0,
            inherited_style_groups: 0,
        },
        InputValue::ElementStyleInput {
            reaction: transaction::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        },
    );

    let mut published = Vec::new();
    assert!(engine.take_style_transaction(host, |_, _, reactions| {
        published.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert!(!published.contains(&shadow_root.raw()));
}

#[test]
fn exact_planning_carries_preorder_topology_into_matching() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    for offset in 0..31 {
        add_feature(&mut engine, nodes[0], FeatureKey::Attribute(StyleAtomID(300 + offset)));
    }
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch) > 0);

    assert!(engine.begin_cold_matching_batch(nodes[0]));
    assert!(engine.match_element(nodes[3]).unwrap().is_empty());
    engine.end_cold_matching_batch();
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn sequence_routing_rejects_children_outside_the_positional_compound() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 8];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let guard = StyleAtomID(200);
    let also = StyleAtomID(201);
    let target = StyleAtomID(202);
    add_guarded_nth_target_rule(&mut engine, guard, also, target);

    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(nodes[0].raw()), None, None)));
    for (container, previous, descendant) in [
        (nodes[2], nodes[1], nodes[3]),
        (nodes[4], nodes[2], nodes[5]),
        (nodes[6], nodes[4], nodes[7]),
    ] {
        engine.record_tree_delta(
            container,
            None,
            Some(relations(Some(nodes[0].raw()), Some(previous.raw()), None)),
        );
        engine.record_tree_delta(descendant, None, Some(relations(Some(container.raw()), None, None)));
        add_feature(&mut engine, descendant, FeatureKey::Class(target));
    }
    for class in [guard, also] {
        add_feature(&mut engine, nodes[2], FeatureKey::Class(class));
    }
    add_feature(&mut engine, nodes[4], FeatureKey::Class(guard));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
        None,
    );
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
}

#[test]
fn an_exact_batch_filters_a_featureless_subject() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    add_guard_universal_rule(&mut engine, guard);
    for node in [nodes[1], nodes[2]] {
        add_feature(&mut engine, node, FeatureKey::Class(guard));
    }
    discard_transaction(&mut engine);

    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[2].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn ancestor_requirement_scratch_is_released_after_document_matching() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    add_guard_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    assert_eq!(engine.match_document(nodes[0]), Ok(1));
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn identical_sheet_sets_share_a_scope_program() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = [0_u32; 7];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let [
        document_root,
        first_host,
        second_host,
        first_shadow_root,
        first_child,
        second_shadow_root,
        second_child,
    ] = nodes.as_slice()
    else {
        unreachable!()
    };
    let first_scope = TreeScopeID(1);
    let second_scope = TreeScopeID(2);

    engine.record_tree_delta(*document_root, None, Some(relations(None, None, None)));
    engine.record_tree_delta(
        *first_host,
        None,
        Some(relations(Some(document_root.raw()), None, Some(second_host.raw()))),
    );
    engine.record_tree_delta(
        *second_host,
        None,
        Some(relations(Some(document_root.raw()), Some(first_host.raw()), None)),
    );
    for (host, root, child, scope) in [
        (*first_host, *first_shadow_root, *first_child, first_scope),
        (*second_host, *second_shadow_root, *second_child, second_scope),
    ] {
        engine.record_tree_delta(root, None, Some(TreeRelations::detached(scope)));
        engine.record_tree_delta(
            child,
            None,
            Some(TreeRelations {
                parent: Some(root),
                tree_scope: scope,
                ..TreeRelations::detached(scope)
            }),
        );
        engine.set_shadow_root(host, root);
        engine.set_tree_scope_root(scope, root);
        set_atom_feature(&mut engine, child, FeatureKey::TagName, StyleAtomID(100));
        add_feature(&mut engine, child, FeatureKey::Class(StyleAtomID(200)));
    }
    for node in [*document_root, *first_host, *second_host] {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }

    let program = engine
        .programs
        .add(test_selector_program(".target", &[("target", StyleAtomID(200))]));
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, first_scope);
    engine.attach_sheet(sheet, second_scope);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    engine.replace_rule_version(rule, version);
    discard_transaction(&mut engine);

    assert!(engine.begin_cold_matching_batch(*document_root));
    let first_cached = engine.match_element_for_cascade(*first_child).unwrap();
    let second_cached = engine.match_element_for_cascade(*second_child).unwrap();
    assert_eq!(first_cached.len(), 1);
    assert_eq!(second_cached.len(), 1);
    assert_eq!(first_cached[0].tree_scope, first_scope);
    assert_eq!(second_cached[0].tree_scope, second_scope);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheMisses), 1);
    assert_eq!(engine.counters().get(Counter::PrefixAnswerCacheHits), 1);
    engine.end_cold_matching_batch();

    let first = engine.match_element(*first_child).unwrap();
    let second = engine.match_element(*second_child).unwrap();
    assert_eq!(first.len(), 1);
    assert_eq!(second.len(), 1);
    assert_eq!(first[0].tree_scope, first_scope);
    assert_eq!(second[0].tree_scope, second_scope);
    engine.remember_retained_match_answer(*first_child, &first);
    assert!(matches!(engine.retained_match_answer(*first_child), Lookup::Known(_)));
    let reuses_before = engine.counters().get(Counter::RetainedMatchAnswerReuses);
    engine.begin_published_match_answer_completion_batch(*document_root, false);
    let published = engine.complete_published_match_answer(*first_child, None).unwrap();
    engine.end_published_match_answer_completion_batch();
    assert_eq!(
        engine.counters().get(Counter::RetainedMatchAnswerReuses),
        reuses_before + 1
    );
    assert_eq!(published.matches.as_deref(), Some(first.as_slice()));
    assert_eq!(published.cascade_input, None);
    assert_eq!(engine.match_element_signature(*first_child), None);
    // The batch also ranks the document scope for the ancestor spine, so three concrete
    // scopes retain programs while the two shadow scopes share one immutable program.
    assert_eq!(engine.scope_program_by_scope.iter().flatten().count(), 3);
    let shared_program = engine.scope_program_by_scope[first_scope.0 as usize].unwrap().1;
    assert_eq!(
        shared_program,
        engine.scope_program_by_scope[second_scope.0 as usize].unwrap().1
    );
    assert_eq!(
        engine.scope_programs.iter().flatten().count(),
        2,
        "the concrete shadow scopes retain one shared immutable program beside the document's"
    );
    assert_eq!(engine.held_scope_program.map(|(_, _, id)| id), Some(shared_program));

    let second_sheet = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    engine.attach_sheet(second_sheet, second_scope);
    let second_rule = engine.append_rule(second_sheet, None, RuleKind::Style);
    engine.add_routing_rule(second_rule, program);
    let mut second_version = engine.program.rule_version(second_rule);
    second_version.selector_program = Some(program);
    engine.replace_rule_version(second_rule, second_version);
    discard_transaction(&mut engine);

    assert_eq!(engine.match_element(*first_child).unwrap().len(), 1);
    assert_eq!(engine.match_element(*second_child).unwrap().len(), 2);
    assert_ne!(
        engine.scope_program_by_scope[first_scope.0 as usize].unwrap().1,
        engine.scope_program_by_scope[second_scope.0 as usize].unwrap().1
    );
    assert_eq!(
        engine.scope_programs.iter().flatten().count(),
        3,
        "a scope that changes its effective sheet set gets a distinct program"
    );
    assert_eq!(
        engine.held_scope_program.map(|(_, _, id)| id),
        Some(engine.scope_program_by_scope[second_scope.0 as usize].unwrap().1)
    );
}

#[test]
fn equivalent_sheet_programs_share_dispatch_topology() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let selector_program = engine
        .programs
        .add(test_selector_program(".target", &[("target", StyleAtomID(200))]));
    let mut rules = Vec::new();
    for (scope, object) in [(TreeScopeID(1), 1), (TreeScopeID(2), 2)] {
        let sheet = engine.add_sheet(StyleSheetObjectID(object), CascadeOrigin::Author);
        engine.attach_sheet(sheet, scope);
        let rule = engine.append_rule(sheet, None, RuleKind::Style);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(selector_program);
        engine.replace_rule_version(rule, version);
        rules.push(rule);
    }
    discard_transaction(&mut engine);

    let (_, first) = engine.ranked_scope_program(TreeScopeID(1));
    let (_, second) = engine.ranked_scope_program(TreeScopeID(2));
    assert!(!Rc::ptr_eq(&first, &second));
    assert!(first.shares_topology_with(&second));
    assert_eq!(engine.scope_cascade_templates.len(), 1);
    assert_eq!(first.entries()[0].cascade_order, second.entries()[0].cascade_order);
    assert_eq!(first.entries()[0].rule, rules[0]);
    assert_eq!(second.entries()[0].rule, rules[1]);

    engine.invalidate_scope_programs();
    let (_, rebuilt) = engine.ranked_scope_program(TreeScopeID(1));
    assert!(first.shares_topology_with(&rebuilt));

    let replacement_program = engine
        .programs
        .add(test_selector_program(".other", &[("other", StyleAtomID(201))]));
    engine.selector_programs_need_sweep = true;
    for rule in rules {
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(replacement_program);
        engine.replace_rule_version(rule, version);
    }
    discard_transaction(&mut engine);
    assert!(
        engine
            .scope_dispatch_templates
            .keys()
            .all(|shape| shape.0.iter().all(|&(program, _)| program != selector_program))
    );
}

#[test]
fn a_scope_dispatch_can_extend_a_finished_prefix_template() {
    let mut programs = SelectorPrograms::new();
    let base = programs.add(test_selector_program(
        ".base .target",
        &[("base", StyleAtomID(200)), ("target", StyleAtomID(201))],
    ));
    let suffix = programs.add(test_selector_program(
        ".extra .target",
        &[("extra", StyleAtomID(202)), ("target", StyleAtomID(201))],
    ));
    let rules = [RuleID(1), RuleID(2)];

    let mut template = RuleDispatch::new();
    insert_scope_rule(&mut template, &programs, rules[0], base, true);
    template.finish_prefixes();
    let mut extended = RuleDispatch::rebind_rules_for_extension(&template, &rules[..1]);
    insert_scope_rule(&mut extended, &programs, rules[1], suffix, true);
    extended.finish_prefixes();

    let mut cold = RuleDispatch::new();
    insert_scope_rule(&mut cold, &programs, rules[0], base, true);
    insert_scope_rule(&mut cold, &programs, rules[1], suffix, true);
    cold.finish_prefixes();

    assert_eq!(extended.entries(), cold.entries());
    assert_eq!(extended.ancestor_dispatch_shape(), cold.ancestor_dispatch_shape());
    assert!(extended.prefixes().contains_entry(base, 0));
    assert!(extended.prefixes().contains_entry(suffix, 0));
}

#[test]
fn document_author_sheets_keep_independent_shadow_scope_programs() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let independent_scope = TreeScopeID(1);
    let document_style_scope = TreeScopeID(2);
    engine.set_tree_scope_uses_document_sheets(document_style_scope);
    let shadow_sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(shadow_sheet, independent_scope);
    engine.attach_sheet(shadow_sheet, document_style_scope);
    discard_transaction(&mut engine);

    let (independent_program, _) = engine.ranked_scope_program(independent_scope);
    engine.ranked_scope_program(document_style_scope);
    engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    let document_sheet = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    engine.attach_sheet(document_sheet, TreeScopeID::DOCUMENT);

    assert_eq!(
        engine.scope_program_by_scope[independent_scope.0 as usize].unwrap().1,
        independent_program
    );
    assert!(engine.scope_program_by_scope[document_style_scope.0 as usize].is_none());
    assert!(engine.scope_program_by_scope[TreeScopeID::DOCUMENT.0 as usize].is_none());

    let user_agent_sheet = engine.add_sheet(StyleSheetObjectID(3), CascadeOrigin::UserAgent);
    engine.attach_sheet(user_agent_sheet, TreeScopeID::DOCUMENT);

    assert!(engine.scope_program_by_scope[independent_scope.0 as usize].is_none());
}

#[test]
fn changing_one_scope_keeps_an_equivalent_scopes_ranked_program() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let selector_program = engine
        .programs
        .add(test_selector_program(".target", &[("target", StyleAtomID(200))]));
    let shared_sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(shared_sheet, TreeScopeID(1));
    engine.attach_sheet(shared_sheet, TreeScopeID(2));
    let rule = engine.append_rule(shared_sheet, None, RuleKind::Style);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(selector_program);
    engine.replace_rule_version(rule, version);
    let additional_sheet = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    discard_transaction(&mut engine);

    let (first_program, _) = engine.ranked_scope_program(TreeScopeID(1));
    let (second_program, _) = engine.ranked_scope_program(TreeScopeID(2));
    assert_eq!(first_program, second_program);

    engine.attach_sheet(additional_sheet, TreeScopeID(2));
    assert_eq!(
        engine.scope_program_by_scope[TreeScopeID(1).0 as usize].unwrap().1,
        first_program
    );
    assert!(engine.scope_program_by_scope[TreeScopeID(2).0 as usize].is_none());
    assert_eq!(engine.scope_program(first_program).scope_count, 1);
}

#[test]
fn removing_a_middle_child_splices_the_child_sequence() {
    let (mut engine, nodes) = linear_document();
    engine.record_tree_delta(
        nodes[2],
        Some(relations(
            Some(nodes[0].raw()),
            Some(nodes[1].raw()),
            Some(nodes[3].raw()),
        )),
        None,
    );
    discard_transaction(&mut engine);

    assert_eq!(
        engine.tree().children(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[1], nodes[3]]
    );
    assert!(!engine.tree().is_live(nodes[2]));
    assert_eq!(engine.tree().connected_element_count(), 3);
}

#[test]
fn removing_the_first_child_moves_the_parents_head() {
    let (mut engine, nodes) = linear_document();
    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()))),
        None,
    );
    discard_transaction(&mut engine);
    assert_eq!(
        engine.tree().children(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[2], nodes[3]]
    );
}

#[test]
fn moving_a_node_unlinks_it_before_relinking() {
    let (mut engine, nodes) = linear_document();
    // Move the last child to the front of the same parent.
    engine.record_tree_delta(
        nodes[3],
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
        Some(relations(Some(nodes[0].raw()), None, Some(nodes[1].raw()))),
    );
    discard_transaction(&mut engine);
    assert_eq!(
        engine.tree().children(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[3], nodes[1], nodes[2]]
    );
}

#[test]
fn a_single_scope_document_allocates_no_tree_scope_column() {
    let (engine, _) = linear_document();
    assert!(!engine.tree().has_tree_scopes());
}

#[test]
fn the_document_budget_follows_the_live_element_count() {
    let (mut engine, nodes) = linear_document();
    let with_four = engine.memory().tier3_limit();

    engine.record_tree_delta(
        nodes[3],
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
        None,
    );
    discard_transaction(&mut engine);
    assert!(engine.memory().tier3_limit() < with_four);
}

#[test]
fn rule_activation_reaches_only_current_selector_matches() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    for node in [nodes[0], nodes[3]] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    discard_transaction(&mut engine);

    for conditions_hold in [false, true] {
        engine.set_rule_conditions_hold(rule, conditions_hold);
        assert_ne!(engine.program.rule_conditions_hold(rule), conditions_hold);
        let mut planned = Vec::new();
        assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
        assert_eq!(engine.program.rule_conditions_hold(rule), conditions_hold);
        assert_eq!(planned, vec![nodes[3].raw()]);
        assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
    }
}

#[test]
fn rule_deactivation_reaches_only_nodes_where_the_rule_won() {
    let (mut engine, nodes) = linear_document();
    let target = StyleAtomID(200);
    let overriding = StyleAtomID(201);
    let toggled = add_target_rule(&mut engine, StyleSheetObjectID(1), target);
    engine.set_rule_declared_properties(toggled, &[(1, false)], true);
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), overriding);
    engine.set_rule_declared_properties(winner, &[(1, true)], true);
    for node in [nodes[1], nodes[2]] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    add_feature(&mut engine, nodes[1], FeatureKey::Class(overriding));
    discard_transaction(&mut engine);

    for &node in &nodes {
        let exact = engine.match_element(node).unwrap();
        let compact = engine.matches_for_cascade(exact.clone(), false, Some(node));
        engine.remember_retained_match_answer(node, &exact);
        engine.remember_cascade_input(node, &compact);
        publish_current_cascade_as_computed(&mut engine, node);
    }

    engine.set_rule_conditions_hold(toggled, false);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(
        planned,
        vec![nodes[2].raw()],
        "removing a matching loser cannot change the retained winner"
    );
}

#[test]
fn local_routes_for_one_exact_entry_are_compared_once() {
    let (mut engine, nodes) = nested_document();
    let first = StyleAtomID(200);
    let second = StyleAtomID(201);
    let target = StyleAtomID(202);
    let program = engine.programs.add(test_selector_program(
        ".first.second .target",
        &[("first", first), ("second", second), ("target", target)],
    ));
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::User);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    version.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, version);
    engine.set_rule_declared_properties(rule, &[(1, false)], true);
    add_feature(&mut engine, nodes[3], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    for &node in &nodes {
        let exact = engine.match_element(node).unwrap();
        let compact = engine.matches_for_cascade(exact.clone(), false, Some(node));
        engine.remember_retained_match_answer(node, &exact);
        engine.remember_cascade_input(node, &compact);
        publish_current_cascade_as_computed(&mut engine, node);
    }

    let grouped_before = engine.counters().get(Counter::GroupedExactSelectorRoutes);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(first));
    add_feature(&mut engine, nodes[1], FeatureKey::Class(second));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert_eq!(
        engine.counters().get(Counter::GroupedExactSelectorRoutes) - grouped_before,
        1
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn rule_activation_exactly_matches_a_refused_prefix_chain() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let mut raw = vec![0_u32; 36];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    for index in 1..=34 {
        engine.record_tree_delta(
            nodes[index],
            None,
            Some(relations(
                Some(nodes[0].raw()),
                (index > 1).then(|| nodes[index - 1].raw()),
                (index < 34).then(|| nodes[index + 1].raw()),
            )),
        );
    }
    engine.record_tree_delta(nodes[35], None, Some(relations(Some(nodes[34].raw()), None, None)));
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }
    let target = StyleAtomID(300);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let mut refused_rule = None;
    let mut refused_program = None;
    for index in 0..34 {
        let guard = StyleAtomID(200 + index);
        let selector_text = format!(".guard:nth-child({}) .target", index + 1);
        let program = engine.programs.add(test_selector_program(
            &selector_text,
            &[("guard", guard), ("target", target)],
        ));
        let rule = engine.append_rule(sheet, None, RuleKind::Style);
        engine.add_routing_rule(rule, program);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(program);
        version.declaration_block = Some(DeclarationBlockID(index + 1));
        engine.replace_rule_version(rule, version);
        engine.set_rule_declared_properties(rule, &[(index as u16 + 1, false)], true);
        add_feature(&mut engine, nodes[index as usize + 1], FeatureKey::Class(guard));
        refused_rule = Some(rule);
        refused_program = Some(program);
    }
    add_feature(&mut engine, nodes[35], FeatureKey::Class(target));
    let refused_rule = refused_rule.unwrap();
    let refused_program = refused_program.unwrap();
    engine.set_rule_conditions_hold(refused_rule, false);
    discard_transaction(&mut engine);

    let (_, dispatch) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    assert!(!dispatch.prefixes().is_empty());
    assert!(!dispatch.prefixes().contains_entry(refused_program, 0));
    assert!(engine.begin_cold_matching_batch(nodes[0]));
    for &node in &nodes {
        engine.match_element(node).unwrap();
    }
    let (scope_program, _) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    let caches = engine.prefix_caches.borrow();
    let states = match caches.states.lookup(scope_program) {
        Lookup::Known(states) => states,
        Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected retained prefix states"),
    };
    assert!(states.retained_matches_for(nodes[35]).is_some());
    drop(caches);
    let incidences = engine.materialize_current_selector_incidence(refused_program).unwrap();
    assert!(incidences.iter().any(|incidence| incidence.node == nodes[35]));
    engine.end_cold_matching_batch();

    engine.set_rule_conditions_hold(refused_rule, true);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[35].raw()]);
}

#[test]
fn rule_activation_uses_the_fact_side_where_the_rule_contributes() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    for (node, class) in [(nodes[1], guard), (nodes[3], target)] {
        add_feature(&mut engine, node, FeatureKey::Class(class));
    }
    discard_transaction(&mut engine);

    engine.set_rule_conditions_hold(rule, false);
    remove_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(
        planned,
        vec![nodes[3].raw()],
        "turning the rule off tests the old selector facts"
    );

    engine.set_rule_conditions_hold(rule, true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    planned.clear();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(
        planned,
        vec![nodes[3].raw()],
        "turning the rule on tests the new selector facts"
    );
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn a_sheet_transition_reaches_only_selector_matches() {
    let (mut engine, nodes) = nested_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_target_rule(&mut engine, guard, target);
    let sheet = engine.program.rule_sheet(rule);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(guard));
    for node in [nodes[0], nodes[3]] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    discard_transaction(&mut engine);

    engine.detach_sheet(sheet, TreeScopeID::DOCUMENT);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()], "detaching reads the old selector facts");
    assert_eq!(
        engine.counters().get(Counter::SheetChangeCandidatesRejected),
        1,
        "the root carries the dispatch class but does not match the ancestor requirement"
    );

    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    planned.clear();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()], "attaching reads the new selector facts");
    assert_eq!(engine.counters().get(Counter::SheetChangeCandidatesRejected), 2);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn any_matching_selector_list_entry_keeps_an_activation_candidate() {
    let (mut engine, nodes) = nested_document();
    let matching_guard = StyleAtomID(200);
    let absent_guard = StyleAtomID(201);
    let target = StyleAtomID(202);
    let rule = add_guard_target_selector_list_rule(&mut engine, matching_guard, absent_guard, target);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(matching_guard));
    for node in [nodes[0], nodes[3]] {
        add_feature(&mut engine, node, FeatureKey::Class(target));
    }
    discard_transaction(&mut engine);

    engine.set_rule_conditions_hold(rule, false);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[3].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

#[test]
fn incomplete_selector_facts_keep_an_activation_candidate() {
    let (mut engine, nodes) = linear_document();
    let guard = StyleAtomID(200);
    let target = StyleAtomID(201);
    let rule = add_guard_sibling_target_rule(&mut engine, guard, target);
    add_feature(&mut engine, nodes[2], FeatureKey::Class(target));
    discard_transaction(&mut engine);

    engine.set_rule_conditions_hold(rule, false);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction_nodes(nodes[0], |nodes| planned.extend_from_slice(nodes)));
    assert_eq!(planned, vec![nodes[2].raw()]);
    assert_eq!(engine.memory().bytes_in_category(MemoryCategory::BatchScratch), 0);
}

// -- Stylesheet program deltas -----------------------------------------------------------

fn kinds_of(transaction: &StyleTransaction) -> Vec<InputKind> {
    transaction.inputs.iter().map(|input| input.key.kind()).collect()
}

fn authoring_engine() -> (StyleEngine, SheetID, RuleID) {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    let mut contents = engine.program().rule_version(rule);
    contents.selector_program = Some(SelectorProgramID(1));
    contents.declaration_block = Some(DeclarationBlockID(1));
    engine.replace_rule_version(rule, contents);
    discard_transaction(&mut engine);
    (engine, sheet, rule)
}

#[test]
fn repeated_selector_replacement_reuses_program_and_route_storage() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    let mut retained_bytes = None;

    for index in 0..128_u32 {
        let (program, inserted) = engine
            .programs
            .add_with_status(test_selector_program(".target", &[("target", StyleAtomID(index + 1))]));
        engine.selector_programs_need_sweep |= inserted;
        engine.programs.settle_memory(&mut engine.memory);
        engine.add_routing_rule(rule, program);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(program);
        engine.replace_rule_version(rule, version);

        discard_transaction(&mut engine);
        assert!(engine.programs.len() <= 2);
        assert_eq!(engine.routing.len(), 1);
        if index == 2 {
            retained_bytes = Some(engine.programs.capacity_bytes());
        } else if index > 2 {
            assert_eq!(engine.programs.capacity_bytes(), retained_bytes.unwrap());
        }
    }
}

#[test]
fn adding_a_live_selector_program_keeps_existing_routing() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);

    for index in 0..2_u32 {
        let rule = engine.append_rule(sheet, None, RuleKind::Style);
        let (program, inserted) = engine
            .programs
            .add_with_status(test_selector_program(".target", &[("target", StyleAtomID(index + 1))]));
        engine.selector_programs_need_sweep |= inserted;
        engine.programs.settle_memory(&mut engine.memory);
        engine.add_routing_rule(rule, program);
        let mut version = engine.program.rule_version(rule);
        version.selector_program = Some(program);
        engine.replace_rule_version(rule, version);

        let routing_before_sweep = Rc::clone(&engine.routing);
        discard_transaction(&mut engine);
        assert!(Rc::ptr_eq(&routing_before_sweep, &engine.routing));
    }
    assert_eq!(engine.routing.len(), 2);
}

#[test]
fn a_declaration_edit_journals_only_the_declaration_field() {
    let (mut engine, _, rule) = authoring_engine();
    let mut contents = engine.program().rule_version(rule);
    contents.declaration_block = Some(DeclarationBlockID(2));
    engine.replace_rule_version(rule, contents);

    let transaction = engine.take_transaction();
    assert_eq!(
        transaction.inputs.iter().map(|input| input.key).collect::<Vec<_>>(),
        vec![InputKey::RuleField(rule, RuleField::Declarations)],
        "an edit that cannot change selector truth must not name the selector"
    );
    engine.release_transaction(transaction);
}

#[test]
fn a_selector_edit_journals_the_selector_field() {
    let (mut engine, _, rule) = authoring_engine();
    let mut contents = engine.program().rule_version(rule);
    contents.selector_program = Some(SelectorProgramID(2));
    engine.replace_rule_version(rule, contents);
    assert_eq!(
        engine.program().rule_version(rule).selector_program,
        Some(SelectorProgramID(1))
    );
    assert_eq!(
        engine.current_rule_version(rule).selector_program,
        Some(SelectorProgramID(2))
    );

    let transaction = engine.take_transaction();
    assert_eq!(
        engine.program().rule_version(rule).selector_program,
        Some(SelectorProgramID(2))
    );
    assert_eq!(
        transaction.inputs.iter().map(|input| input.key).collect::<Vec<_>>(),
        vec![InputKey::RuleField(rule, RuleField::Selector)]
    );
    engine.release_transaction(transaction);
}

#[test]
fn rule_metadata_commits_at_the_transaction_barrier() {
    let (mut engine, _, rule) = authoring_engine();
    engine.set_rule_gated_by_container_query(rule);
    engine.set_rule_in_a_layer(rule);

    assert!(!engine.program.rule_is_gated_by_container_query(rule));
    assert!(engine.current_rule_is_gated_by_container_query(rule));
    assert!(
        engine
            .program
            .rules_in_a_layer_in_scope(TreeScopeID::DOCUMENT)
            .is_empty()
    );
    assert!(engine.has_pending_transaction());

    let transaction = engine.take_transaction();
    assert!(engine.program.rule_is_gated_by_container_query(rule));
    assert_eq!(engine.program.rules_in_a_layer_in_scope(TreeScopeID::DOCUMENT), [rule]);
    engine.release_transaction(transaction);
}

#[test]
fn container_query_gating_invalidates_committed_scope_dispatch() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let program = engine
        .programs
        .add(test_selector_program(".target", &[("target", StyleAtomID(200))]));
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.add_routing_rule(rule, program);
    let mut version = engine.program.rule_version(rule);
    version.selector_program = Some(program);
    engine.replace_rule_version(rule, version);
    discard_transaction(&mut engine);

    // The held program is only ever set beside its scope's retained entry, and the sheet-scoped
    // sweep reaches it through that entry.
    let (scope_program, _) = engine.ranked_scope_program(TreeScopeID::DOCUMENT);
    assert_eq!(engine.held_scope_program.map(|(_, _, id)| id), Some(scope_program));

    engine.set_rule_gated_by_container_query(rule);

    assert!(engine.held_scope_program.is_none());
    assert!(
        engine
            .scope_program_by_scope
            .get(TreeScopeID::DOCUMENT.0 as usize)
            .copied()
            .flatten()
            .is_none()
    );
}

#[test]
fn program_inputs_lower_to_one_join_delta_shape() {
    let (mut engine, sheet, rule) = authoring_engine();
    let first_program = SelectorProgramID(1);
    let second_program = SelectorProgramID(2);

    let mut contents = engine.program().rule_version(rule);
    contents.selector_program = Some(second_program);
    engine.replace_rule_version(rule, contents);
    let transaction = engine.take_transaction();
    let input = transaction.inputs[0].key;
    assert_eq!(
        transaction.program_joins_for(input),
        &[ProgramJoinDelta {
            input,
            rule,
            before_program: Some(first_program),
            after_program: Some(second_program),
            before_contributes: true,
            after_contributes: true,
            kind: ProgramJoinDeltaKind::ActiveRuleMatch,
        }]
    );
    let selection = engine.rules_for_retained_answer_patch(&transaction).unwrap();
    assert_eq!(
        selection
            .affected
            .iter()
            .map(|affected| (affected.rule, affected.program, affected.evaluate))
            .collect::<Vec<_>>(),
        vec![(rule, first_program, false), (rule, second_program, true)]
    );
    engine.release_transaction(transaction);

    let mut contents = engine.program().rule_version(rule);
    contents.declaration_block = Some(DeclarationBlockID(2));
    engine.replace_rule_version(rule, contents);
    let transaction = engine.take_transaction();
    assert_eq!(transaction.program_joins[0].kind, ProgramJoinDeltaKind::Declarations);
    assert!(
        engine
            .rules_for_retained_answer_patch(&transaction)
            .unwrap()
            .always_emit
    );
    engine.release_transaction(transaction);

    engine.set_rule_layer(rule, CascadeLayerID(1));
    let transaction = engine.take_transaction();
    assert_eq!(transaction.program_joins[0].kind, ProgramJoinDeltaKind::Priority);
    assert!(
        engine
            .rules_for_retained_answer_patch(&transaction)
            .unwrap()
            .orders_shifted
    );
    engine.release_transaction(transaction);

    engine.set_sheet_enabled(sheet, false);
    let transaction = engine.take_transaction();
    let input = transaction.inputs[0].key;
    assert_eq!(
        transaction.program_joins_for(input),
        &[ProgramJoinDelta {
            input,
            rule,
            before_program: Some(second_program),
            after_program: Some(second_program),
            before_contributes: true,
            after_contributes: false,
            kind: ProgramJoinDeltaKind::ActiveRuleMatch,
        }]
    );
    let selection = engine.rules_for_retained_answer_patch(&transaction).unwrap();
    assert_eq!(selection.affected.len(), 1);
    assert_eq!(selection.affected[0].program, second_program);
    assert!(selection.affected[0].evaluate);
    engine.release_transaction(transaction);
}

#[test]
fn leaving_a_slot_refuses_document_scope_retained_answer_patching() {
    for _ in 0..64 {
        let (mut engine, nodes) = linear_document();
        let unassigned = relations(Some(nodes[0].raw()), None, Some(nodes[2].raw()));
        let mut assigned = unassigned;
        assigned.assigned_slot = Some(nodes[3]);
        engine.record_tree_delta(nodes[1], Some(unassigned), Some(assigned));
        discard_transaction(&mut engine);

        engine.record_tree_delta(nodes[1], Some(assigned), Some(unassigned));
        let transaction = engine.take_transaction();
        assert!(
            engine.rules_for_retained_answer_patch(&transaction).is_none(),
            "a default-slot to missing-slot move needs cross-scope exact matching"
        );
        engine.release_transaction(transaction);
    }
}

#[test]
fn rewriting_a_rule_to_its_current_contents_journals_nothing() {
    let (mut engine, _, rule) = authoring_engine();
    let contents = engine.program().rule_version(rule);
    engine.replace_rule_version(rule, contents);

    let transaction = engine.take_transaction();
    assert!(transaction.is_empty());
    engine.release_transaction(transaction);
}

#[test]
fn reordering_a_sheet_is_a_cascade_change_not_a_selector_change() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let first = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    let second = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    engine.attach_sheet(first, TreeScopeID::DOCUMENT);
    engine.attach_sheet(second, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(first, None, RuleKind::Style);
    discard_transaction(&mut engine);

    engine.detach_sheet(first, TreeScopeID::DOCUMENT);
    engine.attach_sheet_before_sheet(first, Some(second), TreeScopeID::DOCUMENT);
    let transaction = engine.take_transaction();
    assert_eq!(
        transaction.inputs.iter().map(|input| input.key).collect::<Vec<_>>(),
        vec![InputKey::CascadeTopology(TopologyAxis::SheetOrder(
            TreeScopeID::DOCUMENT
        ))]
    );
    assert_eq!(
        engine.program().rule_version(rule).selector_program,
        None,
        "the sheet's rules were not rewritten"
    );
    engine.release_transaction(transaction);
}

#[test]
fn sheet_attachment_commits_at_the_transaction_barrier() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let first = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    let second = engine.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
    engine.attach_sheet(first, TreeScopeID::DOCUMENT);
    discard_transaction(&mut engine);

    engine.attach_sheet_before_sheet(second, Some(first), TreeScopeID::DOCUMENT);

    assert_eq!(engine.program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![first]);
    assert_eq!(
        engine.current_sheets_in_scope(TreeScopeID::DOCUMENT),
        vec![second, first]
    );
    let transaction = engine.take_transaction();
    assert_eq!(
        engine.program.sheets_in_scope(TreeScopeID::DOCUMENT),
        vec![second, first]
    );
    engine.release_transaction(transaction);
}

#[test]
fn rule_changes_share_one_sheet_attachment_decision_per_transaction() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);

    engine.append_rule(sheet, None, RuleKind::Style);
    engine.append_rule(sheet, None, RuleKind::Style);
    assert_eq!(engine.rule_change_is_carried_by_sheet.len(), 1);

    discard_transaction(&mut engine);
    assert!(engine.rule_change_is_carried_by_sheet.is_empty());

    engine.append_rule(sheet, None, RuleKind::Style);
    assert_eq!(engine.rule_change_is_carried_by_sheet.len(), 1);
    let transaction = engine.take_transaction();
    assert!(
        transaction
            .inputs
            .iter()
            .any(|input| matches!(input.key, InputKey::RuleField(_, RuleField::Existence)))
    );
    engine.release_transaction(transaction);
}

#[test]
fn deleting_a_group_rule_journals_every_identity_it_took_with_it() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let media = engine.append_rule(sheet, None, RuleKind::Media);
    let inner = engine.append_rule(sheet, Some(media), RuleKind::Style);
    discard_transaction(&mut engine);

    let removed = engine.remove_rule(media);
    assert_eq!(removed, vec![media, inner]);

    let transaction = engine.take_transaction();
    let keys: Vec<InputKey> = transaction.inputs.iter().map(|input| input.key).collect();
    assert!(keys.contains(&InputKey::RuleField(media, RuleField::Existence)));
    assert!(keys.contains(&InputKey::RuleField(inner, RuleField::Existence)));
    engine.release_transaction(transaction);
}

#[test]
fn rule_existence_commits_at_the_transaction_barrier() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    discard_transaction(&mut engine);

    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    assert!(!engine.program.rule_is_live(rule));
    assert!(engine.current_rule_is_live(rule));
    let transaction = engine.take_transaction();
    assert!(engine.program.rule_is_live(rule));
    engine.release_transaction(transaction);

    engine.remove_rule(rule);
    assert!(engine.program.rule_is_live(rule));
    assert!(!engine.current_rule_is_live(rule));
    let transaction = engine.take_transaction();
    assert!(!engine.program.rule_is_live(rule));
    engine.release_transaction(transaction);
}

#[test]
fn inserting_and_deleting_an_unobserved_rule_cancels_out() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    discard_transaction(&mut engine);

    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    engine.remove_rule(rule);

    let transaction = engine.take_transaction();
    assert!(
        transaction.is_empty(),
        "a rule inserted and removed before observation produces no selector or cascade work"
    );
    engine.release_transaction(transaction);
}

#[test]
fn publishing_an_implicit_layer_order_does_not_edit_the_program() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let version = engine.program.version();

    engine.set_layer_order(TreeScopeID::DOCUMENT, &[CascadeLayerID::UNLAYERED]);

    assert_eq!(engine.program.version(), version);
    assert!(engine.pending_program_base_version.is_none());
    let transaction = engine.take_transaction();
    assert!(transaction.is_empty());
    engine.release_transaction(transaction);
}

#[test]
fn allocating_an_unattached_sheet_does_not_edit_the_program() {
    let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
    let version = engine.program.version();

    engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);

    assert_eq!(engine.program.version(), version);
    assert!(!engine.has_pending_transaction());
    let transaction = engine.take_transaction();
    assert!(transaction.is_empty());
    engine.release_transaction(transaction);
}

#[test]
fn a_layer_topology_change_is_its_own_input_kind() {
    let (mut engine, _, _) = authoring_engine();
    let base = CascadeLayerID(1);
    let theme = CascadeLayerID(2);
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[base, theme]);
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[theme, base]);
    engine.set_tree_scope_uses_document_sheets(TreeScopeID(1));

    assert_eq!(engine.program.layer_index(TreeScopeID::DOCUMENT, base), 0);
    assert_eq!(engine.layer_index(TreeScopeID::DOCUMENT, base), 1);
    assert!(!engine.program.scope_uses_document_sheets(TreeScopeID(1)));

    let transaction = engine.take_transaction();
    assert_eq!(engine.program.layer_index(TreeScopeID::DOCUMENT, base), 1);
    assert!(engine.program.scope_uses_document_sheets(TreeScopeID(1)));
    assert_eq!(kinds_of(&transaction), vec![InputKind::CascadeTopology]);
    assert_eq!(engine.counters().get(Counter::CascadeTopologyDeltas), 1);
    engine.release_transaction(transaction);
}

#[test]
fn disabling_a_sheet_is_an_activation_change() {
    let (mut engine, sheet, _) = authoring_engine();
    engine.set_sheet_enabled(sheet, false);
    assert!(engine.program.sheet_is_enabled(sheet));

    let transaction = engine.take_transaction();
    assert!(!engine.program.sheet_is_enabled(sheet));
    assert_eq!(
        transaction.inputs.iter().map(|input| input.key).collect::<Vec<_>>(),
        vec![InputKey::SheetActivation(sheet)]
    );
    engine.release_transaction(transaction);
}

#[test]
fn an_added_rule_that_loses_everywhere_confirms_without_cold_matching() {
    let (mut engine, nodes) = linear_document();
    let guard_class = StyleAtomID(200);
    let target_class = StyleAtomID(201);
    let important = add_target_rule(&mut engine, StyleSheetObjectID(1), target_class);
    engine.set_rule_declared_properties(important, &[(1, true)], true);
    // A second matched rule with an incomplete declaration list keeps the winner inventory
    // incomplete, so the whole-inventory proof cannot carry the stop; the transition proof must.
    let incomplete = add_target_rule(&mut engine, StyleSheetObjectID(2), target_class);
    engine.set_rule_declared_properties(incomplete, &[(2, false)], false);
    // The addition arrives through an ancestor class toggle, so the confirmed node itself
    // carries no direct transaction input and stays eligible for confirmation.
    let loser = add_guard_target_rule_in_sheet(&mut engine, StyleSheetObjectID(3), guard_class, target_class);
    engine.set_rule_declared_properties(loser, &[(1, false)], true);
    for &node in &nodes {
        set_atom_feature(&mut engine, node, FeatureKey::TagName, StyleAtomID(100));
    }
    add_feature(&mut engine, nodes[1], FeatureKey::Class(target_class));
    discard_transaction(&mut engine);

    let old_answer = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert!(old_answer.iter().any(|entry| entry.rule == important));
    publish_current_cascade_as_computed(&mut engine, nodes[1]);

    add_feature(&mut engine, nodes[0], FeatureKey::Class(guard_class));
    let stops_before = engine.counters().get(Counter::PublishedExactCascadeStops);
    let proofs_before = engine.counters().get(Counter::TransitionProofConfirmed);
    let mut planned = Vec::new();
    assert!(engine.take_style_transaction(nodes[0], |_, _, reactions| {
        planned.extend(reactions.iter().map(|reaction| reaction.style_node));
    }));

    assert!(
        !planned.contains(&nodes[1].raw()),
        "an added rule that loses everywhere must not publish the losing node"
    );
    assert_eq!(
        engine.counters().get(Counter::PublishedExactCascadeStops),
        stops_before + 1
    );
    assert_eq!(
        engine.counters().get(Counter::TransitionProofConfirmed),
        proofs_before + 1
    );
    let current = engine.match_element_for_cascade(nodes[1]).unwrap();
    assert!(current.iter().any(|entry| entry.rule == loser));
}

#[test]
fn answer_transitions_refuse_equality_removals_and_winning_additions() {
    let (mut engine, nodes) = linear_document();
    let anchor_class = StyleAtomID(200);
    let toggled_class = StyleAtomID(201);
    let base = add_target_rule(&mut engine, StyleSheetObjectID(1), anchor_class);
    engine.set_rule_declared_properties(base, &[(1, false)], true);
    let winner = add_target_rule(&mut engine, StyleSheetObjectID(2), toggled_class);
    engine.set_rule_declared_properties(winner, &[(1, false)], true);
    add_feature(&mut engine, nodes[1], FeatureKey::Class(anchor_class));
    discard_transaction(&mut engine);

    let exact = engine.match_element(nodes[1]).unwrap();
    let before = engine.matches_for_cascade(exact.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact);
    engine.remember_cascade_input(nodes[1], &before);
    let Lookup::Known(&before_input) = engine.retained_match_answers.cascade_input_lookup(nodes[1]) else {
        panic!("the anchored answer must intern a cascade input");
    };
    publish_current_cascade_as_computed(&mut engine, nodes[1]);

    // Equality is never a proof: a stale retained answer compares equal to itself.
    assert!(!engine.answer_transition_cannot_change_cascade(nodes[1], before_input, before_input));

    add_feature(&mut engine, nodes[1], FeatureKey::Class(toggled_class));
    discard_transaction(&mut engine);
    let exact = engine.match_element(nodes[1]).unwrap();
    let with = engine.matches_for_cascade(exact.clone(), false, Some(nodes[1]));
    engine.remember_retained_match_answer(nodes[1], &exact);
    engine.remember_cascade_input(nodes[1], &with);
    let Lookup::Known(&with_winner) = engine.retained_match_answers.cascade_input_lookup(nodes[1]) else {
        panic!("the toggled answer must intern a cascade input");
    };
    assert_ne!(before_input, with_winner);

    // A later-sheet addition to the same property is not strictly weaker; the proof must refuse.
    assert!(!engine.answer_transition_cannot_change_cascade(nodes[1], before_input, with_winner));

    // A removal can uncover a candidate provenance cannot always name; it must refuse too.
    assert!(!engine.answer_transition_cannot_change_cascade(nodes[1], with_winner, before_input));
}
