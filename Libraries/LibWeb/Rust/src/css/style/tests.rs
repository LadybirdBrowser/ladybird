/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::instrumentation::Counter;
use super::layout_style::DerivedStyleRecord;
use super::pipeline::rule_store::{DeclarationBlockID, SheetListStart};
use super::*;
use crate::css::animated_overlay::AnimatedOverlay;
use crate::css::computed_value_types::{ComputedSize, SizingValues};
use crate::css::computed_values::{DefaultGroups, StyleGroups};
use crate::css::table_group_builder::group_index;
use crate::layout::node_data::{LayoutStyleRef, NodeKind, NodeSlotId};
use crate::layout::{LayoutNodeArena, StyleRelayout};

fn native_rules(source: &str) -> std::rc::Rc<crate::css::rule::NativeRuleList> {
    use crate::css::css_tokenizer::TokenizerInput;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    let units: Vec<_> = source.encode_utf16().collect();
    let context = crate::css::parser::value_parser::ParseContext::default();
    crate::css::rule::NativeRuleList::from_parsed(parse_shared_stylesheet(
        TokenizerInput::Utf16(&units),
        Some(&context),
    ))
}

impl StyleEngine {}

#[test]
fn native_declaration_publication_reuses_values_and_observes_live_mutation() {
    use super::bridge::{BoundScopeChain, publish_rule_declarations, publish_style_rule};
    use crate::css::declaration_block::{
        DeclarationBlock, rust_declaration_block_replace, rust_declaration_block_retain,
    };
    let (mut engine, _) = linear_document();
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let first = native_rules("* { color: #14181c; }");
    let bound = first.rule_at(0).style_rule().selectors();
    let rule = publish_style_rule(
        &mut engine,
        sheet.0 + 1,
        0,
        &bound.selectors,
        &BoundScopeChain::default(),
    );
    let mut first = DeclarationBlock::new(first.rule_at(0).cascade_declarations().unwrap());
    let retained = unsafe { Box::from_raw(rust_declaration_block_retain(&first)) };
    let second = native_rules("* { color: rgb(20, 24, 28); }")
        .rule_at(0)
        .cascade_declarations()
        .unwrap();
    let transitions = DeclarationBlock::new(
        native_rules("* { transition-duration: 1s; }")
            .rule_at(0)
            .cascade_declarations()
            .unwrap(),
    );
    let callbacks = crate::css::ffi_stats::CPP_CALLBACK_COUNT.get();
    assert!(!publish_rule_declarations(&mut engine, rule, &first.data()));
    assert!(!publish_rule_declarations(&mut engine, rule, &second));
    rust_declaration_block_replace(&mut first, &transitions);
    assert!(publish_rule_declarations(&mut engine, rule, &retained.data()));
    assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), callbacks);
}

fn add_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: LocalFeatureKey) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Absent),
        InputValue::Feature(FeatureValue::Present),
    );
}

fn set_atom_feature(engine: &mut StyleEngine, node: StyleNodeID, feature: LocalFeatureKey, atom: StyleAtomID) {
    engine.record_input(
        InputKey::LocalFeature(node, feature),
        InputValue::Feature(FeatureValue::Absent),
        InputValue::Feature(FeatureValue::Atom(atom)),
    );
}

pub(super) fn discard_transaction(engine: &mut StyleEngine) {
    let transaction = engine.take_transaction();
    engine.release_transaction(transaction);
}

#[test]
fn retained_stores_reject_every_logically_mapped_property() {
    let (_, nodes) = linear_document();
    let target = computed::ComputedStyleTarget::new(nodes[1], u8::MAX);
    let mut mapped_property_count = 0;

    for property in crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
        ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
    {
        if crate::css::property_metadata::property_is_in_logical_group(property) {
            mapped_property_count += 1;
            assert!(!RetainedState::retained_store_supports_property(target, property));
        }
    }

    assert!(mapped_property_count > 0);
}

#[test]
fn part_names_are_derived_from_their_host_pairs() {
    let mut engine = StyleEngine::new();
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

    assert_eq!(
        engine.projection.written_facts(element).unwrap().parts(),
        &[inner_name, outer_name]
    );
    assert_eq!(engine.projection.part_hosts_of(element), &pairs);
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
fn a_sibling_only_link_write_does_not_recompute_the_sibling_subtree_depth() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[1]), None, None)));
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[2]), None, None)));
    discard_transaction(&mut engine);
    engine.tree.take_depth_recompute_visits();

    let mut inserted_raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut inserted_raw);
    let inserted = StyleNodeID::from_raw(inserted_raw[0]).unwrap();
    engine.record_tree_delta(inserted, None, Some(relations(Some(raw[0]), None, Some(raw[1]))));
    engine.settle_tree_at_barrier();

    assert_eq!(engine.tree.take_depth_recompute_visits(), 1);
    assert_eq!(engine.element_depth(nodes[3]), 3);
}

#[test]
fn wrapping_existing_children_recomputes_depth_from_the_arriving_parent() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, Some(raw[2]))));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[1]), None, None)));
    discard_transaction(&mut engine);

    let mut wrapper_raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut wrapper_raw);
    let wrapper = StyleNodeID::from_raw(wrapper_raw[0]).unwrap();
    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(raw[0]), None, Some(raw[2]))),
        Some(relations(Some(wrapper.raw()), None, Some(raw[2]))),
    );
    engine.record_tree_delta(
        nodes[2],
        Some(relations(Some(raw[0]), Some(raw[1]), None)),
        Some(relations(Some(wrapper.raw()), Some(raw[1]), None)),
    );
    engine.record_tree_delta(wrapper, None, Some(relations(Some(raw[0]), None, None)));
    engine.settle_tree_at_barrier();

    assert_eq!(engine.element_depth(wrapper), 1);
    assert_eq!(engine.element_depth(nodes[1]), 2);
    assert_eq!(engine.element_depth(nodes[2]), 2);
    assert_eq!(engine.element_depth(nodes[3]), 3);
}

#[test]
fn reparenting_between_equal_depth_parents_still_visits_the_subtree() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(raw[0]), None, Some(raw[2]))));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(raw[0]), Some(raw[1]), None)));
    engine.record_tree_delta(nodes[3], None, Some(relations(Some(raw[1]), None, None)));
    engine.record_tree_delta(nodes[4], None, Some(relations(Some(raw[3]), None, None)));
    discard_transaction(&mut engine);
    engine.tree.take_depth_recompute_visits();

    engine.record_tree_delta(
        nodes[3],
        Some(relations(Some(raw[1]), None, None)),
        Some(relations(Some(raw[2]), None, None)),
    );
    engine.settle_tree_at_barrier();

    assert_eq!(engine.tree.take_depth_recompute_visits(), 2);
    assert_eq!(engine.element_depth(nodes[3]), 2);
    assert_eq!(engine.element_depth(nodes[4]), 3);
}

#[test]
fn reparenting_below_a_sibling_only_staged_parent_recomputes_depth() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 7];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let [root, a, b, m, p, c, n] = nodes.as_slice() else {
        unreachable!();
    };
    engine.record_tree_delta(*root, None, Some(relations(None, None, None)));
    engine.record_tree_delta(*a, None, Some(relations(Some(root.raw()), None, Some(b.raw()))));
    engine.record_tree_delta(*b, None, Some(relations(Some(root.raw()), Some(a.raw()), None)));
    engine.record_tree_delta(*m, None, Some(relations(Some(a.raw()), None, None)));
    engine.record_tree_delta(*p, None, Some(relations(Some(m.raw()), None, None)));
    engine.record_tree_delta(*c, None, Some(relations(Some(p.raw()), None, None)));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        *n,
        None,
        Some(relations(Some(root.raw()), Some(a.raw()), Some(b.raw()))),
    );
    engine.record_tree_delta(
        *p,
        Some(relations(Some(m.raw()), None, None)),
        Some(relations(Some(b.raw()), None, None)),
    );
    engine.settle_tree_at_barrier();

    assert_eq!(engine.element_depth(*p), 2);
    assert_eq!(engine.element_depth(*c), 3);
}

#[test]
fn moving_back_after_an_intermediate_tree_apply_restores_depth() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 5];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.into_iter().map(|raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let [root, x, q1, q2, p] = nodes.as_slice() else {
        unreachable!();
    };
    engine.record_tree_delta(*root, None, Some(relations(None, None, None)));
    engine.record_tree_delta(*x, None, Some(relations(Some(root.raw()), None, Some(q1.raw()))));
    engine.record_tree_delta(*q1, None, Some(relations(Some(root.raw()), Some(x.raw()), None)));
    engine.record_tree_delta(*q2, None, Some(relations(Some(q1.raw()), None, None)));
    engine.record_tree_delta(*p, None, Some(relations(Some(x.raw()), None, None)));
    discard_transaction(&mut engine);

    engine.record_tree_delta(
        *p,
        Some(relations(Some(x.raw()), None, None)),
        Some(relations(Some(q2.raw()), None, None)),
    );
    engine.settle_tree_at_barrier();
    assert_eq!(engine.element_depth(*p), 3);

    engine.record_tree_delta(
        *p,
        Some(relations(Some(q2.raw()), None, None)),
        Some(relations(Some(x.raw()), None, None)),
    );
    engine.settle_tree_at_barrier();

    assert_eq!(engine.element_depth(*p), 2);
}

#[test]
fn an_element_that_arrives_and_departs_in_one_transaction_retires_at_its_barrier() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let passing = StyleNodeID::from_raw(raw[0]).unwrap();
    let arrived = relations(Some(nodes[0].raw()), Some(nodes[3].raw()), None);
    engine.record_tree_delta(passing, None, Some(arrived));
    engine.record_tree_delta(passing, Some(arrived), None);
    assert!(engine.retained.projection.relations_were_written(passing));

    let transaction = engine.take_transaction();
    assert!(!engine.tree.is_live(passing));
    assert!(!engine.retained.projection.is_live(passing));
    assert_eq!(engine.projection.next_element_sibling(nodes[3]), None);
    assert_eq!(engine.retained.projection.next_element_sibling(nodes[3]), None);
    engine.release_transaction(transaction);
    assert!(!engine.retained.projection.has_written_links());
}

#[test]
fn a_parent_whose_children_arrive_first_has_not_departed() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let [parent, child] = raw.map(|raw| StyleNodeID::from_raw(raw).unwrap());
    engine.record_tree_delta(child, None, Some(relations(Some(parent.raw()), None, None)));
    assert!(!engine.retained.projection.relations_were_written(parent));
    discard_transaction(&mut engine);
    assert!(engine.tree.is_live(parent));
    assert!(engine.retained.projection.is_live(parent));

    engine.record_tree_delta(
        parent,
        None,
        Some(relations(Some(nodes[0].raw()), Some(nodes[3].raw()), None)),
    );
    discard_transaction(&mut engine);
    assert_eq!(engine.projection.first_element_child(parent), Some(child));
    assert_eq!(engine.element_depth(child), 2);
}

#[test]
fn an_element_the_host_takes_out_of_a_tree_retires_even_where_none_was_written() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);

    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let node = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(node, Some(relations(Some(nodes[0].raw()), None, None)), None);
    assert!(engine.retained.projection.relations_were_written(node));
    discard_transaction(&mut engine);
    assert!(!engine.tree.is_live(node));
}

#[test]
fn a_fork_counts_its_unsettled_link_rows_apart_from_the_host() {
    use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);
    let unsettled = std::sync::Arc::new(AtomicU32::new(0));
    engine.share_tree_depth(std::sync::Arc::new(AtomicU32::new(0)), unsettled.clone());

    // Moving the last child under the first writes its links, its previous sibling's and its new
    // parent's.
    engine.record_tree_delta(
        nodes[3],
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
        Some(relations(Some(nodes[1].raw()), None, None)),
    );
    assert_eq!(unsettled.load(Relaxed), 3);

    let mut fork = engine.clone();
    fork.detach_tree_depth_for_fork();
    discard_transaction(&mut fork);
    assert_eq!(fork.element_depth(nodes[3]), 2);
    assert_eq!(unsettled.load(Relaxed), 3, "the fork settles its own rows");

    discard_transaction(&mut engine);
    assert_eq!(engine.element_depth(nodes[3]), 2);
    assert_eq!(unsettled.load(Relaxed), 0);
}

/// Builds `root -> [a, b, c]` through the same delta path C++ drives.
pub(super) fn linear_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let mut engine = StyleEngine::new();
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

fn nested_document() -> (StyleEngine, Vec<StyleNodeID>) {
    let mut engine = StyleEngine::new();
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
        set_atom_feature(&mut engine, node, LocalFeatureKey::TagName, StyleAtomID(100));
    }
    (engine, nodes)
}

#[test]
fn structural_deltas_maintain_the_relation_columns() {
    let (mut engine, nodes) = linear_document();
    discard_transaction(&mut engine);
    assert_eq!(
        engine.projection.children(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[1], nodes[2], nodes[3]]
    );
    assert_eq!(
        engine.projection.preorder(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[0], nodes[1], nodes[2], nodes[3]]
    );
    assert_eq!(engine.tree().connected_element_count(), 4);
}

#[test]
fn container_conditions_evaluate_over_any_record_source() {
    use super::container_queries::ContainerRecords;
    use crate::css::container_conditions::{ContainerConditionData, ContainerConditionsData};
    use crate::css::css_string::CssString;

    /// Records naming one element a query container by a name, which the installed state does not.
    struct NamedContainer {
        container: StyleNodeID,
        /// The name, as a raw fly string, which `_name` keeps alive.
        raw_name: usize,
        _name: ak::Utf16FlyString,
    }

    impl ContainerRecords for NamedContainer {
        fn query_row(&self, node: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>> {
            (node == self.container).then(|| tree::ContainerQueryInputs {
                style_record: 0,
                size_queries_available: true,
                names: std::slice::from_ref(&self.raw_name),
                is_size_container: false,
                is_inline_size_container: false,
                is_scroll_state_container: false,
                writing_mode: crate::css::css_enums::writing_mode::HORIZONTAL_TB,
                direction: crate::css::css_enums::direction::LTR,
            })
        }

        fn any_element_row(&self, _: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>> {
            None
        }

        fn record_view(
            &self,
            _: StyleNodeID,
            _: tree::ContainerQueryInputs<'_>,
        ) -> Option<computed::StyleRecordView<'_>> {
            None
        }

        fn custom_store(
            &self,
            _: StyleNodeID,
            _: tree::ContainerQueryInputs<'_>,
        ) -> Option<Option<&crate::css::custom_properties::CustomPropertyStore>> {
            Some(None)
        }
    }

    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 3];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<StyleNodeID> = raw.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)).collect();
    engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
    engine.record_tree_delta(nodes[1], None, Some(relations(Some(nodes[0].raw()), None, None)));
    engine.record_tree_delta(nodes[2], None, Some(relations(Some(nodes[1].raw()), None, None)));
    discard_transaction(&mut engine);

    let name: Vec<u16> = "card".encode_utf16().collect();
    let fly_name = ak::Utf16FlyString::from_utf16(&name);
    let records = NamedContainer {
        container: nodes[1],
        raw_name: fly_name.raw_identity(),
        _name: fly_name,
    };
    // `@container card`, which holds wherever a container of that name is above the subject.
    let conditions = [std::sync::Arc::new(ContainerConditionsData {
        conditions: Box::new([ContainerConditionData {
            name: Some(CssString::from_utf16(&name)),
            query: None,
        }]),
    })];
    let installed = engine
        .retained
        .container_conditions_verdict(&conditions, nodes[2], false)
        .expect("a name-only condition is decided");
    assert!(!installed.matches);
    let over_records = engine
        .retained
        .container_conditions_verdict_with(&records, &conditions, nodes[2], false)
        .expect("a name-only condition is decided");
    assert!(over_records.matches);
    // The container itself is no container of its own.
    let for_container = engine
        .retained
        .container_conditions_verdict_with(&records, &conditions, nodes[1], false)
        .expect("a name-only condition is decided");
    assert!(!for_container.matches);
}

#[test]
fn departing_scope_roots_are_removed_from_the_reverse_scope_index() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let first_root = StyleNodeID::from_raw(raw[0]).unwrap();
    let second_root = StyleNodeID::from_raw(raw[1]).unwrap();
    let scope = TreeScopeID(1);

    engine.record_tree_delta(first_root, None, Some(TreeRelations::detached(scope)));
    engine.set_tree_scope_root(scope, first_root);
    assert_eq!(engine.scope_by_root.get(first_root), Some(scope));

    engine.record_tree_delta(second_root, None, Some(TreeRelations::detached(scope)));
    engine.set_tree_scope_root(scope, second_root);
    assert_eq!(engine.scope_by_root.get(first_root), None);
    assert_eq!(engine.scope_by_root.get(second_root), Some(scope));
    discard_transaction(&mut engine);

    engine.record_tree_delta(second_root, Some(TreeRelations::detached(scope)), None);
    discard_transaction(&mut engine);
    assert_eq!(engine.scope_by_root.get(second_root), None);
    assert_eq!(engine.scope_roots[scope.0 as usize], None);
}

#[test]
fn an_element_arriving_and_departing_in_one_transaction_is_forgotten() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let node = StyleNodeID::from_raw(raw[0]).unwrap();
    let scope = TreeScopeID(1);
    let class = StyleAtomID(100);

    engine.record_tree_delta(node, None, Some(TreeRelations::detached(scope)));
    engine.set_tree_scope_root(scope, node);
    add_feature(&mut engine, node, LocalFeatureKey::Class(class));
    engine.record_tree_delta(node, Some(TreeRelations::detached(scope)), None);
    discard_transaction(&mut engine);

    assert!(engine.projection.facts(node).is_none());
    assert_eq!(engine.scope_by_root.get(node), None);
    assert_eq!(engine.scope_roots[scope.0 as usize], None);
}

#[test]
fn a_local_feature_written_back_within_a_transaction_ends_as_written_back() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 1];
    engine.allocate_style_nodes(&mut raw);
    let node = StyleNodeID::from_raw(raw[0]).unwrap();
    engine.record_tree_delta(node, None, Some(relations(None, None, None)));
    let [first, second] = [engine.intern_atom(0x15000), engine.intern_atom(0x15001)];
    engine.set_element_language(node, first);
    engine.set_element_heading_level(node, 1);
    engine.set_element_part_exposure(node, first);
    discard_transaction(&mut engine);

    // Each setter compares against what was last written, not against what the last transaction
    // committed: moving a feature away and back within one transaction leaves it where it was.
    engine.set_element_language(node, second);
    engine.set_element_heading_level(node, 2);
    engine.set_element_part_exposure(node, second);
    engine.set_element_language(node, first);
    engine.set_element_heading_level(node, 1);
    engine.set_element_part_exposure(node, first);
    let written = engine.projection.written_facts(node).unwrap();
    assert_eq!(written.language(), first);
    assert_eq!(written.heading_level(), 1);
    assert_eq!(written.part_exposure(), first);
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
            reaction: input_types::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        },
    );
    engine.record_tree_delta(nodes[3], Some(relations(Some(nodes[2].raw()), None, None)), None);

    let mut published = Vec::new();
    engine.run_style_update(nodes[0], |reactions| {
        published.extend(reactions.iter().map(|reaction| reaction.style_node));
    });
    assert!(!published.contains(&nodes[3].raw()));
}

#[test]
fn a_shadow_root_routes_without_being_published_as_a_style_output() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 2];
    engine.allocate_style_nodes(&mut raw);
    let host = StyleNodeID::from_raw(raw[0]).unwrap();
    let shadow_root = StyleNodeID::from_raw(raw[1]).unwrap();
    engine.record_tree_delta(host, None, Some(relations(None, None, None)));
    engine.record_tree_delta(shadow_root, None, Some(relations(Some(host.raw()), None, None)));
    engine.set_tree_scope_root(TreeScopeID(1), shadow_root);
    for node in [host, shadow_root] {
        set_atom_feature(&mut engine, node, LocalFeatureKey::TagName, StyleAtomID(100));
    }
    discard_transaction(&mut engine);

    engine.record_input(
        InputKey::ElementStyleInput(shadow_root),
        InputValue::ElementStyleInput {
            reaction: 0,
            inherited_style_groups: 0,
        },
        InputValue::ElementStyleInput {
            reaction: input_types::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        },
    );

    let mut published = Vec::new();
    engine.run_style_update(host, |reactions| {
        published.extend(reactions.iter().map(|reaction| reaction.style_node));
    });
    assert!(!published.contains(&shadow_root.raw()));
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
        engine.projection.children(nodes[0]).collect::<Vec<_>>(),
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
        engine.projection.children(nodes[0]).collect::<Vec<_>>(),
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
        engine.projection.children(nodes[0]).collect::<Vec<_>>(),
        vec![nodes[3], nodes[1], nodes[2]]
    );
}

// -- Stylesheet program deltas -----------------------------------------------------------

/// The selectors `text` writes.
fn test_selectors(text: &str) -> crate::css::selector::SelectorListRef {
    let parsed = crate::css::selector_parser::parse_selector_list(
        &text.encode_utf16().collect::<Vec<u16>>()[..],
        &[],
        crate::css::selector_parser::SelectorType::Standalone,
        crate::css::selector_parser::SelectorParsingMode::Standard,
    )
    .expect("a test writes valid selectors");
    crate::css::selector::SelectorListRef::pack(&parsed.selectors)
}

fn authoring_engine() -> (StyleEngine, SheetID, RuleID) {
    let mut engine = StyleEngine::new();
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    let mut contents = engine.rules().rule_version(rule).clone();
    contents.selectors = engine.retained.rules.start_of_selectors(sheet, &test_selectors(".c"));
    contents.declaration_block = Some(DeclarationBlockID::new(1));
    engine.replace_rule_version(rule, contents);
    discard_transaction(&mut engine);
    (engine, sheet, rule)
}

#[test]
fn a_declaration_edit_changes_only_its_rule() {
    let (mut engine, _, rule) = authoring_engine();
    let mut contents = engine.rules().rule_version(rule).clone();
    contents.declaration_block = Some(DeclarationBlockID::new(2));
    engine.replace_rule_version(rule, contents);

    let transaction = engine.take_transaction();
    assert!(transaction.has_no_element_inputs());
    assert_eq!(transaction.rule_changes.rules, vec![rule]);
    assert!(transaction.rule_changes.sheets.is_empty());
    assert!(transaction.rule_changes.reordered_scopes.is_empty());
    engine.release_transaction(transaction);
}

#[test]
fn a_selector_edit_changes_its_rule() {
    let (mut engine, sheet, rule) = authoring_engine();
    let first = engine.rules().rule_version(rule).selectors;
    let second = engine.retained.rules.start_of_selectors(sheet, &test_selectors(".d"));
    assert_ne!(first, second);
    let mut contents = engine.rules().rule_version(rule).clone();
    contents.selectors = second;
    engine.replace_rule_version(rule, contents);
    assert_eq!(engine.rules().rule_version(rule).selectors, first);
    assert_eq!(engine.current_rule_version(rule).selectors, second);

    let transaction = engine.take_transaction();
    assert_eq!(engine.rules().rule_version(rule).selectors, second);
    assert_eq!(transaction.rule_changes.rules, vec![rule]);
    engine.release_transaction(transaction);
}

#[test]
fn rule_metadata_commits_at_the_transaction_barrier() {
    let (mut engine, _, rule) = authoring_engine();
    engine.set_rule_gated_by_container_query(rule);
    engine.set_rule_in_a_layer(rule);

    assert!(!engine.rules.rule_is_gated_by_container_query(rule));
    assert!(engine.current_rule_is_gated_by_container_query(rule));
    assert!(engine.has_pending_transaction());

    let transaction = engine.take_transaction();
    assert!(engine.rules.rule_is_gated_by_container_query(rule));
    engine.release_transaction(transaction);
}

#[test]
fn reordering_a_sheet_is_a_cascade_change_not_a_selector_change() {
    let mut engine = StyleEngine::new();
    let first = engine.add_sheet(CascadeOrigin::Author);
    let second = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(first, TreeScopeID::DOCUMENT);
    engine.attach_sheet(second, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(first, None, RuleKind::Style);
    discard_transaction(&mut engine);

    engine.detach_sheet(first, TreeScopeID::DOCUMENT);
    engine.attach_sheet_before_sheet(first, Some(second), TreeScopeID::DOCUMENT);
    let transaction = engine.take_transaction();
    assert!(
        transaction.rule_changes.is_empty(),
        "a sheet put back where it was moves nothing"
    );
    engine.release_transaction(transaction);

    engine.detach_sheet(first, TreeScopeID::DOCUMENT);
    engine.attach_sheet_before_sheet(first, None, TreeScopeID::DOCUMENT);
    let transaction = engine.take_transaction();
    assert!(transaction.rule_changes.rules.is_empty());
    assert!(transaction.rule_changes.sheets.is_empty());
    assert_eq!(transaction.rule_changes.reordered_scopes, vec![TreeScopeID::DOCUMENT]);
    assert!(
        engine.rules().rule_version(rule).selectors == SheetListStart::NONE,
        "the sheet's rules were not rewritten"
    );
    engine.release_transaction(transaction);
}

#[test]
fn sheet_attachment_commits_at_the_transaction_barrier() {
    let mut engine = StyleEngine::new();
    let first = engine.add_sheet(CascadeOrigin::Author);
    let second = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(first, TreeScopeID::DOCUMENT);
    discard_transaction(&mut engine);

    engine.attach_sheet_before_sheet(second, Some(first), TreeScopeID::DOCUMENT);

    assert_eq!(engine.rules.sheets_in_scope(TreeScopeID::DOCUMENT), vec![first]);
    assert_eq!(
        engine.current_sheets_in_scope(TreeScopeID::DOCUMENT),
        vec![second, first]
    );
    let transaction = engine.take_transaction();
    assert_eq!(engine.rules.sheets_in_scope(TreeScopeID::DOCUMENT), vec![second, first]);
    engine.release_transaction(transaction);
}

#[test]
fn rules_arriving_with_their_sheet_are_carried_by_it() {
    let mut engine = StyleEngine::new();
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    engine.append_rule(sheet, None, RuleKind::Style);
    engine.append_rule(sheet, None, RuleKind::Style);
    let transaction = engine.take_transaction();
    assert!(transaction.rule_changes.rules.is_empty());
    assert_eq!(transaction.rule_changes.sheets, vec![sheet]);
    engine.release_transaction(transaction);

    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    let transaction = engine.take_transaction();
    assert_eq!(transaction.rule_changes.rules, vec![rule]);
    assert!(transaction.rule_changes.sheets.is_empty());
    engine.release_transaction(transaction);
}

#[test]
fn deleting_a_group_rule_changes_every_identity_it_took_with_it() {
    let mut engine = StyleEngine::new();
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let media = engine.append_rule(sheet, None, RuleKind::Media);
    let inner = engine.append_rule(sheet, Some(media), RuleKind::Style);
    discard_transaction(&mut engine);

    let removed = engine.remove_rule(media);
    assert_eq!(removed, vec![media, inner]);

    let transaction = engine.take_transaction();
    assert_eq!(transaction.rule_changes.rules, vec![media, inner]);
    engine.release_transaction(transaction);
}

#[test]
fn rule_existence_commits_at_the_transaction_barrier() {
    let mut engine = StyleEngine::new();
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    discard_transaction(&mut engine);

    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    assert!(!engine.rules.rule_is_live(rule));
    assert!(engine.current_rule_is_live(rule));
    let transaction = engine.take_transaction();
    assert!(engine.rules.rule_is_live(rule));
    engine.release_transaction(transaction);

    engine.remove_rule(rule);
    assert!(engine.rules.rule_is_live(rule));
    assert!(!engine.current_rule_is_live(rule));
    let transaction = engine.take_transaction();
    assert!(!engine.rules.rule_is_live(rule));
    engine.release_transaction(transaction);
}

#[test]
fn a_layer_order_change_reorders_its_scope() {
    let (mut engine, _, _) = authoring_engine();
    let base = CascadeLayerID(1);
    let theme = CascadeLayerID(2);
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[base, theme]);
    let transaction = engine.take_transaction();
    assert!(
        transaction.rule_changes.reordered_scopes.is_empty(),
        "a scope's first layer order moves no rule"
    );
    engine.release_transaction(transaction);

    engine.set_layer_order(TreeScopeID::DOCUMENT, &[theme, base]);
    engine.set_tree_scope_uses_document_sheets(TreeScopeID(1));

    assert_eq!(engine.rules.layer_index(TreeScopeID::DOCUMENT, base), 0);
    assert_eq!(engine.layer_index(TreeScopeID::DOCUMENT, base), 1);
    assert!(!engine.rules.scope_uses_document_sheets(TreeScopeID(1)));

    let transaction = engine.take_transaction();
    assert_eq!(engine.rules.layer_index(TreeScopeID::DOCUMENT, base), 1);
    assert!(engine.rules.scope_uses_document_sheets(TreeScopeID(1)));
    assert_eq!(transaction.rule_changes.reordered_scopes, vec![TreeScopeID::DOCUMENT]);
    assert_eq!(engine.counters().get(Counter::CascadeTopologyDeltas), 1);
    engine.release_transaction(transaction);
}

#[test]
fn disabling_a_sheet_is_an_activation_change() {
    let (mut engine, sheet, _) = authoring_engine();
    engine.set_sheet_enabled(sheet, false);
    assert!(engine.rules.sheet_is_enabled(sheet));

    let transaction = engine.take_transaction();
    assert!(!engine.rules.sheet_is_enabled(sheet));
    assert_eq!(transaction.rule_changes.sheets, vec![sheet]);
    engine.release_transaction(transaction);
}

#[test]
fn releasing_a_flush_transaction_does_not_reclaim_atoms() {
    let mut engine = StyleEngine::new();
    for raw in 0x1000..0x1100 {
        engine.intern_atom(raw);
    }

    let transaction = engine.take_transaction();
    engine.release_transaction(transaction);

    assert!(engine.host.reclaimed_style_atoms.is_empty());
    assert_eq!(engine.counters().get(Counter::AtomSweeps), 0);
}

#[test]
fn attribute_names_keep_all_noted_forms_live() {
    let (mut engine, nodes) = linear_document();
    let name = engine.intern_atom(0x1000);
    let any_namespace = engine.intern_qualified_atom(StyleAtomID::NONE, name);
    let folded_name = engine.intern_atom(0x1001);
    let folded_any_namespace = engine.intern_qualified_atom(StyleAtomID::NONE, folded_name);
    engine.note_attribute_name_forms(
        name,
        index::AttributeNameForms {
            local: any_namespace,
            folded_name,
            folded_local: folded_any_namespace,
        },
    );
    add_feature(&mut engine, nodes[1], LocalFeatureKey::Attribute(name));
    discard_transaction(&mut engine);
    for raw in 0x2000..0x2100 {
        engine.intern_atom(raw);
    }

    engine.sweep_style_atoms();

    let reclaimed = engine
        .host
        .reclaimed_style_atoms
        .iter()
        .map(|reclaimed| reclaimed.atom)
        .collect::<HashSet<_>>();
    assert!(!reclaimed.contains(&name));
    assert!(!reclaimed.contains(&any_namespace));
    assert!(!reclaimed.contains(&folded_name));
    assert!(!reclaimed.contains(&folded_any_namespace));
}

#[test]
fn an_arena_keeps_the_qualified_attribute_names_it_tests_live_across_a_sweep() {
    let mut engine = StyleEngine::new();
    engine.share_the_process_style_for_test();
    let utf16 = |text: &str| text.encode_utf16().collect::<Vec<u16>>();
    let (prefix, uri) = (utf16("q"), utf16("urn:arena-keeps-qualified-names"));
    let parsed = crate::css::selector_parser::parse_selector_list(
        &utf16("[q|Data-Kept]")[..],
        &[crate::css::css_tokenizer::TokenizerInput::Utf16(&prefix)],
        crate::css::selector_parser::SelectorType::Standalone,
        crate::css::selector_parser::SelectorParsingMode::Standard,
    )
    .expect("a test writes valid selectors");
    let selectors = parsed
        .into_list_declaring(
            crate::css::ffi_support::StyleNestingParent::None,
            crate::css::selector_arena::NamespaceDeclarations::of_texts([(&prefix[..], &uri[..])]),
        )
        .selectors;
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    let mut contents = engine.rules().rule_version(rule).clone();
    contents.selectors = engine.retained.rules.start_of_selectors(sheet, &selectors);
    contents.declaration_block = Some(DeclarationBlockID::new(1));
    engine.replace_rule_version(rule, contents);
    discard_transaction(&mut engine);

    // The host publishes an element's attributes by their qualified names, which the document interns.
    let names: Vec<ak::Utf16FlyString> = ["urn:arena-keeps-qualified-names", "Data-Kept", "data-kept"]
        .into_iter()
        .map(|name| ak::Utf16FlyString::from_utf16(&utf16(name)))
        .collect();
    let [namespace, local, folded] = [0, 1, 2].map(|index| engine.intern_atom(names[index].raw_identity()));
    let qualified = engine.intern_qualified_atom(namespace, local);
    let folded_qualified = engine.intern_qualified_atom(namespace, folded);
    let fillers: Vec<ak::Utf16FlyString> = (0..300)
        .map(|index| ak::Utf16FlyString::from_utf16(&utf16(&format!("arena-sweep-filler-{index}"))))
        .collect();
    for filler in &fillers {
        engine.intern_atom(filler.raw_identity());
    }

    engine.sweep_style_atoms();

    assert_eq!(engine.counters().get(Counter::AtomSweeps), 1);
    let reclaimed = engine
        .host
        .reclaimed_style_atoms
        .iter()
        .map(|reclaimed| reclaimed.atom)
        .collect::<HashSet<_>>();
    assert!(!reclaimed.is_empty());
    for kept in [qualified, folded_qualified, namespace, local, folded] {
        assert!(!reclaimed.contains(&kept));
    }
}

#[test]
fn a_sweep_keeps_the_names_a_rule_tests_in_any_namespace() {
    let mut engine = StyleEngine::new();
    engine.share_the_process_style_for_test();
    let utf16 = |text: &str| text.encode_utf16().collect::<Vec<u16>>();
    let parsed = crate::css::selector_parser::parse_selector_list(
        &utf16("[*|Data-Any-Kept]")[..],
        &[],
        crate::css::selector_parser::SelectorType::Standalone,
        crate::css::selector_parser::SelectorParsingMode::Standard,
    )
    .expect("a test writes valid selectors");
    let selectors = parsed
        .into_list_declaring(
            crate::css::ffi_support::StyleNestingParent::None,
            crate::css::selector_arena::NamespaceDeclarations::of_texts([]),
        )
        .selectors;
    let sheet = engine.add_sheet(CascadeOrigin::Author);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    let rule = engine.append_rule(sheet, None, RuleKind::Style);
    let mut contents = engine.rules().rule_version(rule).clone();
    contents.selectors = engine.retained.rules.start_of_selectors(sheet, &selectors);
    contents.declaration_block = Some(DeclarationBlockID::new(1));
    engine.replace_rule_version(rule, contents);
    engine.retained.note_selectors(&selectors, None);
    discard_transaction(&mut engine);
    let tested = engine.retained.selector_summary().tested_attribute_names;
    assert!(!tested.is_empty());

    let fillers: Vec<ak::Utf16FlyString> = (0..300)
        .map(|index| ak::Utf16FlyString::from_utf16(&utf16(&format!("any-namespace-sweep-filler-{index}"))))
        .collect();
    for filler in &fillers {
        engine.intern_atom(filler.raw_identity());
    }
    engine.sweep_style_atoms();

    assert_eq!(engine.counters().get(Counter::AtomSweeps), 1);
    let kept = engine.retained.selector_summary().tested_attribute_names;
    for name in tested {
        assert!(kept.contains(&name), "the sweep made {name:?} known unread");
    }
}

#[test]
fn engine_atom_reuse_replaces_custom_property_names() {
    let mut engine = StyleEngine::new();
    let old_name = engine.intern_atom(0x1000);
    let old_text = ak::Utf16FlyString::from_utf16(&[u16::from(b'-'), u16::from(b'-'), u16::from(b'o')]);
    unsafe {
        engine.note_custom_property_name(old_name, old_text.raw_identity());
    }
    for raw in 0x2000..0x2100 {
        engine.intern_atom(raw);
    }

    engine.sweep_style_atoms();

    assert!(
        engine
            .host
            .reclaimed_style_atoms
            .iter()
            .any(|reclaimed| reclaimed.atom == old_name)
    );
    engine.host.reclaimed_style_atoms.clear();

    let new_name = engine.intern_atom(0x3000);
    assert_eq!(new_name, old_name);
    let new_text = ak::Utf16FlyString::from_utf16(&[u16::from(b'-'), u16::from(b'-'), u16::from(b'n')]);
    unsafe {
        engine.note_custom_property_name(new_name, new_text.raw_identity());
    }
    assert_eq!(
        engine.custom_property_environments.name(new_name).unwrap().raw(),
        new_text.raw_identity()
    );
}

#[test]
fn owed_element_style_inputs_fold_into_covering_reactions() {
    use super::input_types::STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED;
    let mut engine = StyleEngine::new();
    let mut raw_nodes = [0; 1];
    engine.allocate_style_nodes(&mut raw_nodes);
    let node = StyleNodeID::from_raw(raw_nodes[0]).unwrap();

    engine.record_derived_element_style_input(node, STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED, 0b0010);
    assert!(engine.has_deferred_element_style_input(node));
    // A record delta covers only what it already carries.
    assert_eq!(
        engine.absorb_element_style_input(node, STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED, 0b0100),
        0
    );
    assert!(engine.has_deferred_element_style_input(node));
    assert_eq!(
        engine.absorb_element_style_input(node, STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED, 0b0110),
        u32::from(STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED) | (0b0110 << 8)
    );
    assert!(!engine.has_deferred_element_style_input(node));
}

#[test]
fn held_style_records_outlive_engine_assignments_and_release_on_retirement() {
    let mut engine = StyleEngine::new();
    let mut raw_nodes = [0; 2];
    engine.allocate_style_nodes(&mut raw_nodes);
    let [first, second] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
    let publish = |engine: &mut StyleEngine, node, pseudo_element_styles| {
        engine
            .install_test_record(
                computed::ComputedStyleTarget::new(node, u8::MAX),
                None,
                None,
                pseudo_element_styles,
            )
            .raw()
    };
    let held = |engine: &StyleEngine, record| engine.computed_group_sets.host_record(record).cloned();
    let held_record = publish(&mut engine, first, 1);
    assert_eq!(publish(&mut engine, second, 1), held_record);
    let record = held(&engine, held_record);
    engine.set_held_style_record(first, record);
    let record = held(&engine, held_record);
    engine.set_held_style_record(first, record);
    let record = held(&engine, held_record);
    engine.set_held_style_record(second, record);

    // The engine can publish a replacement before the host installs it.
    let replacement = publish(&mut engine, first, 2);
    engine.computed_group_sets.remove(second);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(engine.computed_group_sets.final_style_record_is_live(held_record));

    let record = held(&engine, replacement);
    engine.set_held_style_record(first, record);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(engine.computed_group_sets.final_style_record_is_live(held_record));
    assert!(engine.computed_group_sets.final_style_record_is_live(replacement));
    assert_eq!(engine.held_style_record(first), Some(replacement));

    // Clearing one holder must not release another holder's shared record.
    engine.set_held_style_record(first, None);
    engine.computed_group_sets.remove(first);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(engine.computed_group_sets.final_style_record_is_live(held_record));
    assert!(!engine.computed_group_sets.final_style_record_is_live(replacement));

    engine.retire_node_state(second);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(!engine.computed_group_sets.final_style_record_is_live(held_record));
}

#[test]
fn pseudo_element_records_the_host_references_outlive_engine_assignments() {
    use super::publication::pseudo_kind::BEFORE;
    let mut engine = StyleEngine::new();
    let mut raw_node = [0];
    engine.allocate_style_nodes(&mut raw_node);
    let node = StyleNodeID::from_raw(raw_node[0]).unwrap();
    let publish = |engine: &mut StyleEngine, pseudo_kind, pseudo_element_styles| {
        engine
            .install_test_record(
                computed::ComputedStyleTarget::new(node, pseudo_kind),
                None,
                None,
                pseudo_element_styles,
            )
            .raw()
    };
    let held_record = publish(&mut engine, BEFORE, 1);
    let host_reference = engine.computed_group_sets.host_record(held_record).cloned();
    engine.set_held_pseudo_style_record(node, pipeline::PseudoElementTarget::of_kind(BEFORE), held_record);

    // The engine can publish a replacement before the host installs it.
    let replacement = publish(&mut engine, BEFORE, 2);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(engine.computed_group_sets.final_style_record_is_live(held_record));

    drop(host_reference);
    engine.set_held_pseudo_style_record(node, pipeline::PseudoElementTarget::of_kind(BEFORE), replacement);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(!engine.computed_group_sets.final_style_record_is_live(held_record));
    assert!(engine.computed_group_sets.final_style_record_is_live(replacement));

    engine.retire_node_state(node);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(!engine.computed_group_sets.final_style_record_is_live(replacement));
}

#[test]
fn a_hover_frame_lease_keeps_only_the_records_let_go_of_since_it_was_taken() {
    let mut engine = StyleEngine::new();
    let mut raw_node = [0];
    engine.allocate_style_nodes(&mut raw_node);
    let target = computed::ComputedStyleTarget::new(StyleNodeID::from_raw(raw_node[0]).unwrap(), u8::MAX);
    let first = engine.install_test_record(target, None, None, 0).raw();
    let named_by_the_frame = engine.install_test_record(target, None, None, 1).raw();

    let lease = engine.lease_hover_frame_style_records(Default::default());
    let last = engine.install_test_record(target, None, None, 2).raw();
    engine.computed_group_sets.reclaim_unreachable();
    assert!(!engine.computed_group_sets.final_style_record_is_live(first));
    assert!(
        engine
            .computed_group_sets
            .final_style_record_is_live(named_by_the_frame)
    );

    drop(lease);
    engine.computed_group_sets.reclaim_unreachable();
    assert!(
        !engine
            .computed_group_sets
            .final_style_record_is_live(named_by_the_frame)
    );
    assert!(engine.computed_group_sets.final_style_record_is_live(last));
}

#[test]
fn records_left_unreachable_under_a_lease_are_reclaimed_once_it_drops() {
    let mut engine = StyleEngine::new();
    let mut raw_node = [0];
    engine.allocate_style_nodes(&mut raw_node);
    let target = computed::ComputedStyleTarget::new(StyleNodeID::from_raw(raw_node[0]).unwrap(), u8::MAX);
    let mut publish = |pseudo_element_styles| {
        engine
            .install_test_record(target, None, None, pseudo_element_styles)
            .raw()
    };
    let first = publish(0);
    // The next record replaces the first, which nothing holds then.
    publish(1);

    let lease = engine.lease_style_records();
    engine.begin_style_record_view_epoch();
    engine.end_style_record_view_epoch();
    assert!(engine.computed_group_sets.final_style_record_is_live(first));

    // The recording lands with no style operation after it, and its landing still reclaims what the lease kept.
    drop(lease);
    engine.free_style_records_kept_for_leases();
    assert!(!engine.computed_group_sets.final_style_record_is_live(first));
}

struct StyleReplacement {
    arena: LayoutNodeArena,
    engine: Box<StyleEngine>,
    target: computed::ComputedStyleTarget,
    row: NodeSlotId,
    base: u64,
    cached_fragment_epoch: u32,
}

impl StyleReplacement {
    fn new() -> Self {
        let mut engine = Box::new(StyleEngine::new());
        let mut nodes = [0];
        engine.allocate_style_nodes(&mut nodes);
        let node = StyleNodeID::from_raw(nodes[0]).unwrap();
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let base = engine.install_test_record(target, None, None, 0).raw();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(StyleEngineHandle::from_raw(&raw mut *engine));
        let row = arena.allocate_unbound();
        arena.stamp_dom_row(row, NodeKind::BlockContainer, Some(node));
        arena.bind_row(row);
        arena.stamp_published_style(row, base);
        Self {
            arena,
            engine,
            target,
            row,
            base,
            cached_fragment_epoch: 0,
        }
    }

    fn overlay(&mut self, groups: &StyleGroups) -> DerivedStyleRecord {
        let overlay = AnimatedOverlay::default();
        let record = self
            .engine
            .computed_group_sets
            .publish_animation_overlay(self.target, 1, HostShared::new(&raw const overlay), Some(groups), false)
            .unwrap()
            .style_record
            .raw();
        let pinned = DerivedStyleRecord::pin(&mut self.engine, record);
        self.engine.computed_group_sets.publish_animation_overlay(
            self.target,
            0,
            HostShared::new(std::ptr::null()),
            None,
            false,
        );
        assert_eq!(Arc::strong_count(self.engine.host_record(record).unwrap()), 1);
        pinned
    }

    fn cache_layout(&mut self) {
        self.cached_fragment_epoch = self.arena.data(self.row).fragment_cache_epoch;
    }

    fn keeps_cached_layout(&self) -> bool {
        self.arena.data(self.row).fragment_cache_epoch == self.cached_fragment_epoch
    }
}

impl Drop for StyleReplacement {
    fn drop(&mut self) {
        self.arena
            .free_subtree(self.row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        self.arena.set_style_engine(StyleEngineHandle::null());
    }
}

fn groups_with_width(width: f64) -> StyleGroups {
    let mut groups = DefaultGroups::default().groups().clone();
    // SAFETY: SIZING names SizingValues, and write_group gives this style its own copy.
    unsafe {
        groups.write_group::<SizingValues>(group_index::SIZING, |sizing| {
            sizing.width = ComputedSize::from_data(&RetainedStyleValueData::from_owned(StyleValueData::Length {
                value: width,
                unit: crate::css::style_compute::px_length_unit(),
            }));
        });
    }
    groups
}

#[test]
fn replacing_a_published_overlay_compares_it_before_releasing_its_last_pin() {
    for changes_layout in [false, true] {
        let mut test = StyleReplacement::new();
        let groups = if changes_layout {
            groups_with_width(100.0)
        } else {
            DefaultGroups::default().groups().clone()
        };
        let previous = test.overlay(&groups);
        drop(groups);
        test.arena.stamp_published_style(test.row, previous.record);
        test.engine.unpin_layout_style_record(previous.record);
        test.cache_layout();
        let payloads = LayoutStyleRef::of_groups(test.engine.style_record_payloads(test.base).unwrap());

        test.arena
            .install_style_record(test.row, test.base, Some(payloads), Some(Some(previous.payloads)));

        assert_eq!(test.arena.node_style_record(test.row), test.base);
        assert_eq!(test.keeps_cached_layout(), !changes_layout);
        assert!(test.engine.host_record(previous.record).is_none());
    }
}

#[test]
fn replacing_and_restoring_samples_keep_the_previous_record_through_comparison() {
    let mut test = StyleReplacement::new();
    let first = test.overlay(&groups_with_width(100.0));
    let host_style = test
        .arena
        .install_sample_beside_anonymous_boxes(test.row, first, StyleRelayout::ByLayoutAffectingGroups)
        .expect("the first sample takes the host's style");
    test.cache_layout();

    let second = test.overlay(&groups_with_width(200.0));
    assert!(
        test.arena
            .install_sample_beside_anonymous_boxes(test.row, second, StyleRelayout::ByLayoutAffectingGroups)
            .is_none()
    );
    assert!(test.engine.host_record(first.record).is_none());
    assert!(test.engine.host_record(second.record).is_some());
    assert!(!test.keeps_cached_layout());

    let same = DerivedStyleRecord::pin(&mut test.engine, second.record);
    test.cache_layout();
    assert!(
        test.arena
            .install_sample_beside_anonymous_boxes(test.row, same, StyleRelayout::ByLayoutAffectingGroups)
            .is_none()
    );
    assert!(test.keeps_cached_layout());
    assert!(test.engine.host_record(second.record).is_some());

    test.arena
        .restore_host_style(test.row, host_style, StyleRelayout::ByLayoutAffectingGroups);
    assert_eq!(test.arena.node_style_record(test.row), test.base);
    assert!(!test.keeps_cached_layout());
    assert!(test.engine.host_record(second.record).is_none());
}

#[test]
fn a_reissued_identity_holds_no_construction_facts() {
    use super::bridge::element_construction_fact::{IS_BODY, IS_EDITING_HOST, IS_IN_USER_AGENT_SHADOW_TREE};
    let (mut engine, nodes) = linear_document();
    let leaving = nodes[3];
    engine.set_element_construction_facts(leaving, IS_BODY | IS_EDITING_HOST);
    assert_eq!(engine.element_construction_facts(leaving), IS_BODY | IS_EDITING_HOST);

    let mut text = [0_u32; 1];
    engine.allocate_text_style_nodes(&mut text);
    let text = StyleNodeID::from_raw(text[0]).unwrap();
    engine.set_text_is_in_user_agent_shadow_tree(text, true);
    assert_eq!(engine.element_construction_facts(text), IS_IN_USER_AGENT_SHADOW_TREE);

    engine.record_tree_delta(
        leaving,
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
        None,
    );
    engine.retire_text_style_nodes([text]);
    discard_transaction(&mut engine);
    engine.discard_style_update_outputs();
    let mut reissued = [0_u32; 1];
    engine.allocate_style_nodes(&mut reissued);
    assert_eq!(reissued[0], leaving.raw());
    assert_eq!(engine.element_construction_facts(leaving), 0);
    let mut reissued_text = [0_u32; 1];
    engine.allocate_text_style_nodes(&mut reissued_text);
    assert_eq!(reissued_text[0], text.raw());
    assert_eq!(engine.element_construction_facts(text), 0);
}

#[test]
fn a_reissued_identity_asks_for_no_particular_box() {
    use super::bridge::ElementBoxKind;
    let (mut engine, nodes) = linear_document();
    let leaving = nodes[3];
    assert_eq!(engine.element_box_kind(leaving), ElementBoxKind::FromDisplay);
    engine.set_element_box_kind(leaving, ElementBoxKind::Image);
    assert_eq!(engine.element_box_kind(leaving), ElementBoxKind::Image);

    engine.record_tree_delta(
        leaving,
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
        None,
    );
    discard_transaction(&mut engine);
    engine.discard_style_update_outputs();
    let mut reissued = [0_u32; 1];
    engine.allocate_style_nodes(&mut reissued);
    assert_eq!(reissued[0], leaving.raw());
    assert_eq!(engine.element_box_kind(leaving), ElementBoxKind::FromDisplay);
}

#[test]
fn a_reissued_identity_has_no_replaced_content_input() {
    use super::ReplacedContentInput;
    let (mut engine, nodes) = linear_document();
    let leaving = nodes[3];
    let input = ReplacedContentInput::Canvas {
        width: 300,
        height: 150,
    };
    engine.set_element_replaced_content_input(leaving, input);
    assert_eq!(engine.element_replaced_content_input(leaving), input);

    engine.record_tree_delta(
        leaving,
        Some(relations(Some(nodes[0].raw()), Some(nodes[2].raw()), None)),
        None,
    );
    discard_transaction(&mut engine);
    engine.discard_style_update_outputs();
    let mut reissued = [0_u32; 1];
    engine.allocate_style_nodes(&mut reissued);
    assert_eq!(reissued[0], leaving.raw());
    assert_eq!(
        engine.element_replaced_content_input(leaving),
        ReplacedContentInput::None
    );
}

#[test]
fn a_text_node_holds_its_published_characters_until_its_identity_is_reissued() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 1];
    engine.allocate_text_style_nodes(&mut raw);
    let text = StyleNodeID::from_raw(raw[0]).unwrap();
    let data = ak::Utf16String::from(ak::Utf16FlyString::from_utf16(
        &"characters a text box renders".encode_utf16().collect::<Vec<_>>(),
    ));
    engine.set_text_data(text, data.clone());
    engine.set_text_is_password_input(text, true);
    // The mirror shares the document's string rather than copying it.
    assert_eq!(
        engine.tree.text_data(text).map(ak::Utf16String::raw_identity),
        Some(data.raw_identity())
    );
    assert!(engine.tree.text_is_password_input(text));

    engine.retire_text_style_nodes([text]);
    engine.discard_style_update_outputs();
    let mut reissued = [0_u32; 1];
    engine.allocate_text_style_nodes(&mut reissued);
    assert_eq!(reissued[0], text.raw());
    assert!(engine.tree.text_data(text).is_some_and(ak::Utf16String::is_empty));
    assert!(!engine.tree.text_is_password_input(text));
}

#[test]
fn inheritance_parent_keeps_the_dom_parent_of_nodes_outside_the_flat_tree() {
    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 7];
    engine.allocate_style_nodes(&mut raw);
    let nodes: Vec<_> = raw.iter().map(|&raw| StyleNodeID::from_raw(raw).unwrap()).collect();
    let [host, assigned, unassigned, shadow_root, wrapper, slot, fallback] = nodes.as_slice() else {
        unreachable!()
    };

    let projection = &mut engine.retained.projection;
    projection.set_first_element_child(*host, Some(*assigned));
    projection.set_parent(*assigned, Some(*host));
    projection.set_next_element_sibling(*assigned, Some(*unassigned));
    projection.set_previous_element_sibling(*unassigned, Some(*assigned));
    projection.set_parent(*unassigned, Some(*host));
    projection.set_first_element_child(*shadow_root, Some(*wrapper));
    projection.set_parent(*wrapper, Some(*shadow_root));
    projection.set_first_element_child(*wrapper, Some(*slot));
    projection.set_parent(*slot, Some(*wrapper));
    projection.set_first_element_child(*slot, Some(*fallback));
    projection.set_parent(*fallback, Some(*slot));
    projection.set_shadow_root(*host, *shadow_root);
    projection.set_assigned_slot(*assigned, Some(*slot));
    projection.set_assigned_nodes(*slot, &[*assigned]);
    projection.commit();
    projection.release();

    let tree = &engine.retained.projection;
    // A slotted element inherits from its slot, and a shadow tree's top-level element from the host.
    assert_eq!(tree.inheritance_parent(*assigned), Some(*slot));
    assert_eq!(tree.inheritance_parent(*wrapper), Some(*host));
    assert_eq!(tree.inheritance_parent(*slot), Some(*wrapper));
    assert_eq!(tree.inheritance_parent(*host), None);
    // A host's child no slot takes, and a slot's fallback while it has assigned nodes, are outside
    // the flat tree but still inherit from their DOM parent.
    assert_eq!(tree.flat_tree_parent(*unassigned), None);
    assert_eq!(tree.inheritance_parent(*unassigned), Some(*host));
    assert_eq!(tree.flat_tree_parent(*fallback), None);
    assert_eq!(tree.inheritance_parent(*fallback), Some(*slot));
}

#[test]
fn shadow_text_line_break_language_follows_host_mutation() {
    use crate::layout::text_chunker::LineBreakLanguage;

    let mut engine = StyleEngine::new();
    let mut raw = [0_u32; 4];
    engine.allocate_style_nodes(&mut raw);
    let [document, host, shadow_root, wrapper] = raw.map(|raw| StyleNodeID::from_raw(raw).unwrap());
    engine.record_tree_delta(document, None, Some(relations(None, None, None)));
    engine.record_tree_delta(host, None, Some(relations(Some(document.raw()), None, None)));
    engine.record_tree_delta(shadow_root, None, Some(TreeRelations::detached(TreeScopeID(1))));
    engine.record_tree_delta(wrapper, None, Some(relations(Some(shadow_root.raw()), None, None)));
    engine.set_shadow_root(host, shadow_root);
    engine.settle_tree_at_barrier();
    let mut raw_text = [0_u32; 5];
    engine.allocate_text_style_nodes(&mut raw_text);
    let [direct, wrapped, light, under_document, detached] = raw_text.map(|raw| StyleNodeID::from_raw(raw).unwrap());
    engine.link_style_nodes_in_dom_order(&[
        direct.raw(),
        shadow_root.raw(),
        0,
        wrapped.raw(),
        wrapper.raw(),
        0,
        light.raw(),
        host.raw(),
        0,
        under_document.raw(),
        document.raw(),
        0,
    ]);
    let german = engine.intern_atom(0x13393);
    engine.set_element_language_text(german, &"de".encode_utf16().collect::<Vec<_>>());
    engine.set_element_language(wrapper, german);

    for (index, (tag, language)) in [
        ("ja", LineBreakLanguage::Japanese),
        ("de", LineBreakLanguage::Other),
        ("zh-Hant", LineBreakLanguage::Chinese),
        ("JA-jp", LineBreakLanguage::Japanese),
        ("ko-Hani", LineBreakLanguage::Chinese),
        ("ja-Latn", LineBreakLanguage::Other),
        ("zh-Hira", LineBreakLanguage::Japanese),
        ("", LineBreakLanguage::Other),
    ]
    .into_iter()
    .enumerate()
    {
        let atom = engine.intern_atom(0x14000 + index);
        let units = tag.encode_utf16().collect::<Vec<_>>();
        engine.set_element_language_text(atom, &units);
        engine.set_element_language(host, atom);
        discard_transaction(&mut engine);
        let retained = &engine.retained;
        assert_eq!(retained.text_line_break_language_tag(direct), units, "{tag}");
        assert_eq!(retained.text_line_break_language_tag(light), units, "{tag}");
        assert!(LineBreakLanguage::from_tag(retained.text_line_break_language_tag(direct)) == language);
        assert_eq!(
            retained.text_line_break_language_tag(wrapped),
            "de".encode_utf16().collect::<Vec<_>>()
        );
        assert!(retained.text_line_break_language_tag(under_document).is_empty());
        assert!(retained.text_line_break_language_tag(detached).is_empty());
        // The existing casing resolver keeps its separate DOM-parent contract.
        assert!(retained.text_language_tag(direct).is_empty());
    }
}

pub(super) fn staged_hover_pending_shadow_fixture() -> (StyleEngine, [StyleNodeID; 10]) {
    let mut engine = StyleEngine::new();
    let mut raw = [0; 10];
    engine.allocate_style_nodes(&mut raw);
    let nodes = raw.map(|raw| StyleNodeID::from_raw(raw).unwrap());
    engine.mark_relation_only_style_node(nodes[0]);
    engine.set_element_adjustment_facts(nodes[1], super::bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT);
    // document, document element, host, shadow root, outer popover, slot A, light child, inner popover, slot B, fallback
    let parents = [
        None,
        None,
        Some(1),
        None,
        Some(3),
        Some(4),
        Some(2),
        Some(4),
        Some(7),
        Some(5),
    ];
    for (index, node) in nodes.iter().copied().enumerate() {
        if index == 0 {
            continue;
        }
        let mut row = TreeRelations::detached(if [3, 4, 5, 7, 8, 9].contains(&index) {
            TreeScopeID(1)
        } else {
            TreeScopeID::DOCUMENT
        });
        row.parent = parents[index].map(|index| nodes[index]);
        if index == 5 {
            row.next_element_sibling = Some(nodes[7]);
        }
        if index == 7 {
            row.previous_element_sibling = Some(nodes[5]);
        }
        if index == 6 {
            row.assigned_slot = Some(nodes[5]);
        }
        engine.record_tree_delta(node, None, Some(row));
    }
    for (parent, child) in [(1, 2), (2, 6), (3, 4), (4, 5), (5, 9), (7, 8)] {
        engine.stage_first_child(nodes[parent], Some(nodes[child]));
    }
    engine.set_shadow_root(nodes[2], nodes[3]);
    engine.set_slot_assigned_nodes(nodes[5], &[nodes[6]]);
    engine.tree.link_in_dom_order(nodes[1], Some(nodes[0]), None);
    (engine, nodes)
}

pub(super) fn staged_hover_shadow_fixture() -> (StyleEngine, [StyleNodeID; 10]) {
    let (mut engine, nodes) = staged_hover_pending_shadow_fixture();
    discard_transaction(&mut engine);
    assert_eq!(engine.projection.parent(nodes[1]), None);
    assert_eq!(engine.projection.first_element_child(nodes[0]), None);
    assert_eq!(engine.tree.dom_children(nodes[0]).collect::<Vec<_>>(), vec![nodes[1]]);
    assert!(nodes.iter().all(|&node| engine.tree.is_live(node)));
    assert_eq!(engine.projection.flat_tree_parent(nodes[6]), Some(nodes[5]));
    assert_eq!(engine.projection.flat_tree_parent(nodes[4]), Some(nodes[2]));
    assert_eq!(engine.projection.flat_tree_parent(nodes[9]), None);
    (engine, nodes)
}

pub(super) fn assert_staged_hover_facts(engine: &StyleEngine, nodes: &[StyleNodeID; 10], expected: &[usize]) {
    let actual: Vec<_> = nodes
        .iter()
        .enumerate()
        .filter_map(|(i, &node)| engine.projection.hover_is_placed_on(node).then_some(i))
        .collect();
    assert_eq!(actual, expected);
}

#[test]
fn staged_hover_slotted_top_layer_uses_flat_ancestors() {
    let (mut engine, nodes) = staged_hover_shadow_fixture();
    engine.set_element_adjustment_facts(nodes[4], bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER);
    engine.set_hover(Some(nodes[6]));
    assert_staged_hover_facts(&engine, &nodes, &[4, 5, 6]);
}

#[test]
fn staged_hover_reads_pending_slot_reassignment_in_both_directions() {
    let (mut engine, nodes) = staged_hover_shadow_fixture();
    engine.set_hover(Some(nodes[6]));
    let before = engine.settled_tree_relations(nodes[6]);
    let mut after = before;
    after.assigned_slot = Some(nodes[8]);
    engine.record_tree_delta(nodes[6], Some(before), Some(after));
    assert_eq!(engine.projection.assigned_slot_of(nodes[6]), Some(nodes[5]));
    engine.set_hover(Some(nodes[6]));
    assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 6, 7, 8]);
    engine.set_slot_assigned_nodes(nodes[5], &[]);
    engine.set_slot_assigned_nodes(nodes[8], &[nodes[6]]);
    discard_transaction(&mut engine);
    engine.record_tree_delta(nodes[6], Some(after), Some(before));
    assert_eq!(engine.projection.assigned_slot_of(nodes[6]), Some(nodes[8]));
    engine.set_hover(Some(nodes[6]));
    assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 6]);
}

#[test]
fn staged_hover_unassigned_light_child_has_no_flat_chain() {
    let (mut engine, nodes) = staged_hover_shadow_fixture();
    engine.set_hover(Some(nodes[6]));
    let before = engine.settled_tree_relations(nodes[6]);
    let mut after = before;
    after.assigned_slot = None;
    engine.record_tree_delta(nodes[6], Some(before), Some(after));
    engine.set_slot_assigned_nodes(nodes[5], &[]);
    engine.set_hover(Some(nodes[6]));
    assert_eq!(engine.hover_target(), Some(nodes[6]));
    assert_staged_hover_facts(&engine, &nodes, &[]);
}

#[test]
fn staged_hover_fallback_observes_current_element_and_text_assignments() {
    let (mut engine, nodes) = staged_hover_shadow_fixture();
    engine.set_hover(Some(nodes[9]));
    assert_eq!(engine.hover_target(), Some(nodes[9]));
    assert_staged_hover_facts(&engine, &nodes, &[]);
    engine.set_slot_assigned_nodes(nodes[5], &[]);
    engine.set_hover(Some(nodes[9]));
    assert_staged_hover_facts(&engine, &nodes, &[1, 2, 4, 5, 9]);
    let mut raw_text = [0];
    engine.allocate_text_style_nodes(&mut raw_text);
    let text = StyleNodeID::from_raw(raw_text[0]).unwrap();
    engine.set_slot_assigned_nodes(nodes[5], &[text]);
    assert!(
        engine
            .projection
            .assigned_nodes_of(nodes[5])
            .iter()
            .all(|node| node.text_index().is_some())
    );
    engine.set_hover(Some(nodes[9]));
    assert_eq!(engine.hover_target(), Some(nodes[9]));
    assert_staged_hover_facts(&engine, &nodes, &[]);
}

#[test]
fn staged_hover_pending_shadow_insertion_jumps_to_host() {
    let (mut engine, nodes) = staged_hover_shadow_fixture();
    let before = engine.settled_tree_relations(nodes[6]);
    let mut after = before;
    after.parent = Some(nodes[3]);
    after.assigned_slot = None;
    after.tree_scope = TreeScopeID(1);
    engine.record_tree_delta(nodes[6], Some(before), Some(after));
    engine.set_slot_assigned_nodes(nodes[5], &[]);
    assert_eq!(engine.projection.parent(nodes[6]), Some(nodes[2]));
    engine.set_hover(Some(nodes[6]));
    assert_staged_hover_facts(&engine, &nodes, &[1, 2, 6]);
}

#[test]
fn staged_hover_nearest_top_layer_changes_with_same_target() {
    let (mut engine, nodes) = staged_hover_shadow_fixture();
    let before = engine.settled_tree_relations(nodes[6]);
    let mut after = before;
    after.assigned_slot = Some(nodes[8]);
    engine.record_tree_delta(nodes[6], Some(before), Some(after));
    engine.set_slot_assigned_nodes(nodes[5], &[]);
    engine.set_slot_assigned_nodes(nodes[8], &[nodes[6]]);
    discard_transaction(&mut engine);
    for (outer, inner, expected) in [
        (true, true, &[6, 7, 8][..]),
        (true, false, &[4, 6, 7, 8][..]),
        (false, true, &[6, 7, 8][..]),
        (false, false, &[1, 2, 4, 6, 7, 8][..]),
    ] {
        engine.set_element_adjustment_facts(
            nodes[4],
            if outer {
                bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER
            } else {
                0
            },
        );
        engine.set_element_adjustment_facts(
            nodes[7],
            if inner {
                bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER
            } else {
                0
            },
        );
        engine.set_hover(Some(nodes[6]));
        assert_staged_hover_facts(&engine, &nodes, expected);
    }
}
