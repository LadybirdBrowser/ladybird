/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Seeded mutation batches over a generated document, each checked by restyling it with live style
//! the engine and comparing every record against a cold pass over the same document, and what that pass
//! matched against the reference matcher.

use super::super::bridge::element_adjustment_fact;
use super::super::*;
use super::counters::StyleCounter;
use super::rules::SEGMENT_MIN_ENTRIES;
use super::test_support::{
    add_class, add_feature, add_rules, add_sheet, add_user_agent_sheet, allocate_nodes, assert_live_matches_cold, atom,
    attach_shadow_tree, describe, document_with_children, hosted_engine, linear_document, nested_document, relations,
    remove_class, remove_feature, run_style_update, set_atom_feature, set_tag, start_live, start_live_on,
};

// Cover hundreds of committed mutation batches while staying cheap enough for every Rust run.
const CONTAINERS: usize = 6;
const CHILDREN: usize = 8;
const CLASSES: usize = 8;
const RULES: usize = 16;
const SEEDS: u64 = 12;

/// How many seeds the seeded workloads run, which `LIBWEB_STYLE_FUZZ_SEEDS` raises for a long run.
fn seeds() -> u64 {
    std::env::var("LIBWEB_STYLE_FUZZ_SEEDS")
        .ok()
        .and_then(|seeds| seeds.parse().ok())
        .unwrap_or(SEEDS)
}
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

fn class_name(index: usize) -> String {
    format!("class{index}")
}

fn tag_name(node: StyleNodeID) -> &'static str {
    ["div", "span", "p"][node.raw() as usize % 3]
}

/// The style rule of `index`: one of eight selector shapes over three of the classes.
fn rule_source(index: usize) -> String {
    let a = class_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let c = class_name((index + 5) % CLASSES);
    let selector = match index % 8 {
        0 => format!(".{a}"),
        1 => format!(".{a} .{b}"),
        2 => format!(".{a} > .{b}"),
        3 => format!(".{a} ~ .{b}"),
        4 => format!(".{a} + .{b}"),
        5 => format!(".{a} ~ .{b} .{c}"),
        6 => format!(".{a} .{b} ~ .{c}"),
        _ => format!(".{a} + .{b} + .{c}"),
    };
    format!("{selector} {{ {}; }}", declaration_of(index))
}

/// The declaration of the rule of `index`: one of five properties, important for every seventh
/// rule.
fn declaration_of(index: usize) -> String {
    // NB: Inherited and font-relative properties make a match reach the matched element's
    //     descendants and the other properties of the element.
    let declaration = match index % 5 {
        0 => format!("color: rgb({}, 0, 0)", 16 * index),
        1 => format!("width: {}px", 1 + index),
        2 => format!("font-size: {}em", 1 + index % 3),
        3 => format!("margin-left: {}em", 1 + index),
        _ => format!("padding-top: {}px", 1 + index),
    };
    let important = if index % 7 == 0 { " !important" } else { "" };
    format!("{declaration}{important}")
}

/// The style rule of `index` for the workload of rules that mostly lose to one another: selectors reaching
/// across siblings, over a few properties, written as longhands, logically, as shorthands and to roll back.
fn losing_rule_source(index: usize) -> String {
    let a = class_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let selector = match index % 7 {
        0 => format!(".{a}"),
        1 => format!(".{a}.{b}"),
        2 => format!(".{a} + .{b}"),
        3 => format!(".{a} ~ .{b}"),
        4 => format!(".{a} .{b}"),
        5 => format!(".{a}:not(.{b})"),
        _ => "div".to_owned(),
    };
    let value = index + 1;
    let declaration = match index % 11 {
        0 | 1 => format!("color: rgb({value}, 0, 0)"),
        2 => format!("color: rgb({value}, 0, 0) !important"),
        3 => "color: revert-layer".to_owned(),
        4 => "color: var(--undefined, revert)".to_owned(),
        5 => format!("margin-left: {value}px"),
        6 => format!("margin-inline-start: {value}px"),
        7 => format!("margin: {value}px"),
        8 => format!("color: rgb({value}, 0, 0); margin-left: {value}px"),
        9 => "all: unset".to_owned(),
        _ => format!("font-size: {value}px; width: 2em"),
    };
    format!("{selector} {{ {declaration}; }}")
}

/// The style rule of `index` for the matched declarations workload: the shapes of [`rule_source`],
/// with declarations whose computed values read the parent, the document's root or each other.
fn matched_declarations_rule_source(index: usize) -> String {
    let a = class_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let selector = match index % 3 {
        0 => format!(".{a}"),
        1 => format!(".{a} .{b}"),
        _ => format!(".{a} > .{b}"),
    };
    let declaration = match index % 12 {
        0 => format!("font-size: {}px", 8 + 4 * index),
        1 => "width: 2em".to_owned(),
        2 => format!("color: rgb({}, 0, 0)", 16 * index),
        3 => "background-color: currentcolor".to_owned(),
        4 => format!("--width: {}px", 1 + index),
        5 => "height: var(--width, 3px)".to_owned(),
        6 => "width: inherit".to_owned(),
        7 => "padding-top: 1rem".to_owned(),
        8 => "font-size: larger".to_owned(),
        9 => "margin-inline-start: 2em".to_owned(),
        10 => "writing-mode: vertical-rl".to_owned(),
        _ => "display: revert".to_owned(),
    };
    format!("{selector} {{ {declaration}; }}")
}

/// The style rule of `index` for the `:has()` workload: one of twelve shapes of `:has()` over
/// three of the classes, whose arguments go down, to a child, or sideways from the anchor, as a
/// subject or a non-subject compound, nested in `:is()` and `:not()`.
fn has_rule_source(index: usize) -> String {
    let a = class_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let c = class_name((index + 5) % CLASSES);
    let selector = match index % 12 {
        0 => format!(".{a}:has(.{b})"),
        1 => format!(".{a}:has(> .{b}) .{c}"),
        2 => format!(".{a}:has(+ .{b})"),
        3 => format!(".{a}:has(~ .{b}) > .{c}"),
        4 => format!(":has(.{a} .{b}) .{c}"),
        5 => format!(".{a}:has(+ .{b} .{c})"),
        6 => format!(".{c}:not(:has(.{a} + .{b}))"),
        7 => format!(".{a}:has(> .{b}:first-child)"),
        8 => format!(":is(.{a}:has(~ .{b} > .{c}), .{c}:has(.{a})) ~ *"),
        9 => format!(".{a}:has(.{b} ~ .{c}) > *"),
        10 => format!(":has(> :not(.{a})) > .{b}"),
        _ => format!(".{a}:has(+ .{b} + .{c})"),
    };
    format!("{selector} {{ {}; }}", declaration_of(index))
}

/// The style rule of `index` for the nested selector workload: one of eight shapes nesting
/// combinators in `:not()` and `:is()`, as the subject compound or left of one.
fn nested_rule_source(index: usize) -> String {
    let a = class_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let c = class_name((index + 5) % CLASSES);
    let selector = match index % 8 {
        0 => format!(":not(.{a} ~ *)"),
        1 => format!(".{c}:not(.{a} + .{b})"),
        2 => format!(":not(.{a} ~ .{b}) > .{c}"),
        3 => format!(":is(.{a} + .{b}, .{c} ~ *)"),
        4 => format!(".{a} ~ :not(.{b} + *)"),
        5 => format!(":not(.{a} > *, .{b} ~ .{c})"),
        6 => format!(".{c}:is(.{a} .{b} ~ *)"),
        _ => format!(":not(.{a} *) + .{b}"),
    };
    format!("{selector} {{ {}; }}", declaration_of(index))
}

/// The style rule of `index` for the workload of compounds no one fact is carried by every element
/// matching: left of a sibling combinator, as an element arriving or leaving reaches them, and of
/// positions, with and without `of S`.
fn keyless_rule_source(index: usize) -> String {
    let a = class_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let c = class_name((index + 5) % CLASSES);
    let selector = match index % 16 {
        0 => format!(":not(.{a}) + .{b}"),
        1 => format!(":not(.{a}) ~ .{b}"),
        2 => format!(":not(.{a}) ~ :not(.{b})"),
        3 => format!(":is(.{a}, *) + .{b}"),
        4 => format!(":is(.{a}, :not(.{c})) ~ .{b}"),
        5 => format!(":where(.{a}, :first-child) + .{b}"),
        6 => format!(":empty + .{b}"),
        7 => format!(":empty ~ .{b}"),
        8 => format!(":not(.{a}) ~ .{b} .{c}"),
        9 => format!(":not(.{a}):not(.{c}) + .{b}"),
        10 => format!(":first-child + .{b}"),
        11 => format!(".{a}:nth-child(2) ~ .{b}"),
        12 => format!(".{a}:nth-child(2 of .{b})"),
        13 => format!(":nth-last-child(1 of .{b}) .{c}"),
        14 => format!(".{a}:only-child"),
        _ => format!(".{a}:first-of-type"),
    };
    format!("{selector} {{ {}; }}", declaration_of(index))
}

/// The style rule of `index` for the attribute workload: compounds testing an attribute of the name
/// `attribute_name` gives, as the subject and left of each combinator, in `:has()` and in `of S`.
fn attribute_rule_source(index: usize) -> String {
    let a = attribute_name(index % CLASSES);
    let b = class_name((index + 3) % CLASSES);
    let c = class_name((index + 5) % CLASSES);
    let selector = match index % 12 {
        0 => format!("[{a}]"),
        1 => format!("[{a}] + .{b}"),
        2 => format!(":not([{a}]) ~ .{b}"),
        3 => format!("[{a}] .{b}"),
        4 => format!(".{b}[{a}] > .{c}"),
        5 => format!(":has([{a}]) .{b}"),
        6 => format!(".{b}:has(+ [{a}])"),
        7 => format!(":is([{a}], .{b}) ~ .{c}"),
        8 => format!("[{a}]:nth-child(2) ~ .{b}"),
        9 => format!(":nth-child(1 of [{a}]) .{b}"),
        10 => format!(".{b}:nth-last-child(1 of :not([{a}]))"),
        _ => format!(":where([{a}], :first-child) + .{b}"),
    };
    format!("{selector} {{ {}; }}", declaration_of(index))
}

fn attribute_name(index: usize) -> String {
    format!("data-attribute{index}")
}

struct Workload {
    engine: StyleEngine,
    nodes: Vec<StyleNodeID>,
    root: StyleNodeID,
    containers: Vec<StyleNodeID>,
    children: Vec<Vec<StyleNodeID>>,
    classes: Vec<Vec<usize>>,
    class_atoms: Vec<StyleAtomID>,
    attributes: Vec<Vec<usize>>,
    attribute_atoms: Vec<StyleAtomID>,
    rule_source: fn(usize) -> String,
    /// Each rule's sheet, which holds it alone.
    sheets: Vec<SheetID>,
    rules: Vec<RuleID>,
    rule_conditions: Vec<bool>,
    sheet_conditions: Vec<bool>,
    /// The two layers every third rule is in, in the order the document declares them.
    layers: [CascadeLayerID; 2],
}

impl Workload {
    fn new(seed: u64, rule_source: fn(usize) -> String) -> Self {
        Self::with_shape(seed, rule_source, CONTAINERS, CHILDREN)
    }

    /// A workload of `container_count` containers of `child_count` children each.
    fn with_shape(seed: u64, rule_source: fn(usize) -> String, container_count: usize, child_count: usize) -> Self {
        let mut engine = hosted_engine();
        let nodes = allocate_nodes(&mut engine, 1 + container_count + container_count * child_count);
        let root = nodes[0];
        let containers = nodes[1..1 + container_count].to_vec();
        let children: Vec<Vec<_>> = (0..container_count)
            .map(|container| {
                let first = 1 + container_count + container * child_count;
                nodes[first..first + child_count].to_vec()
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
        engine.set_element_adjustment_facts(root, element_adjustment_fact::IS_DOCUMENT_ELEMENT);

        let class_atoms: Vec<_> = (0..CLASSES)
            .map(|class| atom(&mut engine, &class_name(class)))
            .collect();
        let mut rng = Lcg(seed);
        let mut classes = vec![Vec::new(); nodes.len()];
        for (index, &node) in nodes.iter().enumerate() {
            let tag = atom(&mut engine, tag_name(node));
            engine.record_input(
                InputKey::LocalFeature(node, LocalFeatureKey::TagName),
                InputValue::Feature(FeatureValue::Atom(tag)),
            );
            for _ in 0..rng.under(3) {
                let class = rng.under(CLASSES);
                if classes[index].contains(&class) {
                    continue;
                }
                classes[index].push(class);
                engine.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Class(class_atoms[class])),
                    InputValue::Feature(FeatureValue::Present),
                );
            }
        }

        let attribute_atoms: Vec<_> = (0..CLASSES)
            .map(|name| atom(&mut engine, &attribute_name(name)))
            .collect();
        let attributes = vec![Vec::new(); nodes.len()];

        let layers = [0, 1].map(|layer| CascadeLayerID(atom(&mut engine, &format!("layer{layer}")).0));
        engine.set_layer_order(TreeScopeID::DOCUMENT, &layers);
        let mut sheets = Vec::new();
        let mut rules = Vec::new();
        for index in 0..RULES {
            let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
            sheets.push(sheet);
            rules.extend(add_rules(&mut engine, sheet, &rule_source(index)));
            if index % 3 == 0 {
                engine.set_rule_layer(rules[index], layers[index % 2]);
            }
        }

        Self {
            engine,
            nodes,
            root,
            containers,
            children,
            classes,
            class_atoms,
            attributes,
            attribute_atoms,
            rule_source,
            sheets,
            rules,
            rule_conditions: vec![true; RULES],
            sheet_conditions: vec![true; RULES],
            layers,
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

    fn mutate(&mut self, rng: &mut Lcg) -> String {
        match rng.under(16) {
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
                    InputKey::LocalFeature(node, LocalFeatureKey::Class(self.class_atoms[class])),
                    InputValue::Feature(new),
                );
                format!("toggle {node:?} class{class}: {old:?} -> {new:?}")
            }
            6..=7 => {
                let from = rng.under(self.containers.len());
                if self.children[from].is_empty() {
                    return format!("skip empty container {from}");
                }
                let from_index = rng.under(self.children[from].len());
                let node = self.children[from][from_index];
                let old = self.node_relations(node);
                self.children[from].remove(from_index);
                let to = rng.under(self.containers.len());
                let to_index = rng.under(self.children[to].len() + 1);
                self.children[to].insert(to_index, node);
                let new = self.node_relations(node);
                if old == new {
                    return format!("skip no-op move of {node:?}");
                }
                self.engine.record_tree_delta(node, Some(old), Some(new));
                format!("move {node:?}: {old:?} -> {new:?}")
            }
            10 => {
                // A sheet moves before another, which changes the winner where their rules meet.
                let (index, before) = (rng.under(RULES), rng.under(RULES));
                if index == before {
                    return format!("skip moving {:?} before itself", self.sheets[index]);
                }
                self.engine.detach_sheet(self.sheets[index], TreeScopeID::DOCUMENT);
                self.engine.attach_sheet_before_sheet(
                    self.sheets[index],
                    Some(self.sheets[before]),
                    TreeScopeID::DOCUMENT,
                );
                format!("move {:?} before {:?}", self.sheets[index], self.sheets[before])
            }
            11 => {
                // A rule is replaced by another of some shape and declaration.
                let (index, source) = (rng.under(RULES), rng.under(2 * RULES));
                self.engine.remove_style_rule(self.rules[index]);
                let source = (self.rule_source)(source);
                self.rules[index] = add_rules(&mut self.engine, self.sheets[index], &source)[0];
                self.rule_conditions[index] = true;
                format!("replace rule {index} with {:?}: {source}", self.rules[index])
            }
            12 => {
                let index = rng.under(RULES);
                self.sheet_conditions[index] = !self.sheet_conditions[index];
                self.engine
                    .set_sheet_conditions_hold(self.sheets[index], self.sheet_conditions[index]);
                format!(
                    "set {:?} conditions_hold={}",
                    self.sheets[index], self.sheet_conditions[index]
                )
            }
            13 => {
                self.layers.reverse();
                self.engine.set_layer_order(TreeScopeID::DOCUMENT, &self.layers);
                format!("declare the layers in the order {:?}", self.layers)
            }
            14..=15 => {
                let index = 1 + rng.under(self.nodes.len() - 1);
                let node = self.nodes[index];
                let attribute = rng.under(CLASSES);
                let key = LocalFeatureKey::Attribute(self.attribute_atoms[attribute]);
                if self.attributes[index].contains(&attribute) {
                    self.attributes[index].retain(|&existing| existing != attribute);
                    remove_feature(&mut self.engine, node, key);
                    format!("remove {node:?} attribute {attribute}")
                } else {
                    self.attributes[index].push(attribute);
                    add_feature(&mut self.engine, node, key);
                    format!("add {node:?} attribute {attribute}")
                }
            }
            _ => {
                let index = rng.under(self.rules.len());
                self.rule_conditions[index] = !self.rule_conditions[index];
                self.engine
                    .set_rule_conditions_hold(self.rules[index], self.rule_conditions[index]);
                format!(
                    "set {:?} conditions_hold={}",
                    self.rules[index], self.rule_conditions[index]
                )
            }
        }
    }
}

/// The batches a seed applied, printed when a check of them fails.
struct History {
    seed: u64,
    lines: Vec<String>,
}

impl Drop for History {
    fn drop(&mut self) {
        if std::thread::panicking() {
            eprintln!(
                "seed={:#018x}\noperation history:\n{}",
                self.seed,
                self.lines.join("\n")
            );
        }
    }
}

fn run_seed(seed_index: u64, rule_source: fn(usize) -> String) {
    let seed = 0x5EED_5400_0000_0000_u64 | seed_index;
    let mut workload = Workload::new(seed, rule_source);
    let mut rng = Lcg(seed ^ 0xD1FF_E2E1_71A1_0001);
    let mut history = History {
        seed,
        lines: vec!["--- step 0".to_owned()],
    };
    start_live(&mut workload.engine, workload.root);
    assert_eq!(
        assert_live_matches_cold(&mut workload.engine, workload.root),
        workload.nodes.len()
    );

    for step in 1..=STEPS {
        history.lines.push(format!("--- step {step}"));
        for _ in 0..3 {
            history.lines.push(workload.mutate(&mut rng));
        }
        run_style_update(&mut workload.engine, workload.root);
        assert_eq!(
            assert_live_matches_cold(&mut workload.engine, workload.root),
            workload.nodes.len()
        );
    }
}

#[test]
fn seeded_transactions_restyle_live_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_rules_that_lose_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, losing_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_what_matched_declarations_read_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, matched_declarations_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_what_matched_declarations_read_alike_on_one_thread_and_on_several() {
    for seed_index in 0..seeds() {
        run_seed_on_one_thread_and_on_several(seed_index, matched_declarations_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_has_anchors_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, has_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_compounds_testing_attributes_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, attribute_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_nested_combinators_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, nested_rule_source);
    }
}

// A document wide enough that each pass on several threads hands most of its elements to the pool.
const WIDE_CONTAINERS: usize = 8;
const WIDE_CHILDREN: usize = 24;
const THREAD_SEEDS: u64 = 4;
const THREAD_STEPS: usize = 16;
const OTHER_THREADS: u32 = 4;

/// Run a seed's batches over two copies of a wide workload, one restyled by the engine on one
/// thread and the other on several, and check after each batch that both hold the same records,
/// and that the one on several threads holds what cold passes compute on one thread and on
/// several, with every cache off and with them on.
fn run_seed_on_one_thread_and_on_several(seed_index: u64, rule_source: fn(usize) -> String) {
    let seed = 0x5EED_7400_0000_0000_u64 | seed_index;
    let mut workloads = [1, OTHER_THREADS].map(|threads| {
        let mut workload = Workload::with_shape(seed, rule_source, WIDE_CONTAINERS, WIDE_CHILDREN);
        start_live_on(&mut workload.engine, workload.root, threads);
        workload
    });
    let mut rngs = [0, 1].map(|_| Lcg(seed ^ 0xD1FF_E2E1_71A1_0001));
    let mut history = History {
        seed,
        lines: vec!["--- step 0".to_owned()],
    };
    for step in 0..=THREAD_STEPS {
        if step != 0 {
            history.lines.push(format!("--- step {step}"));
            for _ in 0..3 {
                let [one, several] = &mut workloads;
                let [one_rng, several_rng] = &mut rngs;
                let line = one.mutate(one_rng);
                assert_eq!(several.mutate(several_rng), line, "both workloads mutate alike");
                history.lines.push(line);
            }
            for workload in &mut workloads {
                run_style_update(&mut workload.engine, workload.root);
            }
        }
        for workload in &mut workloads {
            assert_eq!(
                assert_live_matches_cold(&mut workload.engine, workload.root),
                workload.nodes.len()
            );
        }
        let [one, several] = &workloads;
        let failures = several.engine.self_check_failures(several.root, Some(OTHER_THREADS));
        assert!(
            failures.is_empty(),
            "the engine on {OTHER_THREADS} threads is not what cold passes compute:\n{}",
            failures.join("\n")
        );
        let state = &several.engine.retained;
        fn records(workload: &Workload) -> &super::records::RetainedRecords {
            workload
                .engine
                .retained
                .live_style
                .as_ref()
                .expect("the engine is live")
                .records()
        }
        let (failures, compared) =
            super::self_check::differences(state, several.root, records(several), records(one), &|node| {
                describe(state, node)
            });
        assert_eq!(compared, several.nodes.len());
        assert!(
            failures.is_empty(),
            "the engine on {OTHER_THREADS} threads (live) holds other records than on one (cold):\n{}",
            failures.join("\n")
        );
    }
}

#[test]
fn seeded_transactions_restyle_alike_on_one_thread_and_on_several() {
    for seed_index in 0..THREAD_SEEDS {
        run_seed_on_one_thread_and_on_several(seed_index, rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_has_anchors_alike_on_one_thread_and_on_several() {
    for seed_index in 0..THREAD_SEEDS {
        run_seed_on_one_thread_and_on_several(seed_index, has_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_compounds_with_no_key_as_a_cold_pass_does() {
    for seed_index in 0..seeds() {
        run_seed(seed_index, keyless_rule_source);
    }
}

#[test]
fn seeded_transactions_restyle_nested_combinators_alike_on_one_thread_and_on_several() {
    for seed_index in 0..THREAD_SEEDS {
        run_seed_on_one_thread_and_on_several(seed_index, nested_rule_source);
    }
}

#[test]
fn a_media_flip_keeps_styles_by_what_the_rules_that_flipped_declare_however_many_of_their_sheet_flipped() {
    // NB: The sheet has enough selectors for a segment. Where every rule of it flips the segment is let
    //     go of, and where a few do its entries die in place.
    for flipped_rules in [SEGMENT_MIN_ENTRIES, 2] {
        let (mut engine, nodes) = linear_document();
        let staying = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        // NB: The first rule of the document matches nothing, and declares what the rule that stays does.
        add_rules(
            &mut engine,
            staying,
            ".first-rule { color: rgb(9, 9, 9); }\n.stays.stays { color: rgb(1, 1, 1); }",
        );
        let flipping = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        let mut source =
            String::from(".loses { color: rgb(2, 2, 2); }\n.decides { background-color: rgb(2, 2, 2); }\n");
        for index in 2..SEGMENT_MIN_ENTRIES {
            source += &format!(".filler-{index} {{ width: 1px; }}\n");
        }
        let rules = add_rules(&mut engine, flipping, &source);
        for (node, class) in [(nodes[1], "loses"), (nodes[2], "decides")] {
            add_class(&mut engine, node, "stays");
            add_class(&mut engine, node, class);
        }
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);

        for &rule in &rules[..flipped_rules] {
            engine.set_rule_conditions_hold(rule, false);
        }
        run_style_update(&mut engine, nodes[0]);
        let styled = styled_by_a_targeted_pass(&mut engine, nodes[0]);
        assert!(!styled.contains(&nodes[1]), "with {flipped_rules} rules flipping");
        assert!(styled.contains(&nodes[2]), "with {flipped_rules} rules flipping");
        assert_live_matches_cold(&mut engine, nodes[0]);
    }
}

#[test]
fn a_media_flip_keeps_styles_only_as_far_as_what_decides_them_is_worth_reading() {
    // NB: Each rule that flips is looked for among the rules that stay, from the last down to its winner.
    for (flipped_rules, keeps) in [(4, true), (256, false)] {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        let mut source = String::new();
        for index in 0..flipped_rules {
            source += &format!(".target {{ color: rgb({}, 0, 0); }}\n", index % 256);
        }
        source += ".target { color: rgb(1, 1, 1); }\n";
        for index in 0..flipped_rules {
            source += &format!(".target {{ margin-left: {index}px; }}\n");
        }
        let rules = add_rules(&mut engine, sheet, &source);
        add_class(&mut engine, nodes[1], "target");
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);

        for &rule in &rules[..flipped_rules] {
            engine.set_rule_conditions_hold(rule, false);
        }
        run_style_update(&mut engine, nodes[0]);
        let styled = styled_by_a_targeted_pass(&mut engine, nodes[0]);
        assert_eq!(
            styled.contains(&nodes[1]),
            !keeps,
            "with {flipped_rules} rules flipping"
        );
        assert_live_matches_cold(&mut engine, nodes[0]);
    }
}

#[test]
fn elements_alike_hold_one_match_list_across_the_walks_of_a_rule_index_update() {
    fn every_element_rule(index: usize) -> String {
        format!("* {{ color: rgb({index}, 0, 0); }}")
    }
    let mut workload = Workload::with_shape(0x5EED_7500_0000_0000, every_element_rule, 8, 256);
    start_live_on(&mut workload.engine, workload.root, OTHER_THREADS);

    // NB: The rule loses to the ones after it, so every element takes a new list and keeps its style.
    let flipped = workload.rules[3];
    workload.engine.set_rule_conditions_hold(flipped, false);
    run_style_update(&mut workload.engine, workload.root);
    assert_eq!(
        assert_live_matches_cold(&mut workload.engine, workload.root),
        workload.nodes.len()
    );

    let records = workload
        .engine
        .retained
        .live_style
        .as_ref()
        .expect("the engine is live")
        .records();
    let list = records.matches_of(workload.nodes[0]).expect("a match list is kept");
    assert_eq!(list.len(), RULES - 1);
    for &node in &workload.nodes {
        let held = records.matches_of(node).expect("a match list is kept");
        assert!(std::sync::Arc::ptr_eq(held, list));
    }
}

#[test]
fn a_move_within_a_parent_after_a_matching_sibling_restyles_it_as_a_cold_pass_does() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x ~ .y { width: 10px; }");
    add_class(&mut engine, nodes[1], "y");
    add_class(&mut engine, nodes[2], "x");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0]), None, Some(nodes[2]))),
        Some(relations(Some(nodes[0]), Some(nodes[2]), Some(nodes[3]))),
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_move_within_a_parent_past_a_later_sibling_restyles_it_as_a_cold_pass_does() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x ~ .y { width: 10px; }");
    add_class(&mut engine, nodes[1], "x");
    add_class(&mut engine, nodes[3], "y");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0]), None, Some(nodes[2]))),
        Some(relations(Some(nodes[0]), Some(nodes[3]), None)),
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn matching_flags_the_parents_whose_children_selectors_read_siblings_positions_or_emptiness_of() {
    use super::projection::StructuralFlags;
    let cases = [
        (
            ".x + .y { width: 10px; }",
            StructuralFlags::CHILDREN_READ_PREVIOUS_SIBLING,
        ),
        (".x ~ .y { width: 10px; }", StructuralFlags::CHILDREN_READ_SIBLINGS),
        (
            ".y:nth-child(2n) { width: 10px; }",
            StructuralFlags::CHILDREN_READ_POSITIONS,
        ),
    ];
    for (rule, flag) in cases {
        for with_subject in [false, true] {
            let (mut engine, nodes) = linear_document();
            let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
            add_rules(&mut engine, sheet, rule);
            add_class(&mut engine, nodes[1], "x");
            if with_subject {
                add_class(&mut engine, nodes[3], "y");
            }
            start_live(&mut engine, nodes[0]);
            let flags = engine.retained.projection.structural_flags(nodes[0]);
            assert_eq!(flags.contains(flag), with_subject, "{rule}");

            // An element leaving a parent whose children no selector read the siblings of restyles no sibling,
            // and one leaving where they did restyles those it reached.
            engine.record_tree_delta(
                nodes[2],
                Some(relations(Some(nodes[0]), Some(nodes[1]), Some(nodes[3]))),
                None,
            );
            engine.record_tree_delta(
                nodes[3],
                Some(relations(Some(nodes[0]), Some(nodes[2]), None)),
                Some(relations(Some(nodes[0]), Some(nodes[1]), None)),
            );
            run_style_update(&mut engine, nodes[0]);
            assert_live_matches_cold(&mut engine, nodes[0]);
        }
    }

    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x:empty { width: 10px; }");
    add_class(&mut engine, nodes[1], "x");
    start_live(&mut engine, nodes[0]);
    let projection = &engine.retained.projection;
    assert!(
        projection
            .structural_flags(nodes[1])
            .contains(StructuralFlags::READ_EMPTINESS)
    );
    assert!(
        !projection
            .structural_flags(nodes[2])
            .contains(StructuralFlags::READ_EMPTINESS)
    );
}

/// The elements the last pass styled, where it styled only some.
fn styled_by_a_targeted_pass(engine: &mut StyleEngine, root: StyleNodeID) -> Vec<StyleNodeID> {
    let retained = &mut engine.retained;
    let counters = &mut retained.live_counters;
    assert!(!counters.last_pass_is_full(), "the pass styled only some elements");
    counters.last_pass_elements(&retained.projection, root, false).to_vec()
}

#[test]
fn a_change_left_of_a_sibling_combinator_in_not_restyles_the_siblings_it_reaches() {
    for (selector, reached) in [(":not(.b ~ *)", 2..4), (":not(.b + *)", 2..3)] {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, &format!("{selector} {{ color: green; }}"));
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);

        for change in [add_class, remove_class] {
            change(&mut engine, nodes[1], "b");
            run_style_update(&mut engine, nodes[0]);
            let mut styled = styled_by_a_targeted_pass(&mut engine, nodes[0]);
            styled.sort();
            assert_eq!(styled, nodes[reached.clone()], "{selector}");
            assert_live_matches_cold(&mut engine, nodes[0]);
        }
    }
}

#[test]
fn a_change_in_a_has_argument_restyles_only_the_anchors_whose_answer_flips() {
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".anchor:has(.x) { width: 10px; } .anchor:has(> .x) > * { height: 10px; } .other:has(.y) { width: 5px; }",
    );
    add_class(&mut engine, nodes[1], "anchor");
    add_class(&mut engine, nodes[2], "anchor");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The `.x` arriving on the grandchild of the outer anchor flips both anchors for the first rule,
    // and the inner one, its parent, for the second, which reaches its child.
    add_class(&mut engine, nodes[3], "x");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), &nodes[1..]);

    // A fact no argument tests restyles nothing, and one only another query's argument tests
    // flips no anchor of it.
    add_class(&mut engine, nodes[3], "unrelated");
    add_class(&mut engine, nodes[3], "y");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());

    remove_class(&mut engine, nodes[3], "x");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), &nodes[1..]);
}

#[test]
fn an_element_arriving_or_leaving_restyles_has_anchors_as_a_cold_pass_does() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        concat!(
            ".anchor:has(> .x) { width: 10px; } .anchor:has(+ .x) { height: 10px; } ",
            ".anchor:has(~ .x) { margin-left: 10px; } :has(> :first-child.x) > * { padding-top: 1px; } ",
            ".anchor:has(+ *) { color: rgb(0, 0, 255); }",
        ),
    );
    add_class(&mut engine, nodes[0], "anchor");
    add_class(&mut engine, nodes[1], "anchor");
    add_class(&mut engine, nodes[2], "x");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // `b` leaves from between `a` and `c`, and then a new `.x` arrives first.
    engine.record_tree_delta(
        nodes[2],
        Some(relations(Some(nodes[0]), Some(nodes[1]), Some(nodes[3]))),
        None,
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    let arriving = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(arriving, None, Some(relations(Some(nodes[0]), None, Some(nodes[1]))));
    set_tag(&mut engine, arriving, "div");
    add_class(&mut engine, arriving, "x");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // A plain element arrives after `c`, and the first `.x` leaves.
    let later = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(later, None, Some(relations(Some(nodes[0]), Some(nodes[3]), None)));
    set_tag(&mut engine, later, "div");
    engine.record_tree_delta(arriving, Some(relations(Some(nodes[0]), None, Some(nodes[1]))), None);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn an_element_moving_into_a_has_anchor_takes_its_subtree_along() {
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".anchor:has(.x) { width: 10px; } .anchor:has(.y .x) { height: 10px; }",
    );
    add_class(&mut engine, nodes[1], "anchor");
    add_class(&mut engine, nodes[2], "y");
    add_class(&mut engine, nodes[3], "x");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // `b`, holding the `.x`, moves from under the anchor to beside it.
    engine.record_tree_delta(
        nodes[2],
        Some(relations(Some(nodes[1]), None, None)),
        Some(relations(Some(nodes[0]), Some(nodes[1]), None)),
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    engine.record_tree_delta(
        nodes[2],
        Some(relations(Some(nodes[0]), Some(nodes[1]), None)),
        Some(relations(Some(nodes[1]), None, None)),
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn an_element_arriving_in_an_empty_witness_restyles_its_anchor() {
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".hollow:has(.box:empty) { width: 10px; }");
    add_class(&mut engine, nodes[1], "hollow");
    add_class(&mut engine, nodes[2], "box");
    engine.record_tree_delta(nodes[3], Some(relations(Some(nodes[2]), None, None)), None);
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    let arriving = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(arriving, None, Some(relations(Some(nodes[2]), None, None)));
    set_tag(&mut engine, arriving, "span");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).contains(&nodes[1]));
}

#[test]
fn a_subtree_leaving_with_a_witness_restyles_what_its_anchor_reaches() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".anchor:has(.x) ~ * { width: 10px; } .anchor:has(> .x) + * { height: 10px; }",
    );
    let witness = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(witness, None, Some(relations(Some(nodes[1]), None, None)));
    set_tag(&mut engine, witness, "div");
    add_class(&mut engine, witness, "x");
    add_class(&mut engine, nodes[1], "anchor");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The anchor leaves with its witness, which, as an element below the one leaving, names no
    // parent it left.
    engine.record_tree_delta(nodes[1], Some(relations(Some(nodes[0]), None, Some(nodes[2]))), None);
    engine.record_tree_delta(witness, Some(relations(None, None, None)), None);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_change_to_a_compound_nested_in_a_has_argument_restyles_the_anchors_it_reaches() {
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        concat!(
            ".anchor:has(> :is(.active > *)) { width: 10px; } ",
            ".anchor:has(:is(.outer .y)) { height: 10px; } ",
            ".anchor:has(:is(.p + .q)) { margin-left: 10px; }",
        ),
    );
    add_class(&mut engine, nodes[1], "anchor");
    add_class(&mut engine, nodes[2], "anchor");
    add_class(&mut engine, nodes[3], "y");
    add_class(&mut engine, nodes[3], "q");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The anchor itself, and an element above it, match the compounds nested in the arguments.
    add_class(&mut engine, nodes[2], "active");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    add_class(&mut engine, nodes[0], "outer");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // A `.p` arriving before the `.q` makes the nested selector match it.
    let arriving = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(arriving, None, Some(relations(Some(nodes[2]), None, Some(nodes[3]))));
    set_tag(&mut engine, arriving, "div");
    add_class(&mut engine, arriving, "p");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    remove_class(&mut engine, nodes[2], "active");
    remove_class(&mut engine, nodes[0], "outer");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_dying_rule_entry_leaves_the_match_lists_not_naming_it_alone() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    let rules = add_rules(&mut engine, sheet, ".x { width: 10px; } .y { height: 10px; }");
    add_class(&mut engine, nodes[1], "x");
    add_class(&mut engine, nodes[2], "y");
    start_live(&mut engine, nodes[0]);
    let matches_of = |engine: &StyleEngine, node: StyleNodeID| {
        engine
            .retained
            .live_style
            .as_ref()
            .expect("the engine is live")
            .records()
            .matches_of(node)
            .cloned()
    };
    let kept = matches_of(&engine, nodes[2]).expect("the pass kept the matches");

    // The list of the element the dying entry did not match is the one it held, not a copy.
    engine.remove_style_rule(rules[0]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    let after = matches_of(&engine, nodes[2]).expect("the element keeps its matches");
    assert!(std::sync::Arc::ptr_eq(&kept, &after));
}

#[test]
fn a_rule_change_restyles_only_the_elements_its_rules_match_or_matched() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    let rules = add_rules(&mut engine, sheet, ".x { width: 10px; } .y ~ .z { height: 10px; }");
    add_class(&mut engine, nodes[1], "x");
    add_class(&mut engine, nodes[1], "y");
    add_class(&mut engine, nodes[3], "z");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // A rule arriving restyles the element it matches, and one matching nothing nothing.
    let later = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        later,
        ".z { margin-left: 1px; } .nobody { color: rgb(1, 2, 3); }",
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), [nodes[3]]);

    // A rule leaving restyles the elements it matched, as does a change to what it declares.
    engine.remove_style_rule(rules[0]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), [nodes[1]]);
    engine.set_rule_conditions_hold(rules[1], false);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), [nodes[3]]);

    // An element gaining a class in the same transaction as a rule arriving for it is matched
    // against the rule with it.
    let third = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, third, ".w { padding-top: 2px; }");
    add_class(&mut engine, nodes[2], "w");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), [nodes[2]]);

    // A sheet whose rules match nothing arriving and leaving restyles nothing.
    let empty = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, empty, ".nobody { color: rgb(4, 5, 6); }");
    run_style_update(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());
    engine.detach_sheet(empty, TreeScopeID::DOCUMENT);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());
}

#[test]
fn a_cascade_order_change_restyles_only_the_elements_whose_rules_moved() {
    let (mut engine, nodes) = linear_document();
    let layers = ["base", "theme"].map(|name| CascadeLayerID(atom(&mut engine, name).0));
    engine.set_layer_order(TreeScopeID::DOCUMENT, &layers);
    let first = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    let base = add_rules(&mut engine, first, ".x { width: 1px; } .unmoved { height: 1px; }");
    let second = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    let theme = add_rules(&mut engine, second, ".x { width: 2px; }");
    let third = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, third, ".y { width: 3px; }");
    for (rule, layer) in [(base[0], layers[0]), (base[1], layers[0]), (theme[0], layers[1])] {
        engine.set_rule_layer(rule, layer);
    }
    add_class(&mut engine, nodes[1], "x");
    add_class(&mut engine, nodes[2], "unmoved");
    add_class(&mut engine, nodes[3], "y");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // Reversing the layers moves one of the two rules for `.x` against the other, and the
    // unlayered rule for `.y` against neither.
    engine.set_layer_order(TreeScopeID::DOCUMENT, &[layers[1], layers[0]]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    let styled = styled_by_a_targeted_pass(&mut engine, nodes[0]);
    assert!(styled.contains(&nodes[1]) && !styled.contains(&nodes[3]), "{styled:?}");

    // Moving the last sheet to the front moves no rule against another it meets.
    engine.detach_sheet(third, TreeScopeID::DOCUMENT);
    engine.attach_sheet_before_sheet(third, Some(first), TreeScopeID::DOCUMENT);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    let styled = styled_by_a_targeted_pass(&mut engine, nodes[0]);
    assert!(!styled.contains(&nodes[1]) && !styled.contains(&nodes[2]), "{styled:?}");

    // Moving the theme sheet before the base one in the same layer order changes nothing either,
    // as layers decide before sheets do.
    engine.detach_sheet(second, TreeScopeID::DOCUMENT);
    engine.attach_sheet_before_sheet(second, Some(first), TreeScopeID::DOCUMENT);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());

    // Taking the rules out of their layers makes sheet order decide.
    for rule in [base[0], base[1], theme[0]] {
        engine.set_rule_layer(rule, CascadeLayerID::UNLAYERED);
    }
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn elements_declaring_one_viewport_relative_value_all_depend_on_the_viewport() {
    // root -> [a, b, c], all declaring one viewport-relative width, which each absolutizes, as
    // their colors make their winners differ.
    let (mut engine, nodes) = document_with_children(3);
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".sized { width: 10vw; } .green { color: rgb(0, 128, 0); } .blue { color: rgb(0, 0, 255); }",
    );
    for (&node, class) in nodes[1..].iter().zip(["sized", "green", "blue"]) {
        add_class(&mut engine, node, "sized");
        add_class(&mut engine, node, class);
    }
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn list_items_of_tree_scopes_with_their_own_counter_styles_name_their_own_registry() {
    // root -> [a -> item, b], with b hosting a shadow tree whose style scope has its own registry,
    // holding an item too. Both scopes adopt one sheet, so the items of both have the same winners
    // under alike parents, and so do their markers, but each names the registry of its own scope.
    let (mut engine, nodes) = document_with_children(3);
    let item = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(item, None, Some(relations(Some(nodes[1]), None, None)));
    set_tag(&mut engine, item, "div");
    let scope = TreeScopeID(1);
    let shadow = attach_shadow_tree(&mut engine, nodes[2], scope, 1);
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".item { display: list-item; list-style-type: lower-roman; } .item::marker { color: rgb(0, 0, 255); }",
    );
    engine.attach_sheet_before_sheet(sheet, None, scope);
    add_class(&mut engine, item, "item");
    add_class(&mut engine, shadow[1], "item");
    engine
        .retained
        .set_counter_style_environment_identity(TreeScopeID::DOCUMENT, 1);
    engine.retained.set_counter_style_environment_identity(scope, 2);
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_new_counter_style_registry_restyles_the_elements_naming_the_old_one() {
    // root -> [a -> [item, plain], b], with b hosting a shadow tree whose style scope has its own
    // registry, holding an item too. The items' own styles name a counter style the scope's
    // registry decides; of the plain list items, whose decimal no registry overrides, only the
    // markers do.
    let (mut engine, nodes) = document_with_children(3);
    let children = allocate_nodes(&mut engine, 2);
    engine.record_tree_delta(children[0], None, Some(relations(Some(nodes[1]), None, None)));
    engine.record_tree_delta(
        children[1],
        None,
        Some(relations(Some(nodes[1]), Some(children[0]), None)),
    );
    for &child in &children {
        set_tag(&mut engine, child, "div");
    }
    let scope = TreeScopeID(1);
    let shadow = attach_shadow_tree(&mut engine, nodes[2], scope, 2);
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".item { display: list-item; list-style-type: lower-roman; } .plain { display: list-item; list-style-type: decimal; }",
    );
    engine.attach_sheet_before_sheet(sheet, None, scope);
    add_class(&mut engine, children[0], "item");
    add_class(&mut engine, children[1], "plain");
    add_class(&mut engine, shadow[1], "item");
    add_class(&mut engine, shadow[2], "plain");
    engine
        .retained
        .set_counter_style_environment_identity(TreeScopeID::DOCUMENT, 1);
    engine.retained.set_counter_style_environment_identity(scope, 2);
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The host minting a registry for a scope is no input of the elements reading it.
    engine.retained.set_counter_style_environment_identity(scope, 3);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    engine
        .retained
        .set_counter_style_environment_identity(TreeScopeID::DOCUMENT, 4);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn highlight_pseudo_elements_are_styled_where_the_host_shows_the_highlight() {
    use super::super::publication::pseudo_kind::SELECTION;
    let holds_selection = |engine: &StyleEngine, node: StyleNodeID| {
        let live = engine.retained.live_style.as_ref().expect("the engine is live");
        live.records()
            .pseudo_element_records(node)
            .iter()
            .any(|(target, _)| target.kind == SELECTION)
    };
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".moved { width: 1px; }");
    start_live(&mut engine, nodes[0]);

    // A highlight rule arriving while no element shows the highlight reaches the matches the
    // elements keep, which their highlight pseudo-elements cascade once they show it.
    add_rules(&mut engine, sheet, "*::selection { color: rgb(1, 2, 3); }");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(!holds_selection(&engine, nodes[1]));

    // Only the elements the host shows the highlight in style it, whatever else a pass restyles.
    engine.set_highlight_observing_elements(SELECTION, &[nodes[0], nodes[1]]);
    add_class(&mut engine, nodes[2], "moved");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(holds_selection(&engine, nodes[1]) && !holds_selection(&engine, nodes[2]));

    // A highlight rule changing restyles the highlight pseudo-elements of the elements showing it.
    let rules = add_rules(&mut engine, sheet, "div::selection { color: rgb(4, 5, 6); }");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    engine.remove_style_rule(rules[0]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    engine.set_highlight_observing_elements(SELECTION, &[nodes[0], nodes[2]]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(!holds_selection(&engine, nodes[1]) && holds_selection(&engine, nodes[2]));

    engine.set_highlight_observing_elements(SELECTION, &[]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(nodes.iter().all(|&node| !holds_selection(&engine, node)));
}

#[test]
fn a_shadow_tree_rule_change_restyles_only_what_its_rules_reach() {
    let (mut engine, nodes) = linear_document();
    let scope = TreeScopeID(1);
    let shadow = attach_shadow_tree(&mut engine, nodes[1], scope, 2);
    add_class(&mut engine, nodes[2], "item");
    add_class(&mut engine, shadow[2], "item");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The shadow tree's rules reach its own elements and its host, and no element of the document
    // that would match them there.
    let sheet = add_sheet(&mut engine, scope);
    let rules = add_rules(&mut engine, sheet, ".item { width: 1px; } :host { height: 1px; }");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), [nodes[1], shadow[2]]);

    // NB: A host whose matches lost an entry holds none, and the elements of its shadow tree
    //     standing for its pseudo-elements are restyled with it.
    engine.remove_style_rule(rules[1]);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    let styled = styled_by_a_targeted_pass(&mut engine, nodes[0]);
    assert!(styled.contains(&nodes[1]) && !styled.contains(&nodes[2]), "{styled:?}");

    engine.detach_sheet(sheet, scope);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), [shadow[2]]);
}

#[test]
fn a_keyframes_rule_restyles_no_element() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x { width: 1px; }");
    add_class(&mut engine, nodes[1], "x");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    let name = atom(&mut engine, "slide");
    let keyframes = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    engine.add_keyframes_rule(keyframes, None, name);
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());
}

#[test]
fn sibling_positions_measured_before_a_move_are_measured_again_after_it() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x:nth-child(2n) ~ * { width: 10px; }");
    start_live(&mut engine, nodes[0]);

    // The first child is measured where it stands, first, for each change to its classes.
    for change in [add_class, remove_class] {
        change(&mut engine, nodes[1], "x");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    engine.record_tree_delta(
        nodes[1],
        Some(relations(Some(nodes[0]), None, Some(nodes[2]))),
        Some(relations(Some(nodes[0]), Some(nodes[2]), Some(nodes[3]))),
    );
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // It is second now, which gives its later sibling the width.
    add_class(&mut engine, nodes[1], "x");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn an_input_no_rule_tests_restyles_nothing_and_one_a_rule_tests_restyles() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        "#named { width: 1px; } :has(> .inner) { width: 2px; }",
    );
    start_live(&mut engine, nodes[0]);
    let named = atom(&mut engine, "named");
    let unnamed = atom(&mut engine, "unnamed");
    let styled = |engine: &mut StyleEngine, node| styled_by_a_targeted_pass(engine, nodes[0]).contains(&node);

    set_atom_feature(&mut engine, nodes[1], LocalFeatureKey::Id, unnamed);
    add_class(&mut engine, nodes[2], "unnamed");
    run_style_update(&mut engine, nodes[0]);
    assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());
    assert_live_matches_cold(&mut engine, nodes[0]);

    // An id is tested as the value it had and the value it has.
    engine.record_input(
        InputKey::LocalFeature(nodes[1], LocalFeatureKey::Id),
        InputValue::Feature(FeatureValue::Atom(named)),
    );
    run_style_update(&mut engine, nodes[0]);
    assert!(styled(&mut engine, nodes[1]));
    assert_live_matches_cold(&mut engine, nodes[0]);

    // A class only a `:has()` argument tests restyles the anchor.
    add_class(&mut engine, nodes[2], "inner");
    run_style_update(&mut engine, nodes[0]);
    assert!(styled(&mut engine, nodes[0]));
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn only_a_transaction_that_hints_an_element_runs_a_pass() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x { width: 1px; }");
    start_live(&mut engine, nodes[0]);
    let counter = |engine: &StyleEngine, counter: StyleCounter| engine.retained.live_counters.get(counter as u32);
    let passes = counter(&engine, StyleCounter::Passes);

    for change in [add_class, remove_class] {
        change(&mut engine, nodes[1], "unnamed");
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(counter(&engine, StyleCounter::Passes), passes);
        assert!(styled_by_a_targeted_pass(&mut engine, nodes[0]).is_empty());
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    run_style_update(&mut engine, nodes[0]);
    assert_eq!(counter(&engine, StyleCounter::Passes), passes);

    add_class(&mut engine, nodes[1], "x");
    run_style_update(&mut engine, nodes[0]);
    assert_eq!(counter(&engine, StyleCounter::Passes), passes + 1);
    assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), &nodes[1..2]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // A rule no element matches is indexed alone, without building the rule index again, and
    // restyles nothing.
    let index_builds = counter(&engine, StyleCounter::IndexBuilds);
    let indexed_entries = counter(&engine, StyleCounter::IndexedEntries);
    add_rules(&mut engine, sheet, ".y { width: 2px; }");
    run_style_update(&mut engine, nodes[0]);
    assert_eq!(counter(&engine, StyleCounter::Passes), passes + 1);
    assert_eq!(counter(&engine, StyleCounter::IndexBuilds), index_builds);
    assert_eq!(counter(&engine, StyleCounter::IndexedEntries), indexed_entries + 1);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_user_agent_sheet_arriving_shares_another_user_agent_index_without_building_the_rule_index() {
    let (mut engine, nodes) = linear_document();
    add_user_agent_sheet(&mut engine, "div { width: 1px; } .x { height: 1px; }");
    let author = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, author, ".y { width: 2px; }");
    add_class(&mut engine, nodes[1], "x");
    add_class(&mut engine, nodes[2], "z");
    add_class(&mut engine, nodes[3], "y");
    start_live(&mut engine, nodes[0]);
    let counter = |engine: &StyleEngine, counter: StyleCounter| engine.retained.live_counters.get(counter as u32);
    let index_builds = counter(&engine, StyleCounter::IndexBuilds);
    let user_agent_index_builds = counter(&engine, StyleCounter::UserAgentIndexBuilds);

    // A user agent sheet arriving, as the first SVG element of a document brings one, has the
    // document decide with the user agent index of all its user agent sheets, whose rules the
    // first one has keep their numbers: only the element the arriving rule matches is restyled.
    add_user_agent_sheet(&mut engine, ".z { height: 3px; }");
    run_style_update(&mut engine, nodes[0]);
    assert_eq!(counter(&engine, StyleCounter::IndexBuilds), index_builds);
    assert_eq!(
        counter(&engine, StyleCounter::UserAgentIndexBuilds),
        user_agent_index_builds + 1
    );
    assert_live_matches_cold(&mut engine, nodes[0]);

    // An author rule arriving after it is numbered where the document's own entries are.
    add_rules(&mut engine, author, ".x { width: 4px; }");
    run_style_update(&mut engine, nodes[0]);
    assert_eq!(counter(&engine, StyleCounter::IndexBuilds), index_builds);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_document_whose_media_leave_a_user_agent_rule_out_decides_without_it() {
    let (mut engine, nodes) = linear_document();
    let (_, rules) = add_user_agent_sheet(&mut engine, ".x { width: 1px; } .y { height: 1px; }");
    add_class(&mut engine, nodes[1], "x");
    add_class(&mut engine, nodes[2], "y");
    start_live(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The rule's conditions stop holding in this document alone, and hold again.
    for holds in [false, true] {
        engine.set_rule_conditions_hold(rules[0], holds);
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(engine.rules.rule_can_decide(rules[0]), holds);
        assert!(engine.rules.rule_can_decide(rules[1]));
        assert_live_matches_cold(&mut engine, nodes[0]);
    }
}

#[test]
fn a_document_finds_the_user_agent_index_the_process_built_ahead() {
    use super::rules::RuleIndex;
    use std::sync::Arc;
    // NB: The process's atoms are the ones the selectors name.
    let process = Arc::new(ProcessStyle::with_atoms(Arc::clone(
        crate::render_state::process_style().atoms(),
    )));
    let source = 1;
    process.publish_user_agent_sheet(source, |engine, sheet| {
        add_rules(engine, sheet, ".x { width: 1px; }");
    });
    process
        .index_user_agent_sheet_ahead(source, &[])
        .expect("the sheet is published")
        .join()
        .expect("the index builds");
    let (mut engine, _) = linear_document();
    engine.share_process_style(Arc::clone(&process));
    let sheet = engine.add_user_agent_sheet(source);
    engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
    engine.commit_staged_program();
    let index = RuleIndex::build(&engine.rules, Some(&process.rule_indexes));
    assert!(index.user_agent_index().is_some());
    assert!(!index.built_user_agent_index);
}

#[test]
fn documents_read_the_user_agent_rules_the_process_keeps() {
    use std::sync::Arc;
    // NB: The process's atoms are the ones the selectors name.
    let process = Arc::new(ProcessStyle::with_atoms(Arc::clone(
        crate::render_state::process_style().atoms(),
    )));
    let mut declarations: Vec<Vec<_>> = Vec::new();
    for _ in 0..2 {
        let (mut engine, nodes) = linear_document();
        engine.share_process_style(Arc::clone(&process));
        let (_, published) = add_user_agent_sheet(
            &mut engine,
            "div { width: 1px; display: block; } .x { height: 1px; display: block; }",
        );
        let author = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, author, ".y { width: 2px; display: block; }");
        add_class(&mut engine, nodes[1], "x");
        add_class(&mut engine, nodes[2], "y");
        start_live(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);

        // The document's own rules are its author sheet's alone.
        assert_eq!(engine.rules.rule_count(), 1);
        assert_eq!(published.len(), 2);
        declarations.push(
            published
                .iter()
                .map(|&rule| engine.rules.written_values_of(rule).as_ptr())
                .collect::<Vec<_>>(),
        );
    }
    // Both documents read the declarations of the same blocks.
    assert_eq!(declarations[0], declarations[1]);
}

#[test]
fn rules_leaving_take_their_selectors_out_of_the_invalidation_map_in_place() {
    use super::invalidation::{InvalidationMap, Trigger};
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    let rules = add_rules(
        &mut engine,
        sheet,
        ".x .y { width: 1px; } .z { height: 1px; } :nth-child(1 of .w) { width: 2px; } \
         .v:has(.u) { width: 3px; } .y ~ .t { width: 4px; } .x { height: 2px; }",
    );
    add_class(&mut engine, nodes[3], "y");
    start_live(&mut engine, nodes[0]);
    let counter = |engine: &StyleEngine, counter: StyleCounter| engine.retained.live_counters.get(counter as u32);
    let index_builds = counter(&engine, StyleCounter::IndexBuilds);

    // Every rule testing `.w`, `.u` or `.t`, and the one testing `.y` below `.x`, leaves.
    for rule in [rules[0], rules[2], rules[3], rules[4]] {
        engine.remove_style_rule(rule);
    }
    run_style_update(&mut engine, nodes[0]);
    assert_eq!(counter(&engine, StyleCounter::IndexBuilds), index_builds);
    assert_live_matches_cold(&mut engine, nodes[0]);

    // The map the rules left is the one built from the rules that stayed.
    let triggers: Vec<Trigger> = ["x", "y", "z", "w", "v", "u", "t"]
        .into_iter()
        .map(|name| Trigger::Class(atom(&mut engine, name)))
        .collect();
    let live = engine
        .retained
        .live_style
        .as_deref()
        .expect("the engine owns the document");
    let map = live.current_invalidation().expect("the map describes the rules");
    let built = InvalidationMap::build(live.index());
    for trigger in &triggers {
        assert_eq!(map.tests(trigger), built.tests(trigger));
    }
    assert!(!map.has_relative_arguments());

    // A change to a class only a rule that left tested restyles nothing.
    let hinted = counter(&engine, StyleCounter::HintedElements);
    for name in ["w", "u", "t"] {
        add_class(&mut engine, nodes[2], name);
        run_style_update(&mut engine, nodes[0]);
    }
    assert_eq!(counter(&engine, StyleCounter::HintedElements), hinted);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn a_pass_styles_nested_restyled_elements_once_from_the_topmost() {
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x { width: 1px; } .y .z { width: 2px; }");
    add_class(&mut engine, nodes[3], "z");
    start_live(&mut engine, nodes[0]);
    let counter = |engine: &StyleEngine, counter: StyleCounter| engine.retained.live_counters.get(counter as u32);

    // Each element restyled below the next, then each above the next: the ones below go with
    // the topmost, and each one is styled once.
    for (change, order) in [
        (add_class as fn(&mut StyleEngine, StyleNodeID, &str), [3, 2, 1]),
        (remove_class, [1, 2, 3]),
    ] {
        for index in order {
            change(&mut engine, nodes[index], "x");
        }
        let styled_before = counter(&engine, StyleCounter::StyledElements);
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(styled_by_a_targeted_pass(&mut engine, nodes[0]), &nodes[1..4]);
        assert_eq!(counter(&engine, StyleCounter::StyledElements), styled_before + 3);
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    // An element restyled below one whose descendants restyle, with one between them that is not.
    add_class(&mut engine, nodes[3], "x");
    add_class(&mut engine, nodes[1], "y");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    remove_class(&mut engine, nodes[1], "y");
    remove_class(&mut engine, nodes[3], "x");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn invalidation_after_a_wide_transaction_measures_positions_anew() {
    // More children than the invalidation keeps room for between transactions.
    let (mut engine, nodes) = document_with_children(1201);
    let (root, children) = (nodes[0], &nodes[1..]);
    for &child in children.iter().skip(1).step_by(2) {
        set_tag(&mut engine, child, "span");
    }
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".x:nth-of-type(3n) { width: 10px; } .x:nth-last-child(2n) { height: 10px; } .y + .x { color: rgb(0, 128, 0); }",
    );
    start_live(&mut engine, root);

    for &child in children {
        add_class(&mut engine, child, "x");
    }
    run_style_update(&mut engine, root);
    assert_live_matches_cold(&mut engine, root);

    // The first child leaves, which moves every later sibling, among all of them and among
    // those of its type.
    engine.record_tree_delta(children[0], Some(relations(Some(root), None, Some(children[1]))), None);
    run_style_update(&mut engine, root);
    assert_live_matches_cold(&mut engine, root);

    // A `span` arrives between two of them, and one of them gains a class.
    let arriving = allocate_nodes(&mut engine, 1)[0];
    engine.record_tree_delta(
        arriving,
        None,
        Some(relations(Some(root), Some(children[4]), Some(children[5]))),
    );
    set_tag(&mut engine, arriving, "span");
    add_class(&mut engine, children[7], "y");
    run_style_update(&mut engine, root);
    assert_live_matches_cold(&mut engine, root);

    for &child in &children[1..] {
        remove_class(&mut engine, child, "x");
    }
    run_style_update(&mut engine, root);
    assert_live_matches_cold(&mut engine, root);
}

#[test]
fn a_pass_restyling_what_waited_reads_containers_an_earlier_pass_computed() {
    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".c { container-type: inline-size; } .w { width: calc(10cqw + random(1px, 2px)); }",
    );
    start_live(&mut engine, nodes[0]);

    // The element becomes a container in the pass that finds its child waiting on a random base
    // value, and the pass after it restyles only the child, before the host installs either.
    add_class(&mut engine, nodes[1], "c");
    add_class(&mut engine, nodes[2], "w");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
}

#[test]
fn an_inherited_text_fill_color_reaches_an_element_with_a_color_of_its_own() {
    use crate::css::computed_value_views::ComputedValuesView;

    let (mut engine, nodes) = nested_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".parent { color: rgb(16, 30, 30); } .on .parent { -webkit-text-fill-color: transparent; } .own { color: rgb(111, 119, 134); }",
    );
    add_class(&mut engine, nodes[1], "parent");
    add_class(&mut engine, nodes[2], "own");
    start_live(&mut engine, nodes[0]);
    add_class(&mut engine, nodes[0], "on");
    run_style_update(&mut engine, nodes[0]);
    assert_live_matches_cold(&mut engine, nodes[0]);
    for &node in &nodes[1..] {
        let record = super::test_support::live_record(&engine, node);
        let text = ComputedValuesView::of(&record.payloads).inherited_text();
        assert_eq!(text.webkit_text_fill_color, 0);
        assert!(!text.webkit_text_fill_color_is_current_color);
    }
}
