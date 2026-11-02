/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl RetainedState {
    /// Where the cascade of `node` weighs a match from `scope`. Each tree the element is in or slotted into stands at its depth, one deeper than the
    /// last, so only the element's own shadow tree, after all of those, stands elsewhere.
    pub(super) fn encapsulation_context(&self, node: StyleNodeID, scope: TreeScopeID) -> u32 {
        if self.own_shadow_tree(node) != Some(scope) {
            return self.tree_scope_depth(scope);
        }
        let slotted_into = self.scopes_slotted_into(node).count() as u32;
        self.tree_scope_depth(self.projection.tree_scope(node)) + slotted_into + 1
    }

    /// The tree the element's shadow root roots, where its `:host` rules decide from.
    fn own_shadow_tree(&self, node: StyleNodeID) -> Option<TreeScopeID> {
        self.projection
            .shadow_root_of(node)
            .and_then(|shadow_root| self.scope_by_root.get(shadow_root))
    }

    /// Every tree whose `::slotted()` can name this element.
    ///
    /// A slot is itself a slottable, so an element can be assigned to a slot that is assigned to
    /// another: what stands in the flat tree at the end of that chain is the element, not the slots
    /// it passed through, and each tree along the way is one whose `::slotted()` names it.
    pub(super) fn scopes_slotted_into(&self, node: StyleNodeID) -> impl Iterator<Item = TreeScopeID> {
        // A slot standing in a shadow tree is not a flattened slottable of the tree it is assigned
        // into: "find flattened slottables" recurses into its own slottables rather than appending
        // it. So no scope names it through `::slotted()`, even though its assignment is what carries
        // the chain onward for the nodes below it.
        let is_reslotted_slot = self.projection.written_facts(node).is_some_and(|facts| facts.is_slot())
            && self.projection.tree_scope(node) != TreeScopeID::DOCUMENT;
        // A chain is at most as deep as the trees are nested, and a cycle is impossible - a slot
        // cannot be assigned to itself - but a bound keeps a corrupted column from spinning.
        const MAX_REASSIGNMENTS: usize = 32;
        let mut current = node;
        let mut remaining = MAX_REASSIGNMENTS;
        std::iter::from_fn(move || {
            if remaining == 0 || is_reslotted_slot {
                return None;
            }
            remaining -= 1;
            let slot = self.projection.assigned_slot_of(current)?;
            current = slot;
            Some(self.projection.tree_scope(slot))
        })
    }

    /// How deeply a tree scope is encapsulated: the document is zero, a shadow tree inside it one,
    /// and one inside that two. It is where an element's cascade weighs a tree it is in or slotted
    /// into (`encapsulation_context`), outermost first for a normal declaration and innermost first
    /// for an important one.
    #[must_use]
    pub(super) fn tree_scope_depth(&self, tree_scope: TreeScopeID) -> u32 {
        let mut depth = 0;
        let mut scope = tree_scope;
        while scope != TreeScopeID::DOCUMENT {
            let Some(root) = self.scope_root(scope) else {
                break;
            };
            let Some(host) = self.projection.host_of(root) else {
                break;
            };
            depth += 1;
            let next = self.projection.tree_scope(host);
            if next == scope {
                break;
            }
            scope = next;
        }
        depth
    }

    pub(super) fn element_cascade_priority(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        important: bool,
    ) -> CascadePriority {
        let tree_scope = self.projection.tree_scope(node);
        let (origin, element_attachment) = match kind {
            ElementDeclarationKind::InlineStyle => (CascadeOrigin::Author, ElementAttachment::InlineStyle),
            ElementDeclarationKind::PresentationalHint | ElementDeclarationKind::SvgPresentationAttribute => (
                CascadeOrigin::AuthorPresentationalHint,
                ElementAttachment::PresentationalHint,
            ),
        };
        CascadePriority::new(PriorityInputs {
            origin,
            important,
            context_depth: self.tree_scope_depth(tree_scope),
            element_attachment,
            layer_rank: self.rules.layer_rank(tree_scope, CascadeLayerID::UNLAYERED),
            specificity: Specificity::default(),
            scope_proximity: u32::MAX,
            sheet_rank: u64::MAX,
            rule_rank: u64::MAX,
        })
    }
}

impl StyleEngine {
    /// Normalize the pending inputs without advancing the committed snapshot.
    pub(super) fn drain_transaction(&mut self) -> StyleUpdate {
        // A transaction taken other than to flush a geometry read's seal takes everything written.
        self.retained.projection.drop_seal();
        self.finalize_staged_sheet_rule_replacements();
        let transaction = self.host.element_inputs.take_transaction();
        self.retained.counters.add(
            Counter::NormalizedUniqueKeys,
            (transaction.declarations.len() + transaction.style_inputs.len()) as u64,
        );
        transaction
    }

    /// Settle inputs which cannot be planned while the document has no style root. Exact element
    /// style reactions are edge-triggered, so preserve them for the first transaction with a root.
    pub(crate) fn flush_without_document_root(&mut self) {
        let transaction = self.take_transaction();
        for &(node, input) in &transaction.style_inputs {
            self.defer_element_style_input(node, input.reaction, input.inherited_style_groups);
        }
        // Held back rather than owed: without a root there is no transaction to take them.
        self.host.deferred_element_style_inputs_are_pending = false;
        self.release_transaction(transaction);
    }

    /// Merge one element style input into the deferred inputs, which are kept sorted by key.
    pub(crate) fn defer_element_style_input(&mut self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) {
        self.host.deferred_element_style_inputs_moved = true;
        let pending = self
            .host
            .deferred_element_style_inputs
            .entry(node)
            .or_insert(DeferredElementStyleInput {
                reaction: 0,
                inherited_style_groups: 0,
            });
        pending.reaction |= reaction;
        pending.inherited_style_groups |= inherited_style_groups;
    }

    /// Release a drained transaction's scratch charge.
    pub fn release_transaction(&mut self, transaction: StyleUpdate) {
        transaction.release();
        self.forget_departed_elements();
        // NB: Nothing is written between the barrier and here, so this commits only the
        //     departed elements' facts leaving.
        self.retained.projection.release();

        self.host.program_staging.clear();
        // NB: With nothing staged, the committed versions are every version naming a sheet's
        //     selectors. The store is asked first, as writing to it copies what a fork shares.
        if self.retained.rules.wants_letting_go_of_selectors() {
            self.retained.rules.let_go_of_unnamed_selectors();
        }
    }

    pub(super) fn collect_live_style_atoms(&self) -> (HashSet<StyleAtomID>, u64) {
        assert!(
            !self.retained.projection.has_written_links(),
            "style atom sweeping requires a settled tree"
        );
        let mut atoms = HashSet::default();
        let mut visited = self.retained.tree.collect_atoms(&mut atoms);
        visited += self.retained.projection.collect_atoms(&mut atoms);
        visited += self.retained.element_declarations.collect_atoms(&mut atoms);
        visited += self.retained.rules.collect_atoms(&mut atoms);
        if !self.retained.html_element_namespace.is_none() {
            atoms.insert(self.retained.html_element_namespace);
        }
        (atoms, visited)
    }

    pub(super) fn sweep_style_atoms(&mut self) {
        if !self.retained.atoms.should_sweep() || self.host.defers_atom_sweep {
            return;
        }
        let (mut live, visited) = self.collect_live_style_atoms();
        self.retained.atoms.mark_sweep_dependencies(&mut live);
        loop {
            let previous_live_count = live.len();
            self.retained.projection.extend_live_attribute_name_forms(&mut live);
            self.retained.atoms.mark_sweep_dependencies(&mut live);
            if live.len() == previous_live_count {
                break;
            }
        }
        let reclaimable = self.retained.atoms.reclaimable_for_sweep(&live);
        // NB: The host forgets what its catalogs spell of the reclaimed atoms as it learns of them.
        self.retained.custom_property_environments.forget_names(&reclaimable);
        let is_reclaimed = |atom: &StyleAtomID| reclaimable.binary_search(atom).is_ok();
        if self.retained.tested_attribute_names.iter().any(is_reclaimed) {
            Arc::make_mut(&mut self.retained.tested_attribute_names).retain(|atom| !is_reclaimed(atom));
        }
        let relational_arrival_key_is_reclaimed =
            |key: &DispatchKey| key.atom().is_some_and(|atom| is_reclaimed(&atom));
        if self
            .retained
            .relational_arrival_keys
            .keys
            .iter()
            .any(relational_arrival_key_is_reclaimed)
        {
            let mut keys = self.retained.relational_arrival_keys.keys.clone();
            keys.extend(self.retained.unresolved_relational_arrival_keys.drain());
            keys.retain(|key| !relational_arrival_key_is_reclaimed(key));
            self.retained.replace_relational_arrival_keys(keys);
        }
        if self.retained.attribute_value_text_names.iter().any(is_reclaimed) {
            Arc::make_mut(&mut self.retained.attribute_value_text_names).retain(|atom| !is_reclaimed(atom));
            self.retained.attribute_value_text_requirements_version += 1;
        }
        let reclaimed = self.retained.atoms.finish_sweep(&reclaimable);
        self.retained.counters.bump(Counter::AtomSweeps);
        self.retained.counters.add(Counter::AtomSweepRootSlotsVisited, visited);
        self.retained.counters.add(
            Counter::StyleAtomsReclaimed,
            u64::try_from(reclaimed.len()).expect("reclaimed atom count exceeds u64"),
        );
        self.host.reclaimed_style_atoms.extend(reclaimed);
    }

    /// Drop the fact rows of the elements that left, now that nothing can still route from them.
    ///
    /// A retired identity is not reused until the epoch that could name it has retired, so a row
    /// left behind here would be read as the next occupant's. Dropping it at the transaction
    /// boundary keeps the row alive for exactly as long as routing needs it.
    pub(super) fn forget_departed_elements(&mut self) {
        let projection = &self.retained.projection;
        let departed: Vec<StyleNodeID> = projection
            .relations_written()
            .filter(|&node| projection.written_relations(node).is_none())
            .collect();
        if departed.is_empty() {
            return;
        }
        for node in departed {
            // NB: The host forgot the element's row as it published its departure.
            self.retained.element_declarations.forget(node);
            if let Some(tree_scope) = self.retained.scope_by_root.remove(node) {
                self.retained.scope_roots[tree_scope.0 as usize] = None;
            }
        }
    }
}

impl StyleEngine {
    /// Advance every staged input family to the transaction's final snapshot.
    pub(super) fn apply_staged_transaction(&mut self, transaction: &mut StyleUpdate) {
        transaction.rule_changes = self.commit_staged_program();
        self.settle_tree_at_barrier();
        self.retained.projection.commit();
    }

    /// Normalize and apply the staged inputs into one transaction. A required style observation
    /// drains here first, so normalization never combines changes across an observation boundary.
    pub fn take_transaction(&mut self) -> StyleUpdate {
        let mut transaction = self.drain_transaction();
        self.apply_staged_transaction(&mut transaction);
        transaction
    }

    /// Release a transaction taken through the bridge and reclaim atoms before the bridge installs
    /// a new primary view. The returned reclamation batch lets C++ purge its atom memos before any
    /// reclaimed identity can be reused.
    pub(super) fn release_transaction_and_sweep_atoms(&mut self, transaction: StyleUpdate) {
        self.release_transaction(transaction);
        self.sweep_style_atoms();
        self.sweep_specified_values();
    }

    /// Drop the canonical values no element declares any more, once the set doubled since the
    /// last sweep: a script rewriting an inline style enters a value for each write, which nothing
    /// else would let go of.
    pub(super) fn sweep_specified_values(&mut self) {
        if !self.retained.canonical_values.should_sweep() || self.host.defers_atom_sweep {
            return;
        }
        self.retained.canonical_values.sweep();
    }
}
