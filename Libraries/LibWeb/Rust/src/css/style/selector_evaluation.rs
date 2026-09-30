/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How MatchEvaluator interprets a compiled selector program.

use smallvec::SmallVec;

use super::index::StyleAtomID;
use super::index::StyleNodeFacts;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use super::partial_view::Lookup;
use super::relative_selector::RelationalWitnessKey;
use super::relative_selector::RelativeAxis;
use super::relative_selector::WitnessEffect;
use super::relative_selector::candidate_witnesses;
use super::relative_selector::traversal_anchor;
use super::selector::AttributeCase;
use super::selector::AttributeOperator;
use super::selector::AttributeTest;
use super::selector::FeatureTest;
use super::selector::Incomplete;
use super::selector::MatchEvaluator;
use super::selector::MatchFactRow;
use super::selector::NamespaceTest;
use super::selector::NthPosition;
use super::selector::PositionalIndexPolicy;
use super::selector::PrecedingSiblingPrefix;
use super::selector::SelectorNodeID;
use super::selector::SelectorOp;
use super::selector::SelectorProgram;
use super::selector::ValueStateTestKind;
use super::selector::attribute_value_matches;
use super::selector::matches_an_plus_b;
use super::tree::StyleNodeID;

impl<'a> MatchEvaluator<'a> {
    /// Whether the node is the host of the tree whose rules are being evaluated.
    #[must_use]
    fn node_hosts_the_scope(&self, node: StyleNodeID) -> bool {
        match self.scope_shadow_root {
            Some(shadow_root) => self.tree.shadow_root_of(node) == Some(shadow_root),
            // A rule in the document scope is in no shadow tree, so it names no host.
            None => false,
        }
    }

    fn matches_host_argument(
        &mut self,
        program: &SelectorProgram,
        inner: SelectorNodeID,
        host: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let was_matching_host_argument = self.matching_host_argument.replace(true);
        let result = self.matches_node(program, inner, host, counters);
        self.matching_host_argument.set(was_matching_host_argument);
        result
    }

    /// Whether the subject satisfies one scope instance: not excluded by its limit, and matching
    /// what the rule writes inside it.
    ///
    /// The limit is checked from the subject up to the root, which is the specification's
    /// "descendant of the scoping root and not of any scoping limit". The binding is already in
    /// place, so `:scope` in either the limit or the selector names this root.
    fn subject_is_in_scope(
        &mut self,
        program: &SelectorProgram,
        limit: Option<SelectorNodeID>,
        inner: SelectorNodeID,
        node: StyleNodeID,
        root: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        if let Some(limit) = limit {
            let mut walked = Some(node);
            while let Some(current) = walked {
                if self.matches_node(program, limit, current, counters)? {
                    return Ok(false);
                }
                if current == root {
                    break;
                }
                walked = self.tree.parent(current);
            }
        }
        self.matches_node(program, inner, node, counters)
    }

    #[inline]
    fn matches_compound(
        &mut self,
        program: &SelectorProgram,
        first: u32,
        count: u32,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        for &operand in program.operands(first, count) {
            let matches = match program.node(operand) {
                SelectorOp::Feature(test) => self.matches_feature_node(program, test, node, counters)?,
                _ => self.matches_node(program, operand, node, counters)?,
            };
            if !matches {
                return Ok(false);
            }
        }
        Ok(true)
    }

    // https://drafts.csswg.org/css-shadow-1/#host-element-in-tree
    // When considered within its own shadow trees, the shadow host is featureless. Only the
    // :host, :host(), and :host-context() pseudo-classes are allowed to match it. Selector-list
    // pseudos preserve that restriction: only an alternative that reaches :host can match.
    fn matches_featureless_host(
        &mut self,
        program: &SelectorProgram,
        id: SelectorNodeID,
        host: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        match program.node(id) {
            SelectorOp::Host(inner) => self.matches_host_argument(program, inner, host, counters),
            SelectorOp::And { first, count } => {
                if !program.mentions_the_host(id) {
                    return Ok(false);
                }
                for &operand in program.operands(first, count) {
                    let operand_matches = match program.node(operand) {
                        SelectorOp::RelativeExists(_) => self.matches_node(program, operand, host, counters)?,
                        SelectorOp::IsNode(named) => host == named,
                        SelectorOp::ScopeRootInstance => self.scope_root_instance.get() == Some(host),
                        _ => self.matches_featureless_host(program, operand, host, counters)?,
                    };
                    if !operand_matches {
                        return Ok(false);
                    }
                }
                Ok(true)
            }
            SelectorOp::Or { first, count } => {
                for &operand in program.operands(first, count) {
                    if self.matches_featureless_host(program, operand, host, counters)? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            SelectorOp::Where(inner) => self.matches_featureless_host(program, inner, host, counters),
            SelectorOp::IsNode(named) => Ok(host == named),
            SelectorOp::ScopeRootInstance => Ok(self.scope_root_instance.get() == Some(host)),
            _ => Ok(false),
        }
    }

    /// Whether the dispatch bloom leaves `node` any chance of matching the compound at `inner`.
    /// A row facts cannot answer for yet is never prejudged.
    #[inline]
    fn relation_target_may_match(&self, program: &SelectorProgram, inner: SelectorNodeID, node: StyleNodeID) -> bool {
        let required = program.relation_target_bloom(inner);
        if required == 0 {
            return true;
        }
        // The shadow scope root matches featurelessly — so its facts prove nothing about it.
        if Some(node) == self.scope_shadow_root {
            return true;
        }
        match self.row_of(node) {
            Ok(row) => row.facts.dispatch_bloom_of(row.row, false) & required == required,
            Err(_) => true,
        }
    }

    #[inline]
    fn matches_relation_target(
        &mut self,
        program: &SelectorProgram,
        id: SelectorNodeID,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        if Some(node) != self.scope_shadow_root
            && let SelectorOp::And { first, count } = program.node(id)
        {
            return self.matches_compound(program, first, count, node, counters);
        }
        self.matches_node(program, id, node, counters)
    }

    fn matches_descendant_relation(
        &mut self,
        program: &SelectorProgram,
        relation: SelectorNodeID,
        inner: SelectorNodeID,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let side = self.match_workspace.as_ref().unwrap().1;
        let program_id = self.transitive_relation_program.get().unwrap();
        match self
            .match_workspace
            .as_ref()
            .unwrap()
            .0
            .relations(side)
            .lookup(program_id, relation, node)
        {
            Lookup::Known(()) => return Ok(true),
            Lookup::KnownAbsent => return Ok(false),
            Lookup::Missing(_) => {}
        }

        let mut current = node;
        let mut traversed: SmallVec<[StyleNodeID; 8]> = SmallVec::new();
        let mut incomplete = None;
        let answer = loop {
            traversed.push(current);
            let Some(adjacent) = self.parent_of(current) else {
                break false;
            };
            counters.bump(Counter::CombinatorSteps);
            match self.matches_relation_target(program, inner, adjacent, counters) {
                Ok(true) => break true,
                Ok(false) => {}
                Err(error) => {
                    incomplete.get_or_insert(error);
                }
            }
            match self
                .match_workspace
                .as_ref()
                .unwrap()
                .0
                .relations(side)
                .lookup(program_id, relation, adjacent)
            {
                Lookup::Known(()) => break true,
                Lookup::KnownAbsent => break false,
                Lookup::Missing(_) => {}
            }
            current = adjacent;
        };
        if !answer && let Some(incomplete) = incomplete {
            return Err(incomplete);
        }
        // The relation is transitive: every node crossed before reaching the same positive witness
        // or the same negative boundary has the same answer. Publishing the whole traversed prefix
        // turns a later sparse candidate into one lookup even when the immediately adjacent node
        // was not itself a selector candidate.
        for traversed_node in traversed {
            self.match_workspace.as_mut().unwrap().0.relations_by_evaluation_side[side as usize].insert(
                program_id,
                relation,
                traversed_node,
                answer,
            );
        }
        Ok(answer)
    }

    fn matches_preceding_sibling_prefix(
        &mut self,
        program: &SelectorProgram,
        relation: SelectorNodeID,
        inner: SelectorNodeID,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let side = self.match_workspace.as_ref().unwrap().1;
        let program_id = self.transitive_relation_program.get().unwrap();
        let Some(parent) = self.parent_of(node) else {
            return Ok(false);
        };
        let (parent_id, cached_prefix) = self.match_workspace.as_mut().unwrap().0.relations_by_evaluation_side
            [side as usize]
            .preceding_sibling_prefix(program_id, relation, parent);
        if let Some(prefix) = cached_prefix
            && prefix.next == Some(node)
        {
            return Ok(prefix.answer);
        }
        let mut prefix = cached_prefix.unwrap_or_else(|| PrecedingSiblingPrefix {
            next: self.children_of(parent).next(),
            answer: false,
        });
        let mut retried_from_start = false;
        let mut incomplete = None;
        loop {
            if prefix.next == Some(node) {
                if !prefix.answer
                    && let Some(incomplete) = incomplete
                {
                    return Err(incomplete);
                }
                self.match_workspace.as_mut().unwrap().0.relations_by_evaluation_side[side as usize]
                    .insert_preceding_sibling_prefix(program_id, relation, parent_id, prefix);
                return Ok(prefix.answer);
            }
            let Some(current) = prefix.next else {
                // Candidates normally arrive in tree order. If a caller asks out of order, restart
                // this one prefix from the sequence head rather than treating ordering as a
                // correctness requirement.
                if retried_from_start {
                    return Ok(false);
                }
                prefix = PrecedingSiblingPrefix {
                    next: self.children_of(parent).next(),
                    answer: false,
                };
                incomplete = None;
                retried_from_start = true;
                continue;
            };
            counters.bump(Counter::CombinatorSteps);
            if !prefix.answer {
                match self.matches_relation_target(program, inner, current, counters) {
                    Ok(true) => prefix.answer = true,
                    Ok(false) => {}
                    Err(error) => {
                        incomplete.get_or_insert(error);
                    }
                }
            }
            prefix.next = self.next_sibling_of(current);
        }
    }

    /// Whether every part name the rule writes is one this element is exposed to `host` under.
    fn part_names_reach_host(
        &self,
        program: &SelectorProgram,
        parts: SelectorNodeID,
        node: StyleNodeID,
        host: StyleNodeID,
    ) -> Result<bool, Incomplete> {
        let pairs = self.tree.part_hosts_of(node);
        let row = self.row_of(node)?;
        let reaches = |name: StyleAtomID| match pairs.is_empty() {
            // With no pairing recorded the element is addressable only under the names it carries,
            // and all of them reach the host of the tree it stands in.
            true => row.facts.parts_of(row.row).contains(&name),
            false => pairs
                .iter()
                .any(|&(exposed, exposed_to)| exposed == name && exposed_to == host),
        };
        let name_of = |id: SelectorNodeID| match program.node(id) {
            SelectorOp::Part(name) => Some(name),
            _ => None,
        };
        Ok(match program.node(parts) {
            SelectorOp::And { first, count } => program
                .operands(first, count)
                .iter()
                .filter_map(|&operand| name_of(operand))
                .all(reaches),
            other => match other {
                SelectorOp::Part(name) => reaches(name),
                _ => true,
            },
        })
    }

    pub(super) fn matches_node(
        &mut self,
        program: &SelectorProgram,
        id: SelectorNodeID,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        // A shadow root is a node of the style tree, which is what makes a combinator walking up out
        // of the tree stop at it rather than continue into the document. It is not an element,
        // though: it publishes no facts, and nothing matches it - not even `*`. The one exception is
        // `:host`, which names the host standing outside the tree, so a walk that reaches the root
        // crosses there and nowhere else.
        //
        // The only shadow root a walk from inside the tree can reach is the scope's own, so this
        // costs one comparison rather than a lookup.
        if Some(node) == self.scope_shadow_root {
            return match program.node(id) {
                SelectorOp::Host(inner) => match self.tree.host_of(node) {
                    Some(host) => self.matches_host_argument(program, inner, host, counters),
                    None => Ok(false),
                },
                // A scoping root outside the tree is the host, and a combinator reaching up out of
                // the tree lands here rather than on it. `:scope > .a` inside a scope rooted at the
                // host is the same walk as `:host > .a`, so it crosses the same way.
                SelectorOp::ScopeRootInstance => {
                    let host = self.tree.host_of(node);
                    Ok(host.is_some() && self.scope_root_instance.get() == host)
                }
                SelectorOp::IsNode(named) => Ok(self.tree.host_of(node) == Some(named)),
                // An in-shadow-tree query walks its axis from the shadow root and binds its anchor to
                // it, so a chain tying itself back to that anchor - the leftmost step of
                // `:host:has(> .child > .grand_child)` asks for the `.child`'s parent - compares
                // against the root itself. It does not cross to the host the way a scoping root does,
                // because the root is where the walk started.
                SelectorOp::RelativeAnchorInstance => {
                    counters.bump(Counter::StructuralTests);
                    Ok(self.relative_anchor.get() == Some(node))
                }
                // A compound with `:host` crosses to the host, but the host is featureless inside
                // its own shadow tree: only `:host` itself decides against the host's features,
                // through its argument, and `:has()` rides along as the one attached exception.
                // Any other simple selector in the compound - `div:host`, `:host.x`, `*:host`,
                // `:host:hover` - fails the whole compound rather than testing the host's facts.
                // https://drafts.csswg.org/css-shadow-1/#host-element-in-tree
                SelectorOp::And { .. } | SelectorOp::Or { .. } | SelectorOp::Where(_)
                    if program.mentions_the_host(id) =>
                {
                    self.tree.host_of(node).map_or(Ok(false), |host| {
                        self.matches_featureless_host(program, id, host, counters)
                    })
                }
                _ => Ok(false),
            };
        }
        match program.node(id) {
            SelectorOp::Feature(test) => self.matches_feature_node(program, test, node, counters),
            SelectorOp::Language { first, count } => {
                counters.bump(Counter::StateTests);
                let row = self.row_of(node)?;
                let tag = row.facts.language_tag_of(row.row);
                // An element with no resolved language matches no range at all, not even `*`.
                Ok(!tag.is_empty()
                    && program
                        .language_ranges(first, count)
                        .any(|range| crate::css::selector::language_range_matches_tag(range, tag)))
            }
            SelectorOp::State(fact) => {
                counters.bump(Counter::StateTests);
                let row = self.row_of(node)?;
                Ok(row.facts.states_of(row.row).contains(fact))
            }
            SelectorOp::And { first, count } => self.matches_compound(program, first, count, node, counters),
            SelectorOp::Or { first, count } => {
                if self.node_hosts_the_scope(node) && program.mentions_the_host(id) {
                    return self.matches_featureless_host(program, id, node, counters);
                }
                for &operand in program.operands(first, count) {
                    if self.matches_node(program, operand, node, counters)? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            SelectorOp::Where(inner) => self.matches_node(program, inner, node, counters),
            SelectorOp::Not(inner) => Ok(!self.matches_node(program, inner, node, counters)?),
            SelectorOp::Parent(inner) => {
                counters.bump(Counter::CombinatorSteps);
                match self.parent_of(node) {
                    Some(parent) => self.matches_relation_target(program, inner, parent, counters),
                    None => Ok(false),
                }
            }
            SelectorOp::Ancestor(inner) => {
                if self.match_workspace.is_some()
                    && self.transitive_relation_program.get().is_some()
                    && self.scope_root_instance.get().is_none()
                    && self.relative_anchor.get().is_none()
                    && self.scope_shadow_root.is_none()
                {
                    return self.matches_descendant_relation(program, id, inner, node, counters);
                }
                let mut ancestor = self.parent_of(node);
                while let Some(current) = ancestor {
                    counters.bump(Counter::CombinatorSteps);
                    if self.relation_target_may_match(program, inner, current)
                        && self.matches_relation_target(program, inner, current, counters)?
                    {
                        return Ok(true);
                    }
                    ancestor = self.parent_of(current);
                }
                Ok(false)
            }
            SelectorOp::PreviousSibling(inner) => {
                counters.bump(Counter::CombinatorSteps);
                match self.previous_sibling_of(node) {
                    Some(previous) => self.matches_relation_target(program, inner, previous, counters),
                    None => Ok(false),
                }
            }
            SelectorOp::PrecedingSibling(inner) => {
                if self.match_workspace.is_some()
                    && self.transitive_relation_program.get().is_some()
                    && self.scope_root_instance.get().is_none()
                    && self.relative_anchor.get().is_none()
                    && self.scope_shadow_root.is_none()
                {
                    return self.matches_preceding_sibling_prefix(program, id, inner, node, counters);
                }
                let Some(parent) = self.parent_of(node) else {
                    return Ok(false);
                };
                for sibling in self.children_of(parent) {
                    if sibling == node {
                        return Ok(false);
                    }
                    counters.bump(Counter::CombinatorSteps);
                    match self.matches_relation_target(program, inner, sibling, counters) {
                        Ok(true) => return Ok(true),
                        Ok(false) => {}
                        Err(Incomplete::MissingFacts(missing)) if missing == sibling => {
                            return Err(Incomplete::MissingSiblingFacts {
                                first: sibling,
                                last_exclusive: Some(node),
                            });
                        }
                        Err(incomplete) => return Err(incomplete),
                    }
                }
                Ok(false)
            }
            SelectorOp::NthPosition(position) => {
                let memoizes_answers = self.positional_index_policy == PositionalIndexPolicy::All;
                if memoizes_answers
                    && position.of_selector.is_none()
                    && let Some((workspace, side)) = self.match_workspace.as_mut()
                    && let Some(answer) = workspace.positional_answer(position, node, *side)
                {
                    return Ok(answer);
                }
                counters.bump(Counter::StructuralTests);
                let result = self.matches_nth(program, position, node, counters);
                if memoizes_answers
                    && position.of_selector.is_none()
                    && let Some((workspace, side)) = self.match_workspace.as_mut()
                    && let Ok(answer) = result
                {
                    workspace.insert_positional_answer(position, node, *side, answer);
                }
                result
            }
            // Each shadow operator consumes the relation it names. A generic descendant walk does
            // not pierce a shadow root, and a slot's assignment is not its DOM parent, so these
            // cannot be expressed as ordinary combinators.
            SelectorOp::Host(inner) => match self.node_hosts_the_scope(node) && !self.matching_host_argument.get() {
                true => self.matches_host_argument(program, inner, node, counters),
                false => Ok(false),
            },
            SelectorOp::Slotted(inner) => match self.tree.assigned_slot_of(node) {
                Some(_) => self.matches_node(program, inner, node, counters),
                None => Ok(false),
            },
            SelectorOp::AssignedSlot(inner) => {
                // The chain can pass through several trees; the slot this compound describes is the
                // one in the tree whose rules are being asked.
                const MAX_REASSIGNMENTS: usize = 32;
                let mut current = node;
                for _ in 0..MAX_REASSIGNMENTS {
                    let Some(slot) = self.tree.assigned_slot_of(current) else {
                        return Ok(false);
                    };
                    // The slot this compound describes is the one in the tree being asked. A shadow
                    // root's own tree scope is the outer one, so which tree a node is in is answered
                    // by walking to the root rather than by comparing scopes.
                    let in_this_tree = self.scope_shadow_root.is_none_or(|root| {
                        std::iter::successors(Some(slot), |&node| self.tree.parent(node)).any(|node| node == root)
                    });
                    if in_this_tree && self.matches_node(program, inner, slot, counters)? {
                        return Ok(true);
                    }
                    current = slot;
                }
                Ok(false)
            }
            SelectorOp::Part(part) => {
                let row = self.row_of(node)?;
                Ok(row.facts.parts_of(row.row).contains(&part))
            }
            // The host the part is exposed to, which is what the rule's outer compound describes.
            //
            // `exportparts` forwards a name outwards one host at a time, and each level exposes the
            // names it chose to its own host. A level therefore answers this op only when it exposes
            // every name the rule writes and its host is the element the outer compound describes:
            // taking the name from one level and the host from another would name an element that no
            // rule addresses, and taking only the outermost level would miss the rules of every tree
            // the name passed through on its way out.
            SelectorOp::ExposedToHost {
                host: host_compound,
                parts,
            } => {
                // https://drafts.csswg.org/css-shadow-parts-1/#part
                // `::part()` reaches one level down: into a tree hosted by an element of the tree
                // the rule itself is in. Without that bound a rule reached every part in the
                // document, including the ones in its own tree. A compound naming `:host` reaches
                // the rule's own tree as well, which is where a part forwarded out of it stands.
                let scope_host = self.scope_shadow_root.and_then(|root| self.tree.host_of(root));
                let mentions_the_host = program.mentions_the_host(host_compound);
                for level_host in self.part_exposure_hosts(node) {
                    let reaches_a_hosted_tree = self.tree.shadow_host_of(level_host) == scope_host;
                    let reaches_its_own_tree = Some(level_host) == scope_host && mentions_the_host;
                    if !reaches_a_hosted_tree && !reaches_its_own_tree {
                        continue;
                    }
                    if !self.part_names_reach_host(program, parts, node, level_host)? {
                        continue;
                    }
                    if self.matches_node(program, host_compound, level_host, counters)? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            SelectorOp::Root => {
                counters.bump(Counter::StructuralTests);
                Ok(self.root_matches_parentless_node && self.parent_of(node).is_none())
            }
            SelectorOp::InScope {
                root,
                limit,
                inner,
                names_the_scope,
            } => {
                counters.bump(Counter::StructuralTests);
                // One scope per element the `<scope-start>` matches, and the rule is relative to one
                // of them. A scoped selector carries an implied `:scope ` prefix unless it names
                // `:scope`, so the subject is normally a strict descendant of the root.
                //
                // An enclosing scope, when there is one, has already bound its own root; this
                // scope's root has to be inside it, so the walk stops there. The `<scope-start>` is
                // asked before the binding moves, because `:scope` written in one names the scope it
                // is nested in rather than the scope it opens.
                let enclosing_root = self.scope_root_instance.get();

                // A part stands inside a shadow tree, but an outer scope reaches it through the
                // host exposing it. Scope membership is therefore measured from that host rather
                // than from the part's DOM parent chain, which stops at the shadow root.
                if let Some(part_exposure_scope) = self.part_exposure_scope
                    && program.subject_is_a_part(inner)
                {
                    for level_host in self.part_exposure_hosts(node) {
                        if self.tree.tree_scope(level_host) != part_exposure_scope {
                            continue;
                        }
                        let mut candidate = match names_the_scope {
                            true => Some(level_host),
                            false => self.tree.parent(level_host),
                        };
                        while let Some(root_candidate) = candidate {
                            if self.matches_node(program, root, root_candidate, counters)? {
                                let outer = self.scope_root_instance.replace(Some(root_candidate));
                                let answer =
                                    self.subject_is_in_scope(program, limit, inner, node, root_candidate, counters);
                                self.scope_root_instance.set(outer);
                                if answer? {
                                    return Ok(true);
                                }
                            }
                            if Some(root_candidate) == enclosing_root {
                                break;
                            }
                            candidate = self.tree.parent(root_candidate);
                        }
                    }
                    return Ok(false);
                }

                let mut candidate = match names_the_scope {
                    true => Some(node),
                    false => self.tree.parent(node),
                };
                // A shadow root is not an element and roots nothing. The host standing outside the
                // tree can be the scoping root, though - `@scope (:host)` says so, and an `@scope`
                // with no `<scope-start>` whose `<style>` is a direct child of the shadow root roots
                // there too - so the walk crosses at the root and stops.
                if candidate == self.scope_shadow_root {
                    candidate = candidate.and_then(|root| self.tree.host_of(root));
                }
                while let Some(root_candidate) = candidate {
                    if self.matches_node(program, root, root_candidate, counters)? {
                        let outer = self.scope_root_instance.replace(Some(root_candidate));
                        let answer = self.subject_is_in_scope(program, limit, inner, node, root_candidate, counters);
                        self.scope_root_instance.set(outer);
                        if answer? {
                            return Ok(true);
                        }
                    }
                    if Some(root_candidate) == enclosing_root {
                        break;
                    }
                    candidate = self.tree.parent(root_candidate);
                    if candidate == self.scope_shadow_root {
                        candidate = candidate.and_then(|root| self.tree.host_of(root));
                    }
                }
                Ok(false)
            }
            SelectorOp::Empty => {
                counters.bump(Counter::StructuralTests);
                // Element children are style nodes and the tree answers for them. A text or comment
                // child is not, so the element publishes whether it holds one.
                let row = self.row_of(node)?;
                Ok(self.tree.first_element_child(node).is_none() && !row.facts.has_text_content_of(row.row))
            }
            SelectorOp::IsNode(named) => {
                counters.bump(Counter::StructuralTests);
                Ok(node == named)
            }
            SelectorOp::ScopeRootInstance => {
                counters.bump(Counter::StructuralTests);
                Ok(self.scope_root_instance.get() == Some(node))
            }
            SelectorOp::RelativeAnchorInstance => {
                counters.bump(Counter::StructuralTests);
                Ok(self.relative_anchor.get() == Some(node))
            }
            // Without an enclosing `@scope`, the scoping root is the root of the tree.
            SelectorOp::Scope => {
                counters.bump(Counter::StructuralTests);
                Ok(self.tree.parent(node).is_none())
            }
            SelectorOp::ValueState { kind, value } => {
                counters.bump(Counter::StateTests);
                let row = self.row_of(node)?;
                Ok(match kind {
                    ValueStateTestKind::Directionality => row.facts.directionality_of(row.row) == value,
                    ValueStateTestKind::CustomState => row.facts.custom_states_of(row.row).contains(&value),
                })
            }
            SelectorOp::Heading(levels) => {
                counters.bump(Counter::StructuralTests);
                let row = self.row_of(node)?;
                let level = row.facts.heading_level_of(row.row);
                Ok((1..=9).contains(&level) && levels & (1 << (level - 1)) != 0)
            }
            // The existential answer: does any candidate on the query's axis satisfy its compound.
            // Only the Boolean matters, so the walk stops at the first witness it finds.
            SelectorOp::RelativeExists(query_id) => {
                counters.bump(Counter::RelationalTests);
                let query = program.relative_query(query_id);
                // An in-shadow-tree query is anchored on the host and walked from the tree the host
                // opens. Binding the anchor to the shadow root as well as walking from it is what
                // ties a multi-compound chain back to the right place: the leftmost step of
                // `:host:has(> .child > .grand_child)` asks for the `.child`'s parent, and that is
                // the root, not the host.
                let Some(anchor) = traversal_anchor(node, query.match_in_shadow_tree, self.tree) else {
                    return Ok(false);
                };
                let mut matched = Ok(false);
                let mut found = None;
                let enclosing_anchor = self.relative_anchor.replace(Some(anchor));
                candidate_witnesses(
                    query.axis,
                    query.witness_is_below_the_axis,
                    anchor,
                    self.tree,
                    |candidate| match self.matches_node(program, query.compound, candidate, counters) {
                        Ok(true) => {
                            matched = Ok(true);
                            found = Some(candidate);
                            false
                        }
                        Ok(false) => true,
                        Err(incomplete) => {
                            matched = Err(match (query.axis, incomplete) {
                                (RelativeAxis::Descendant, Incomplete::MissingFacts(missing))
                                    if missing == candidate =>
                                {
                                    Incomplete::MissingDescendantFacts {
                                        root: anchor,
                                        first: candidate,
                                    }
                                }
                                (RelativeAxis::FollowingSibling, Incomplete::MissingFacts(missing))
                                    if missing == candidate =>
                                {
                                    Incomplete::MissingSiblingFacts {
                                        first: candidate,
                                        last_exclusive: None,
                                    }
                                }
                                (_, incomplete) => incomplete,
                            });
                            false
                        }
                    },
                );
                self.relative_anchor.set(enclosing_anchor);
                // A completed walk of the live tree is what a retained witness is: proof that the
                // query's Boolean on this element is true right now. Both outcomes are recorded -
                // the entry doubles as "the last completed evaluation answered true", which is the
                // half a routing-time re-verification cannot re-establish on its own. A walk that
                // ended incomplete proved neither and leaves the entry alone.
                if let Some(witnesses) = self.witnesses.as_deref_mut()
                    && let Some(program_id) = self.transitive_relation_program.get()
                    && program.retainable_relative_query(query_id).is_some()
                {
                    let key = RelationalWitnessKey {
                        program: program_id,
                        query: query_id,
                        anchor: node,
                    };
                    match (&matched, found) {
                        (Ok(true), Some(witness)) => {
                            witnesses.push(WitnessEffect::Retain(key, witness));
                        }
                        (Ok(false), _) => witnesses.push(WitnessEffect::Clear(key)),
                        _ => {}
                    }
                }
                matched
            }
        }
    }

    fn matches_feature(
        &self,
        program: &SelectorProgram,
        test: FeatureTest,
        node: StyleNodeID,
    ) -> Result<bool, Incomplete> {
        if test == FeatureTest::AnyElement {
            return Ok(true);
        }
        let row = self.row_of(node)?;
        Ok(match test {
            FeatureTest::AnyElement => true,
            FeatureTest::Namespace(NamespaceTest::None) => row.facts.namespace_of(row.row) == StyleAtomID::NONE,
            FeatureTest::Namespace(NamespaceTest::Named(namespace)) => row.facts.namespace_of(row.row) == namespace,
            FeatureTest::TagName(tag) => tag.matches(row.facts.tag_of(row.row), row.facts.namespace_of(row.row)),
            FeatureTest::Id(id) => row.facts.id_of(row.row) == id,
            FeatureTest::Class(class) => row.facts.classes_of(row.row).contains(&class),
            FeatureTest::Attribute(test) => {
                let insensitive = match test.case {
                    AttributeCase::Sensitive => false,
                    AttributeCase::Insensitive => true,
                    AttributeCase::InsensitiveForNamespace(namespace) => row.facts.namespace_of(row.row) == namespace,
                };
                // `[*|x]` names one attribute per namespace the element carries `x` in, and the
                // test holds when any of them satisfies it.
                self.attributes_named_by(row, test)
                    .any(|attribute| self.matches_attribute_value(program, test, row.facts, attribute, insensitive))
            }
        })
    }

    fn matches_feature_node(
        &self,
        program: &SelectorProgram,
        test: FeatureTest,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        counters.bump(Counter::LocalFeatureTests);
        self.matches_feature(program, test, node)
    }

    /// Every attribute a test names.
    ///
    /// There can be more than one: `[*|x]` names the attribute called `x` in each namespace the
    /// element carries it in, and they publish the same any-namespace atom.
    fn attributes_named_by(
        &self,
        row: MatchFactRow<'a>,
        test: AttributeTest,
    ) -> impl Iterator<Item = super::index::AttributeFact> {
        // NB: Whether this subject folds selector names is one namespace comparison for the whole test
        //     rather than one per attribute.
        let folds = !test.fold_in_namespace.is_none() && row.facts.namespace_of(row.row) == test.fold_in_namespace;
        row.facts
            .attributes_of(row.row)
            .iter()
            .copied()
            .filter(move |attribute| {
                // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
                // When comparing the name part of a CSS attribute selector to the names of attributes on HTML elements in HTML
                // documents, the name part of the CSS attribute selector must first be converted to ASCII lowercase. The same
                // selector when compared to other attributes must be compared according to its original case. In both cases, the
                // comparison is case-sensitive.
                let name = if folds { test.folded } else { test.name };
                let written = if test.any_namespace {
                    row.facts.attribute_name_forms(attribute.name).local
                } else {
                    attribute.name
                };
                written == name
            })
    }

    fn matches_attribute_value(
        &self,
        program: &SelectorProgram,
        test: AttributeTest,
        facts: &StyleNodeFacts,
        attribute: super::index::AttributeFact,
        insensitive: bool,
    ) -> bool {
        if test.operator == AttributeOperator::Presence {
            return true;
        }
        // An exact test on two interned values is an integer comparison, which is why the batch
        // carries no text for attributes only tested this way.
        if test.operator == AttributeOperator::Exact
            && !insensitive
            && !test.value_atom.is_none()
            && !attribute.value.is_none()
        {
            return test.value_atom == attribute.value;
        }

        let literal = program.literal(test.value_offset, test.value_length);
        let Some(value) = facts.text_of(attribute) else {
            return false;
        };
        attribute_value_matches(test.operator, value, literal, insensitive)
    }

    pub(super) fn matches_nth(
        &mut self,
        program: &SelectorProgram,
        position: NthPosition,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        if position.of_selector.is_none()
            && (position.step != 0 || self.positional_index_policy == PositionalIndexPolicy::All)
            && let Some(index) = self.indexed_sibling_position(position, node)?
        {
            return Ok(matches_an_plus_b(position.step, position.offset, index));
        }

        // https://drafts.csswg.org/selectors/#child-index
        // A positional test counts the subject among its inclusive siblings, and the root of a tree
        // has none - which makes it the one and only element of its sequence rather than absent from
        // one. `:first-child`, `:last-child` and `:only-child` all name it.
        // https://drafts.csswg.org/selectors/#typedef-type-selector
        // An element's type is its qualified name, so two `p` elements in different namespaces are
        // different types and are counted in different sequences.
        let subject_type = match position.of_type {
            true => {
                let row = self.row_of(node)?;
                Some((row.facts.tag_of(row.row), row.facts.namespace_of(row.row)))
            }
            false => None,
        };

        // https://drafts.csswg.org/selectors/#child-index
        // A positional test counts the subject among its inclusive siblings, and the root of a tree
        // has none - which makes it the one and only element of its sequence rather than absent from
        // one. `:first-child`, `:last-child` and `:only-child` all name it.
        let Some(parent) = self.parent_of(node) else {
            if !self.counts_in_sequence(program, position, subject_type, node, counters)? {
                return Ok(false);
            }
            return Ok(matches_an_plus_b(position.step, position.offset, 1));
        };
        // The subject has to be one of the counted siblings, or it has no position in the sequence.
        if !self.counts_in_sequence(program, position, subject_type, node, counters)? {
            return Ok(false);
        }

        // Count towards the near end only. The whole sequence is never needed, and a sequence of
        // thousands of siblings is what a long list or a table is, so materializing one per test
        // made `:first-child` cost the length of its parent's child list.
        let mut index: i64 = 1;
        let bounded = position.step == 0;
        let mut current = match position.from_end {
            true => self.next_sibling_of(node),
            false => self.children_of(parent).next(),
        };
        while let Some(sibling) = current {
            if !position.from_end && sibling == node {
                break;
            }
            match self.counts_in_sequence(program, position, subject_type, sibling, counters) {
                Ok(true) => {
                    index += 1;
                    // A test with no step names one position, so once the count is past it no further
                    // sibling can bring it back. `:first-child` stops at the first counted neighbour.
                    if bounded && index > i64::from(position.offset) {
                        return Ok(false);
                    }
                }
                Ok(false) => {}
                Err(Incomplete::MissingFacts(missing)) if missing == sibling => {
                    return Err(Incomplete::MissingSiblingFacts {
                        first: sibling,
                        last_exclusive: (!position.from_end).then_some(node),
                    });
                }
                Err(incomplete) => return Err(incomplete),
            }
            current = self.next_sibling_of(sibling);
        }
        Ok(matches_an_plus_b(position.step, position.offset, index))
    }

    /// Whether one sibling is counted by this positional test's sequence.
    fn counts_in_sequence(
        &mut self,
        program: &SelectorProgram,
        position: NthPosition,
        subject_type: Option<(StyleAtomID, StyleAtomID)>,
        sibling: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        match (subject_type, position.of_selector) {
            (Some((tag, namespace)), _) => {
                let row = self.row_of(sibling)?;
                Ok(row.facts.tag_of(row.row) == tag && row.facts.namespace_of(row.row) == namespace)
            }
            (None, Some(selector)) => self.matches_node(program, selector, sibling, counters),
            (None, None) => Ok(true),
        }
    }
}
