/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a style installation works in, kept between installations so that one installing a few
//! elements allocates nothing for its bookkeeping.

use super::super::pointer_key::PointerKey;
use super::super::*;
use super::live::{KEPT_SCRATCH_CAPACITY, recycle};
use super::projection::Projection;

/// The environment minted for a store over an inherited environment.
pub(super) type MintedEnvironments =
    HashMap<PointerKey<std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>, u64>, u64>;

/// The element is one the installation installs.
const LISTED: u32 = 1 << 0;
/// The element is above one the installation installs.
const HOLDING: u32 = 1 << 1;
/// Whether the element, or an ancestor of it, was display:none before the installation is known.
const HIDDEN_BEFORE_KNOWN: u32 = 1 << 2;
/// The element, or an ancestor of it, was display:none before the installation.
const HIDDEN_BEFORE: u32 = 1 << 3;
const MARK_BITS: u32 = 4;
const LAST_GENERATION: u32 = u32::MAX >> MARK_BITS;

#[derive(Default)]
pub(super) struct InstallScratch {
    /// By element slot, the generation of the installation that last marked the element, above
    /// the marks it gave it. Marks of an earlier generation are no marks.
    marks: Vec<u32>,
    generation: u32,
    stack: Vec<StyleNodeID>,
    /// The elements the installation installs, in flat tree order.
    pub(super) ordered: Vec<StyleNodeID>,
    /// The ancestors of an element whose answer to whether it was hidden before is not known yet.
    pub(super) chain: Vec<StyleNodeID>,
    pub(super) document_damage: Vec<u32>,
    pub(super) minted: MintedEnvironments,
    pub(super) installed: HashMap<PointerKey<std::sync::Arc<super::Record>>, computed::FinalStyleRecordID>,
    /// The elements the installation let go of the animation overlay of, with its view.
    pub(super) released_compositions: Vec<(StyleNodeID, super::live::HeldComposition)>,
    /// The elements whose records hold what a record installed before them does, which they keep
    /// in place of their own, by the address of their own.
    pub(super) adopted: Vec<(StyleNodeID, usize, std::sync::Arc<super::Record>)>,
}

impl InstallScratch {
    /// The bytes the scratch keeps on the heap between installations.
    pub(super) fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        vec_bytes(&self.marks)
            + vec_bytes(&self.stack)
            + vec_bytes(&self.ordered)
            + vec_bytes(&self.chain)
            + vec_bytes(&self.document_damage)
            + map_bytes(&self.minted)
            + map_bytes(&self.installed)
            + vec_bytes(&self.released_compositions)
            + vec_bytes(&self.adopted)
    }

    /// Starts an installation in `tree`, with no element marked.
    pub(super) fn start(&mut self, tree: &Projection) {
        let slot_count = tree.element_slot_count();
        if self.marks.len() < slot_count {
            self.marks.resize(slot_count, 0);
        }
        self.generation += 1;
        if self.generation > LAST_GENERATION {
            self.marks.fill(0);
            self.generation = 1;
        }
    }

    /// Gives `node` `mark`, answering whether it had it not.
    fn mark(&mut self, node: StyleNodeID, mark: u32) -> bool {
        let Some(marks) = self.marks.get_mut(node.element_slot()) else {
            return false;
        };
        if *marks >> MARK_BITS != self.generation {
            *marks = self.generation << MARK_BITS;
        }
        let had = *marks & mark != 0;
        *marks |= mark;
        !had
    }

    fn marks_of(&self, node: StyleNodeID) -> u32 {
        match self.marks.get(node.element_slot()) {
            Some(&marks) if marks >> MARK_BITS == self.generation => marks,
            _ => 0,
        }
    }

    /// Orders the live elements of `nodes` in flat tree order into `ordered`, each after the
    /// parent it inherits from, visiting only the subtrees that hold one.
    pub(super) fn order(&mut self, tree: &Projection, root: StyleNodeID, nodes: impl IntoIterator<Item = StyleNodeID>) {
        self.ordered.clear();
        let mut any = false;
        for node in nodes {
            if !tree.is_live(node) || !self.mark(node, LISTED) {
                continue;
            }
            any = true;
            let mut ancestor = tree.inheritance_parent(node);
            while let Some(current) = ancestor
                && self.mark(current, HOLDING)
            {
                ancestor = tree.inheritance_parent(current);
            }
        }
        if !any {
            return;
        }
        self.stack.clear();
        self.stack.push(root);
        while let Some(node) = self.stack.pop() {
            let marks = self.marks_of(node);
            if marks & LISTED != 0 {
                self.ordered.push(node);
            }
            if marks & HOLDING != 0 {
                let first = self.stack.len();
                self.stack.extend(tree.style_children(node));
                self.stack[first..].reverse();
            }
        }
    }

    /// Whether `node`, or an ancestor of it, was display:none before the installation, where
    /// known.
    pub(super) fn hidden_before(&self, node: StyleNodeID) -> Option<bool> {
        let marks = self.marks_of(node);
        (marks & HIDDEN_BEFORE_KNOWN != 0).then_some(marks & HIDDEN_BEFORE != 0)
    }

    pub(super) fn note_hidden_before(&mut self, node: StyleNodeID, hidden: bool) {
        self.mark(node, HIDDEN_BEFORE_KNOWN);
        if let Some(marks) = self.marks.get_mut(node.element_slot()) {
            *marks = match hidden {
                true => *marks | HIDDEN_BEFORE,
                false => *marks & !HIDDEN_BEFORE,
            };
        }
    }

    /// Empties the scratch for the next installation, keeping its room up to
    /// `KEPT_SCRATCH_CAPACITY` entries in each collection.
    pub(super) fn recycle(&mut self) {
        recycle(&mut self.stack);
        recycle(&mut self.ordered);
        recycle(&mut self.chain);
        recycle(&mut self.document_damage);
        recycle(&mut self.adopted);
        self.minted.clear();
        if self.minted.capacity() > KEPT_SCRATCH_CAPACITY {
            self.minted.shrink_to(KEPT_SCRATCH_CAPACITY);
        }
        self.installed.clear();
        if self.installed.capacity() > KEPT_SCRATCH_CAPACITY {
            self.installed.shrink_to(KEPT_SCRATCH_CAPACITY);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::test_support::*;
    use super::*;

    /// Builds `root -> [first, a, b, c]`, where `first` arrived after the others, so that its
    /// identity sorts last, with a rule that styles the children of `.x`, owned by the engine.
    fn document_with_a_late_first_child() -> (StyleEngine, Vec<StyleNodeID>) {
        let (mut engine, mut nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".x > div { color: rgb(0, 128, 0); }");
        start_live(&mut engine, nodes[0]);
        let first = allocate_nodes(&mut engine, 1)[0];
        engine.record_tree_delta(first, None, Some(relations(Some(nodes[0]), None, Some(nodes[1]))));
        set_tag(&mut engine, first, "div");
        run_style_update(&mut engine, nodes[0]);
        engine.take_installation();
        nodes.insert(1, first);
        (engine, nodes)
    }

    #[test]
    fn an_element_whose_record_holds_what_an_installed_one_does_keeps_that_one() {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".a { color: rgb(0, 128, 0); } .b { color: rgb(0, 128, 0); }",
        );
        start_live(&mut engine, nodes[0]);
        add_class(&mut engine, nodes[1], "a");
        add_class(&mut engine, nodes[2], "b");
        run_style_update(&mut engine, nodes[0]);
        assert!(std::sync::Arc::ptr_eq(
            &live_record(&engine, nodes[1]),
            &live_record(&engine, nodes[2])
        ));
    }

    #[test]
    fn orders_each_live_element_once_in_tree_order() {
        let (engine, nodes) = document_with_a_late_first_child();
        let tree = &engine.retained.projection;
        let mut scratch = InstallScratch::default();
        scratch.start(tree);
        scratch.order(tree, nodes[0], [nodes[4], nodes[2], nodes[1], nodes[4], nodes[2]]);
        assert_eq!(scratch.ordered, [nodes[1], nodes[2], nodes[4]]);

        // What one installation marked is not marked in the next.
        scratch.note_hidden_before(nodes[3], true);
        assert_eq!(scratch.hidden_before(nodes[3]), Some(true));
        scratch.recycle();
        scratch.start(tree);
        assert_eq!(scratch.hidden_before(nodes[3]), None);
        scratch.order(tree, nodes[0], [nodes[3]]);
        assert_eq!(scratch.ordered, [nodes[3]]);

        // Nor once the generations wrapped around.
        scratch.note_hidden_before(nodes[3], false);
        scratch.generation = LAST_GENERATION;
        scratch.start(tree);
        assert_eq!(scratch.hidden_before(nodes[3]), None);
        scratch.order(tree, nodes[0], [nodes[0], nodes[1]]);
        assert_eq!(scratch.ordered, [nodes[0], nodes[1]]);
    }

    #[test]
    fn finds_the_effects_of_each_element_in_tree_order_or_not() {
        let (mut engine, nodes) = document_with_a_late_first_child();
        let children = &nodes[1..];
        for class_present in [true, false] {
            match class_present {
                true => add_class(&mut engine, nodes[0], "x"),
                false => remove_class(&mut engine, nodes[0], "x"),
            }
            run_style_update(&mut engine, nodes[0]);
            let installation = engine.take_installation();
            let effects: Vec<_> = installation.effects.iter().map(|effects| effects.node()).collect();
            assert_eq!(effects, children, "effects are in tree order");
            for &child in children {
                assert_eq!(
                    installation.effects_of(child).map(|effects| effects.node()),
                    Some(child)
                );
            }
            assert!(installation.effects_of(nodes[0]).is_none());
        }

        // Where the elements stand in the order of their identities, the effects are found as well.
        add_class(&mut engine, nodes[2], "x");
        let grandchildren = allocate_nodes(&mut engine, 2);
        engine.record_tree_delta(grandchildren[0], None, Some(relations(Some(nodes[2]), None, None)));
        engine.record_tree_delta(
            grandchildren[1],
            None,
            Some(relations(Some(nodes[2]), Some(grandchildren[0]), None)),
        );
        for &grandchild in &grandchildren {
            set_tag(&mut engine, grandchild, "div");
        }
        run_style_update(&mut engine, nodes[0]);
        engine.take_installation();
        remove_class(&mut engine, nodes[2], "x");
        run_style_update(&mut engine, nodes[0]);
        let installation = engine.take_installation();
        let effects: Vec<_> = installation.effects.iter().map(|effects| effects.node()).collect();
        assert_eq!(effects, grandchildren);
        for &grandchild in &grandchildren {
            assert_eq!(
                installation.effects_of(grandchild).map(|effects| effects.node()),
                Some(grandchild)
            );
        }
    }
}
