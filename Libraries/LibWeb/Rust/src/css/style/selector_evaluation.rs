/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The one interpreter of compiled selector programs.
//!
//! A program is evaluated against a subject: whatever holds the elements and the facts a selector
//! tests. The style engine's subject reads its own storage, and the DOM query subject reads the live
//! DOM. Every matching rule lives here, so two subjects can differ in what their storage holds and
//! never in what a selector means.
//!
//! A subject brings its accelerations along as hooks. A hook can only skip work: each default is
//! the answer the evaluation reaches without the shortcut.

use std::convert::Infallible;

use smallvec::SmallVec;

use super::index::StyleAtomID;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use super::relative_selector::RelativeAxis;
use super::relative_selector::RelativeQueryID;
use super::selector::AtomSpace;
use super::selector::AttributeCase;
use super::selector::AttributeOperator;
use super::selector::AttributeTest;
use super::selector::FeatureTest;
use super::selector::Incomplete;
use super::selector::NamespaceTest;
use super::selector::NthPosition;
use super::selector::SelectorNodeID;
use super::selector::SelectorOp;
use super::selector::SelectorProgram;
use super::selector::ValueStateTestKind;
use super::selector::attribute_value_matches;
use super::selector::matches_an_plus_b;
use super::transaction::StateFact;
use super::tree::StyleNodeID;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::selector::language_range_matches_tag;

/// Where an evaluation reports how much work it did.
pub(crate) trait CounterSink {
    fn bump(&self, counter: Counter);
}

impl CounterSink for Counters {
    fn bump(&self, counter: Counter) {
        Counters::bump(self, counter);
    }
}

/// A subject that counts nothing.
impl CounterSink for () {
    fn bump(&self, _counter: Counter) {}
}

/// Why a subject could not answer, refined by the scan that asked.
///
/// A scan over siblings or descendants that stops at a node its subject cannot read still knows the
/// rest of the range it would have read, which lets its caller widen the question to exactly that.
pub(crate) trait ScanIncomplete<N>: Copy {
    /// The same, reported by a sibling scan that reached `sibling` and would have continued up to
    /// `last_exclusive`, or to the end of the sequence when that is none.
    #[must_use]
    fn in_sibling_scan(self, sibling: N, last_exclusive: Option<N>) -> Self;

    /// The same, reported by a scan of `root`'s descendants that reached `candidate`.
    #[must_use]
    fn in_descendant_scan(self, root: N, candidate: N) -> Self;
}

impl ScanIncomplete<StyleNodeID> for Incomplete {
    fn in_sibling_scan(self, sibling: StyleNodeID, last_exclusive: Option<StyleNodeID>) -> Self {
        match self {
            Self::MissingFacts(missing) if missing == sibling => Self::MissingSiblingFacts {
                first: sibling,
                last_exclusive,
            },
            other => other,
        }
    }

    fn in_descendant_scan(self, root: StyleNodeID, candidate: StyleNodeID) -> Self {
        match self {
            Self::MissingFacts(missing) if missing == candidate => {
                Self::MissingDescendantFacts { root, first: candidate }
            }
            other => other,
        }
    }
}

/// A subject that can always answer.
impl<N> ScanIncomplete<N> for Infallible {
    fn in_sibling_scan(self, _sibling: N, _last_exclusive: Option<N>) -> Self {
        match self {}
    }

    fn in_descendant_scan(self, _root: N, _candidate: N) -> Self {
        match self {}
    }
}

/// How a subject's nodes relate.
///
/// A copyable handle, so a walk can hold it while it evaluates candidates. The nodes are elements,
/// plus the shadow root of the tree whose selectors are evaluated: that is where a combinator walking
/// up out of a shadow tree arrives, and the evaluator never asks a shadow root for a fact.
pub(crate) trait SelectorTree: Copy {
    type Node: Copy + Eq;

    fn parent(self, node: Self::Node) -> Option<Self::Node>;
    fn previous_sibling(self, node: Self::Node) -> Option<Self::Node>;
    fn next_sibling(self, node: Self::Node) -> Option<Self::Node>;
    fn first_child(self, parent: Self::Node) -> Option<Self::Node>;
    /// The first of a node's inclusive siblings, which is the node itself when it has none.
    fn first_sibling(self, node: Self::Node) -> Self::Node;

    /// A node's inclusive siblings, in tree order.
    fn inclusive_siblings(self, node: Self::Node) -> impl Iterator<Item = Self::Node> {
        std::iter::successors(Some(self.first_sibling(node)), move |&sibling| {
            self.next_sibling(sibling)
        })
    }

    /// The parent in the tree as it stands now. Scope, slot and relational walks read it even where
    /// the combinators read an earlier state of the tree.
    fn live_parent(self, node: Self::Node) -> Option<Self::Node> {
        self.parent(node)
    }

    fn shadow_root_of(self, host: Self::Node) -> Option<Self::Node>;
    fn host_of(self, shadow_root: Self::Node) -> Option<Self::Node>;

    /// The element after `node` in tree order that is a descendant of `root`, where `node` is `root` itself to start.
    fn next_in_subtree(self, node: Self::Node, root: Self::Node) -> Option<Self::Node> {
        if let Some(child) = self.first_child(node) {
            return Some(child);
        }
        let mut current = node;
        while current != root {
            if let Some(sibling) = self.next_sibling(current) {
                return Some(sibling);
            }
            current = self.parent(current)?;
        }
        None
    }

    /// Visit the candidate witnesses of a relative selector along `axis` from `anchor`, in tree
    /// order, until `visit` returns false.
    fn for_each_on_axis(
        self,
        axis: RelativeAxis,
        below_the_axis: bool,
        anchor: Self::Node,
        mut visit: impl FnMut(Self::Node) -> bool,
    ) {
        // Visit the subtree of `root`, `root` itself included when `inclusive`, until `visit` returns false.
        fn visit_subtree<T: SelectorTree>(
            tree: T,
            root: T::Node,
            inclusive: bool,
            visit: &mut impl FnMut(T::Node) -> bool,
        ) -> bool {
            if inclusive && !visit(root) {
                return false;
            }
            let mut descendant = tree.next_in_subtree(root, root);
            while let Some(candidate) = descendant {
                if !visit(candidate) {
                    return false;
                }
                descendant = tree.next_in_subtree(candidate, root);
            }
            true
        }

        let (first, every_sibling, subtree) = match axis {
            RelativeAxis::Descendant => {
                visit_subtree(self, anchor, false, &mut visit);
                return;
            }
            RelativeAxis::Child => (self.first_child(anchor), true, false),
            RelativeAxis::NextSibling => (self.next_sibling(anchor), false, false),
            RelativeAxis::FollowingSibling => (self.next_sibling(anchor), true, false),
            // The witness lies under the sibling rather than being it, so the sibling's whole subtree is the
            // candidate range.
            RelativeAxis::NextSiblingSubtree => (self.next_sibling(anchor), false, true),
            RelativeAxis::FollowingSiblingSubtree => (self.next_sibling(anchor), true, true),
        };
        let mut sibling = first;
        while let Some(candidate) = sibling {
            let proceed = match subtree {
                true => visit_subtree(self, candidate, !below_the_axis, &mut visit),
                false => visit(candidate),
            };
            if !proceed || !every_sibling {
                return;
            }
            sibling = self.next_sibling(candidate);
        }
    }

    /// The slot an element is assigned to.
    fn assigned_slot_of(self, _node: Self::Node) -> Option<Self::Node> {
        None
    }

    /// The host of the shadow tree a node is in.
    fn shadow_host_of(self, _node: Self::Node) -> Option<Self::Node> {
        None
    }
}

/// What a feature test reads of one element. Names are atoms of the program's atom space.
pub(crate) trait ElementFeatures {
    type Attribute: Copy;

    fn local_name_is(&self, name: StyleAtomID) -> bool;
    /// `NONE` names the null namespace.
    fn namespace_is(&self, namespace: StyleAtomID) -> bool;
    fn has_id(&self, id: StyleAtomID) -> bool;
    fn has_class(&self, class: StyleAtomID) -> bool;
    /// The attributes with the qualified name `name`, or with the local name `name` in any namespace.
    fn attributes_named(&self, name: StyleAtomID, any_namespace: bool) -> impl Iterator<Item = Self::Attribute> + '_;
}

/// Whether an element passes one feature test. `value_matches` compares an attribute's value with the one the test
/// names, ASCII case-insensitively when it is told to.
pub(crate) fn matches_feature<E: ElementFeatures>(
    element: &E,
    test: FeatureTest,
    mut value_matches: impl FnMut(AttributeTest, E::Attribute, bool) -> bool,
) -> bool {
    // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
    // When comparing a CSS element type selector to the names of HTML elements in HTML documents, the CSS element type
    // selector must first be converted to ASCII lowercase. The same selector when compared to other elements must be
    // compared according to its original case. In both cases, to match, the values must be identical to each other
    // (and therefore the comparison is case sensitive).
    //
    // When comparing the name part of a CSS attribute selector to the names of attributes on HTML elements in HTML
    // documents, the name part of the CSS attribute selector must first be converted to ASCII lowercase. The same
    // selector when compared to other attributes must be compared according to its original case. In both cases, the
    // comparison is case-sensitive.
    //
    // NB: A test's folding namespace is the HTML namespace in an HTML document, and none anywhere else.
    let folds =
        |fold_in_namespace: StyleAtomID| !fold_in_namespace.is_none() && element.namespace_is(fold_in_namespace);
    match test {
        FeatureTest::AnyElement => true,
        FeatureTest::Namespace(NamespaceTest::None) => element.namespace_is(StyleAtomID::NONE),
        FeatureTest::Namespace(NamespaceTest::Named(namespace)) => element.namespace_is(namespace),
        FeatureTest::TagName(tag) => element.local_name_is(match folds(tag.fold_in_namespace) {
            true => tag.folded,
            false => tag.written,
        }),
        FeatureTest::Id(id) => element.has_id(id),
        FeatureTest::Class(class) => element.has_class(class),
        FeatureTest::Attribute(test) => {
            let insensitive = match test.case {
                AttributeCase::Sensitive => false,
                AttributeCase::Insensitive => true,
                AttributeCase::InsensitiveForNamespace(namespace) => element.namespace_is(namespace),
            };
            let name = match folds(test.fold_in_namespace) {
                true => test.folded,
                false => test.name,
            };
            // `[*|x]` names one attribute per namespace the element carries `x` in, and the test holds when any of
            // them satisfies it.
            element
                .attributes_named(name, test.any_namespace)
                .any(|attribute| value_matches(test, attribute, insensitive))
        }
    }
}

/// The elements a selector program is evaluated against, and their facts.
///
/// Facts are read through a row, which a subject fetches once per element and the evaluator then
/// asks every test of. Names are atoms of the program's own atom space. A subject answers what its
/// storage holds, and nothing else: which name, case or namespace a test compares is decided here.
pub(crate) trait SelectorSubject {
    /// The atoms the subject's names are keyed by, which are the only ones a program it evaluates can hold.
    type Atoms: AtomSpace;
    type Node: Copy + Eq;
    type Tree: SelectorTree<Node = Self::Node>;
    type Row: Copy;
    type Attribute: Copy;
    type Features<'s>: ElementFeatures<Attribute = Self::Attribute>
    where
        Self: 's;
    type Incomplete: ScanIncomplete<Self::Node>;
    type Counters: CounterSink;
    /// Where a subject keeps one sibling sequence's preceding-sibling progress.
    type PrefixSlot: Copy;
    /// Whether the subject remembers how many siblings an `of S` positional test counts, through
    /// `sibling_count_of` and `record_sibling_count_of`.
    const REMEMBERS_SIBLING_COUNTS_OF_SELECTORS: bool = false;

    fn tree(&self) -> Self::Tree;

    fn row(&mut self, node: Self::Node) -> Result<Self::Row, Self::Incomplete>;
    fn features(&self, row: Self::Row) -> Self::Features<'_>;
    /// Whether two elements have the same local name and namespace.
    fn same_type(&self, row: Self::Row, other: Self::Row) -> bool;
    /// An attribute's value as an atom, or `NONE` where the subject interned none.
    fn attribute_value_atom(&self, attribute: Self::Attribute) -> StyleAtomID;
    fn attribute_value_text(&self, attribute: Self::Attribute) -> Option<TokenizerInput<'_>>;
    fn has_state(&self, row: Self::Row, fact: StateFact) -> bool;
    /// The element's resolved language tag, empty when it has none.
    fn language_tag(&self, row: Self::Row) -> TokenizerInput<'_>;
    fn directionality_is(&self, row: Self::Row, direction: StyleAtomID) -> bool;
    fn has_custom_state(&self, row: Self::Row, state: StyleAtomID) -> bool;
    /// The element's heading level, or zero when it is not a heading.
    fn heading_level(&self, row: Self::Row) -> u8;
    /// Whether no child of the element keeps it from being `:empty`.
    fn is_empty(&mut self, node: Self::Node) -> Result<bool, Self::Incomplete>;
    /// Whether the element is the root of its document.
    fn is_root(&self, node: Self::Node) -> bool;

    /// Whether the element is the one named by identity.
    fn is_node(&self, _node: Self::Node, _named: StyleNodeID) -> bool {
        false
    }

    fn has_part(&self, _row: Self::Row, _part: StyleAtomID) -> bool {
        false
    }

    /// The distinct hosts a `::part()` rule can address the element from, nearest first.
    fn part_exposure_hosts(&self, _node: Self::Node) -> SmallVec<[Self::Node; 1]> {
        SmallVec::new()
    }

    /// When a part is evaluated through the tree scope it is exposed to, the hosts through which the
    /// element is exposed there.
    fn part_hosts_in_exposure_scope(&self, _node: Self::Node) -> Option<SmallVec<[Self::Node; 1]>> {
        None
    }

    /// Whether the element is exposed to `host` under the part name.
    fn exposes_part_to(
        &mut self,
        _node: Self::Node,
        _part: StyleAtomID,
        _host: Self::Node,
    ) -> Result<bool, Self::Incomplete> {
        Ok(false)
    }

    // Accelerations. Each default is what the evaluation does without one.

    /// Whether `node` can possibly match the compound at `compound`.
    fn may_match(&self, _program: &SelectorProgram<Self::Atoms>, _compound: SelectorNodeID, _node: Self::Node) -> bool {
        true
    }

    /// Whether answers of transitive relations may be remembered under these bindings.
    fn remembers_relations(&self, _bindings: &SelectorBindings<Self::Node>) -> bool {
        false
    }

    /// A remembered answer of the relation at `relation` for `node`.
    fn relation_answer(
        &self,
        _program: &SelectorProgram<Self::Atoms>,
        _relation: SelectorNodeID,
        _node: Self::Node,
    ) -> Option<bool> {
        None
    }

    fn record_relation_answer(
        &mut self,
        _program: &SelectorProgram<Self::Atoms>,
        _relation: SelectorNodeID,
        _node: Self::Node,
        _answer: bool,
    ) {
    }

    /// The remembered progress of the preceding-sibling relation at `relation` through `node`'s
    /// inclusive siblings, or none when the subject remembers none.
    fn preceding_sibling_prefix(
        &mut self,
        program: &SelectorProgram<Self::Atoms>,
        relation: SelectorNodeID,
        node: Self::Node,
    ) -> Option<RememberedPrefix<Self::PrefixSlot, Self::Node>>;

    fn record_preceding_sibling_prefix(
        &mut self,
        program: &SelectorProgram<Self::Atoms>,
        relation: SelectorNodeID,
        slot: Self::PrefixSlot,
        prefix: PrecedingSiblingPrefix<Self::Node>,
    );

    fn positional_answer(&self, _position: NthPosition, _node: Self::Node) -> Option<bool> {
        None
    }

    fn record_positional_answer(&mut self, _position: NthPosition, _node: Self::Node, _answer: bool) {}

    /// The element's 1-based index in the sequence a positional test with no `of S` counts.
    fn sibling_index(&mut self, _position: NthPosition, _node: Self::Node) -> Result<Option<i64>, Self::Incomplete> {
        Ok(None)
    }

    /// A remembered number of siblings matching the selector at `selector`, counted from the start of the node's
    /// sibling sequence (or from its end) through the node.
    fn sibling_count_of(&self, _selector: SelectorNodeID, _from_end: bool, _node: Self::Node) -> Option<u32> {
        None
    }

    fn record_sibling_count_of(&mut self, _selector: SelectorNodeID, _from_end: bool, _node: Self::Node, _count: u32) {}

    /// A remembered answer of the relative query for `anchor`.
    fn relative_answer(
        &self,
        _program: &SelectorProgram<Self::Atoms>,
        _query: RelativeQueryID,
        _anchor: Self::Node,
    ) -> Option<bool> {
        None
    }

    /// Record the completed answer of a relative query, and the witness that made it true.
    fn record_relative_answer(
        &mut self,
        _program: &SelectorProgram<Self::Atoms>,
        _query: RelativeQueryID,
        _anchor: Self::Node,
        _answer: bool,
        _witness: Option<Self::Node>,
    ) {
    }
}

/// How far a preceding-sibling relation has been answered through one sibling sequence: every
/// sibling before `next` has been tried, and `answer` says whether one of them matched.
#[derive(Clone, Copy)]
pub(crate) struct PrecedingSiblingPrefix<N> {
    pub(crate) next: Option<N>,
    pub(crate) answer: bool,
}

/// The preceding-sibling progress a subject remembers for one sibling sequence, and where it keeps it.
pub(crate) struct RememberedPrefix<Slot, N> {
    pub(crate) slot: Slot,
    pub(crate) prefix: Option<PrecedingSiblingPrefix<N>>,
}

impl<N> Default for PrecedingSiblingPrefix<N> {
    fn default() -> Self {
        Self {
            next: None,
            answer: false,
        }
    }
}

/// What a selector's context names while it is evaluated.
#[derive(Clone, Copy)]
pub(crate) struct SelectorBindings<N> {
    /// The shadow root of the tree whose selectors are evaluated, when it is one. `:host` names the
    /// host of the tree its selector is in, so a selector from a document names no host at all.
    pub(crate) scope_shadow_root: Option<N>,
    /// The scoping root `:scope` names: the root of the scope a `<scope-end>` is checked against, or
    /// the element a query is scoped to.
    pub(crate) scope_root_instance: Option<N>,
    /// Whether evaluation is inside the argument of `:host()`. A nested `:host` does not describe a
    /// feature of the host and must not match there.
    pub(crate) matching_host_argument: bool,
    /// The anchor of the relational query being evaluated. Bound only while the witness walk runs,
    /// so that a nested `:has()` names its own anchor.
    pub(crate) relative_anchor: Option<N>,
}

impl<N> Default for SelectorBindings<N> {
    fn default() -> Self {
        Self {
            scope_shadow_root: None,
            scope_root_instance: None,
            matching_host_argument: false,
            relative_anchor: None,
        }
    }
}

type Answer<S> = Result<bool, <S as SelectorSubject>::Incomplete>;

/// Evaluates selector programs against one subject.
pub(crate) struct SelectorEvaluator<S: SelectorSubject> {
    pub(crate) subject: S,
    pub(crate) bindings: SelectorBindings<S::Node>,
}

impl<S: SelectorSubject> SelectorEvaluator<S> {
    /// Whether the node is the host of the tree whose selectors are being evaluated.
    fn node_hosts_the_scope(&self, node: S::Node) -> bool {
        match self.bindings.scope_shadow_root {
            Some(shadow_root) => self.subject.tree().shadow_root_of(node) == Some(shadow_root),
            // A selector in the document scope is in no shadow tree, so it names no host.
            None => false,
        }
    }

    fn matches_host_argument(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        inner: SelectorNodeID,
        host: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        let was_matching_host_argument = std::mem::replace(&mut self.bindings.matching_host_argument, true);
        let result = self.matches_node(program, inner, host, counters);
        self.bindings.matching_host_argument = was_matching_host_argument;
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
        program: &SelectorProgram<S::Atoms>,
        limit: Option<SelectorNodeID>,
        inner: SelectorNodeID,
        node: S::Node,
        root: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        if let Some(limit) = limit {
            let tree = self.subject.tree();
            let mut walked = Some(node);
            while let Some(current) = walked {
                if self.matches_node(program, limit, current, counters)? {
                    return Ok(false);
                }
                if current == root {
                    break;
                }
                walked = tree.live_parent(current);
            }
        }
        self.matches_node(program, inner, node, counters)
    }

    /// Whether `root_candidate` roots a scope of the `@scope` that `node` is in.
    #[allow(clippy::too_many_arguments)]
    fn matches_in_scope_rooted_at(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        root: SelectorNodeID,
        limit: Option<SelectorNodeID>,
        inner: SelectorNodeID,
        node: S::Node,
        root_candidate: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        if !self.matches_node(program, root, root_candidate, counters)? {
            return Ok(false);
        }
        let outer = self.bindings.scope_root_instance.replace(root_candidate);
        let answer = self.subject_is_in_scope(program, limit, inner, node, root_candidate, counters);
        self.bindings.scope_root_instance = outer;
        answer
    }

    #[inline]
    fn matches_compound(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        first: u32,
        count: u32,
        node: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        for &operand in program.operands(first, count) {
            let matches = match program.node(operand) {
                SelectorOp::Feature(test) => {
                    counters.bump(Counter::LocalFeatureTests);
                    self.matches_feature(program, test, node)?
                }
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
        program: &SelectorProgram<S::Atoms>,
        id: SelectorNodeID,
        host: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        match program.node(id) {
            SelectorOp::Host(inner) => self.matches_host_argument(program, inner, host, counters),
            SelectorOp::And { first, count } => {
                if !program.mentions_the_host(id) {
                    return Ok(false);
                }
                for &operand in program.operands(first, count) {
                    let operand_matches = match program.node(operand) {
                        SelectorOp::RelativeExists(_) => self.matches_node(program, operand, host, counters)?,
                        SelectorOp::IsNode(named) => self.subject.is_node(host, named),
                        SelectorOp::ScopeRootInstance => self.bindings.scope_root_instance == Some(host),
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
            SelectorOp::IsNode(named) => Ok(self.subject.is_node(host, named)),
            SelectorOp::ScopeRootInstance => Ok(self.bindings.scope_root_instance == Some(host)),
            _ => Ok(false),
        }
    }

    /// Whether `node` can possibly match the compound at `inner`. The shadow scope root matches
    /// featurelessly, so its facts prove nothing about it.
    #[inline]
    fn relation_target_may_match(
        &self,
        program: &SelectorProgram<S::Atoms>,
        inner: SelectorNodeID,
        node: S::Node,
    ) -> bool {
        Some(node) == self.bindings.scope_shadow_root || self.subject.may_match(program, inner, node)
    }

    #[inline]
    fn matches_relation_target(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        id: SelectorNodeID,
        node: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        if Some(node) != self.bindings.scope_shadow_root
            && let SelectorOp::And { first, count } = program.node(id)
        {
            return self.matches_compound(program, first, count, node, counters);
        }
        self.matches_node(program, id, node, counters)
    }

    fn matches_descendant_relation(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        relation: SelectorNodeID,
        inner: SelectorNodeID,
        node: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        if let Some(answer) = self.subject.relation_answer(program, relation, node) {
            return Ok(answer);
        }

        let tree = self.subject.tree();
        let mut current = node;
        let mut traversed: SmallVec<[S::Node; 8]> = SmallVec::new();
        let mut incomplete = None;
        let answer = loop {
            traversed.push(current);
            let Some(adjacent) = tree.parent(current) else {
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
            if let Some(answer) = self.subject.relation_answer(program, relation, adjacent) {
                break answer;
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
            self.subject
                .record_relation_answer(program, relation, traversed_node, answer);
        }
        Ok(answer)
    }

    fn matches_preceding_sibling(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        relation: SelectorNodeID,
        inner: SelectorNodeID,
        node: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        let tree = self.subject.tree();
        let remembered = match self.subject.remembers_relations(&self.bindings) {
            true => self.subject.preceding_sibling_prefix(program, relation, node),
            false => None,
        };
        let Some(RememberedPrefix {
            slot,
            prefix: cached_prefix,
        }) = remembered
        else {
            for current in tree.inclusive_siblings(node) {
                if current == node {
                    return Ok(false);
                }
                counters.bump(Counter::CombinatorSteps);
                match self.matches_relation_target(program, inner, current, counters) {
                    Ok(true) => return Ok(true),
                    Ok(false) => {}
                    Err(incomplete) => return Err(incomplete.in_sibling_scan(current, Some(node))),
                }
            }
            return Ok(false);
        };

        if let Some(prefix) = cached_prefix
            && prefix.next == Some(node)
        {
            return Ok(prefix.answer);
        }
        let mut prefix = cached_prefix.unwrap_or_else(|| PrecedingSiblingPrefix {
            next: Some(tree.first_sibling(node)),
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
                self.subject
                    .record_preceding_sibling_prefix(program, relation, slot, prefix);
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
                    next: Some(tree.first_sibling(node)),
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
            prefix.next = tree.next_sibling(current);
        }
    }

    /// Whether every part name the rule writes is one this element is exposed to `host` under.
    fn part_names_reach_host(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        parts: SelectorNodeID,
        node: S::Node,
        host: S::Node,
    ) -> Answer<S> {
        match program.node(parts) {
            SelectorOp::And { first, count } => {
                for &operand in program.operands(first, count) {
                    if let SelectorOp::Part(name) = program.node(operand)
                        && !self.subject.exposes_part_to(node, name, host)?
                    {
                        return Ok(false);
                    }
                }
                Ok(true)
            }
            SelectorOp::Part(name) => self.subject.exposes_part_to(node, name, host),
            _ => Ok(true),
        }
    }

    pub(crate) fn matches_node(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        id: SelectorNodeID,
        node: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        let tree = self.subject.tree();
        // A shadow root is a node of the tree, which is what makes a combinator walking up out of
        // the tree stop at it rather than continue into the document. It is not an element, though:
        // it publishes no facts, and nothing matches it - not even `*`. The one exception is
        // `:host`, which names the host standing outside the tree, so a walk that reaches the root
        // crosses there and nowhere else.
        //
        // The only shadow root a walk from inside the tree can reach is the scope's own, so this
        // costs one comparison rather than a lookup.
        if Some(node) == self.bindings.scope_shadow_root {
            return match program.node(id) {
                SelectorOp::Host(inner) => match tree.host_of(node) {
                    Some(host) => self.matches_host_argument(program, inner, host, counters),
                    None => Ok(false),
                },
                // A scoping root outside the tree is the host, and a combinator reaching up out of
                // the tree lands here rather than on it. `:scope > .a` inside a scope rooted at the
                // host is the same walk as `:host > .a`, so it crosses the same way.
                SelectorOp::ScopeRootInstance => {
                    let host = tree.host_of(node);
                    Ok(host.is_some() && self.bindings.scope_root_instance == host)
                }
                SelectorOp::IsNode(named) => {
                    Ok(tree.host_of(node).is_some_and(|host| self.subject.is_node(host, named)))
                }
                // An in-shadow-tree query walks its axis from the shadow root and binds its anchor to
                // it, so a chain tying itself back to that anchor - the leftmost step of
                // `:host:has(> .child > .grand_child)` asks for the `.child`'s parent - compares
                // against the root itself. It does not cross to the host the way a scoping root does,
                // because the root is where the walk started.
                SelectorOp::RelativeAnchorInstance => {
                    counters.bump(Counter::StructuralTests);
                    Ok(self.bindings.relative_anchor == Some(node))
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
                    tree.host_of(node).map_or(Ok(false), |host| {
                        self.matches_featureless_host(program, id, host, counters)
                    })
                }
                _ => Ok(false),
            };
        }
        match program.node(id) {
            SelectorOp::Feature(test) => {
                counters.bump(Counter::LocalFeatureTests);
                self.matches_feature(program, test, node)
            }
            SelectorOp::Language { first, count } => {
                counters.bump(Counter::StateTests);
                let row = self.subject.row(node)?;
                let mut ranges = program.language_ranges(first, count);
                // An element with no resolved language matches no range at all, not even `*`.
                Ok(match self.subject.language_tag(row) {
                    TokenizerInput::Ascii(tag) => {
                        !tag.is_empty() && ranges.any(|range| language_range_matches_tag(range, tag))
                    }
                    TokenizerInput::Utf16(tag) => {
                        !tag.is_empty() && ranges.any(|range| language_range_matches_tag(range, tag))
                    }
                })
            }
            SelectorOp::State(fact) => {
                counters.bump(Counter::StateTests);
                let row = self.subject.row(node)?;
                Ok(self.subject.has_state(row, fact))
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
                match tree.parent(node) {
                    Some(parent) => self.matches_relation_target(program, inner, parent, counters),
                    None => Ok(false),
                }
            }
            SelectorOp::Ancestor(inner) => {
                if self.subject.remembers_relations(&self.bindings) {
                    return self.matches_descendant_relation(program, id, inner, node, counters);
                }
                let mut ancestor = tree.parent(node);
                while let Some(current) = ancestor {
                    counters.bump(Counter::CombinatorSteps);
                    if self.relation_target_may_match(program, inner, current)
                        && self.matches_relation_target(program, inner, current, counters)?
                    {
                        return Ok(true);
                    }
                    ancestor = tree.parent(current);
                }
                Ok(false)
            }
            SelectorOp::PreviousSibling(inner) => {
                counters.bump(Counter::CombinatorSteps);
                match tree.previous_sibling(node) {
                    Some(previous) => self.matches_relation_target(program, inner, previous, counters),
                    None => Ok(false),
                }
            }
            SelectorOp::PrecedingSibling(inner) => self.matches_preceding_sibling(program, id, inner, node, counters),
            SelectorOp::NthPosition(position) => {
                // An `of S` answer depends on what the argument matches, which the bindings decide.
                let memoizes_answers = position.of_selector.is_none();
                if memoizes_answers && let Some(answer) = self.subject.positional_answer(position, node) {
                    return Ok(answer);
                }
                counters.bump(Counter::StructuralTests);
                let result = self.matches_nth(program, position, node, counters);
                if memoizes_answers && let Ok(answer) = result {
                    self.subject.record_positional_answer(position, node, answer);
                }
                result
            }
            // Each shadow operator consumes the relation it names. A generic descendant walk does
            // not pierce a shadow root, and a slot's assignment is not its DOM parent, so these
            // cannot be expressed as ordinary combinators.
            SelectorOp::Host(inner) => match self.node_hosts_the_scope(node) && !self.bindings.matching_host_argument {
                true => self.matches_host_argument(program, inner, node, counters),
                false => Ok(false),
            },
            SelectorOp::Slotted(inner) => match tree.assigned_slot_of(node) {
                Some(_) => self.matches_node(program, inner, node, counters),
                None => Ok(false),
            },
            SelectorOp::AssignedSlot(inner) => {
                // The chain can pass through several trees; the slot this compound describes is the
                // one in the tree whose rules are being asked.
                const MAX_REASSIGNMENTS: usize = 32;
                let mut current = node;
                for _ in 0..MAX_REASSIGNMENTS {
                    let Some(slot) = tree.assigned_slot_of(current) else {
                        return Ok(false);
                    };
                    // The slot this compound describes is the one in the tree being asked. A shadow
                    // root's own tree scope is the outer one, so which tree a node is in is answered
                    // by walking to the root rather than by comparing scopes.
                    let in_this_tree = self.bindings.scope_shadow_root.is_none_or(|root| {
                        std::iter::successors(Some(slot), |&node| tree.live_parent(node)).any(|node| node == root)
                    });
                    if in_this_tree && self.matches_node(program, inner, slot, counters)? {
                        return Ok(true);
                    }
                    current = slot;
                }
                Ok(false)
            }
            SelectorOp::Part(part) => {
                let row = self.subject.row(node)?;
                Ok(self.subject.has_part(row, part))
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
                let scope_host = self.bindings.scope_shadow_root.and_then(|root| tree.host_of(root));
                let mentions_the_host = program.mentions_the_host(host_compound);
                for level_host in self.subject.part_exposure_hosts(node) {
                    let reaches_a_hosted_tree = tree.shadow_host_of(level_host) == scope_host;
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
                Ok(self.subject.is_root(node))
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
                let enclosing_root = self.bindings.scope_root_instance;

                // A part stands inside a shadow tree, but an outer scope reaches it through the
                // host exposing it. Scope membership is therefore measured from that host rather
                // than from the part's DOM parent chain, which stops at the shadow root.
                if program.subject_is_a_part(inner)
                    && let Some(level_hosts) = self.subject.part_hosts_in_exposure_scope(node)
                {
                    for level_host in level_hosts {
                        let mut candidate = match names_the_scope {
                            true => Some(level_host),
                            false => tree.live_parent(level_host),
                        };
                        while let Some(root_candidate) = candidate {
                            if self.matches_in_scope_rooted_at(
                                program,
                                root,
                                limit,
                                inner,
                                node,
                                root_candidate,
                                counters,
                            )? {
                                return Ok(true);
                            }
                            if Some(root_candidate) == enclosing_root {
                                break;
                            }
                            candidate = tree.live_parent(root_candidate);
                        }
                    }
                    return Ok(false);
                }

                let mut candidate = match names_the_scope {
                    true => Some(node),
                    false => tree.live_parent(node),
                };
                // A shadow root is not an element and roots nothing. The host standing outside the
                // tree can be the scoping root, though - `@scope (:host)` says so, and an `@scope`
                // with no `<scope-start>` whose `<style>` is a direct child of the shadow root roots
                // there too - so the walk crosses at the root and stops.
                if candidate == self.bindings.scope_shadow_root {
                    candidate = candidate.and_then(|root| tree.host_of(root));
                }
                while let Some(root_candidate) = candidate {
                    if self.matches_in_scope_rooted_at(program, root, limit, inner, node, root_candidate, counters)? {
                        return Ok(true);
                    }
                    if Some(root_candidate) == enclosing_root {
                        break;
                    }
                    candidate = tree.live_parent(root_candidate);
                    if candidate == self.bindings.scope_shadow_root {
                        candidate = candidate.and_then(|root| tree.host_of(root));
                    }
                }
                Ok(false)
            }
            SelectorOp::Empty => {
                counters.bump(Counter::StructuralTests);
                self.subject.is_empty(node)
            }
            SelectorOp::IsNode(named) => {
                counters.bump(Counter::StructuralTests);
                Ok(self.subject.is_node(node, named))
            }
            SelectorOp::ScopeRootInstance => {
                counters.bump(Counter::StructuralTests);
                Ok(self.bindings.scope_root_instance == Some(node))
            }
            SelectorOp::RelativeAnchorInstance => {
                counters.bump(Counter::StructuralTests);
                Ok(self.bindings.relative_anchor == Some(node))
            }
            // Without an enclosing `@scope`, the scoping root is the root of the tree.
            SelectorOp::Scope => {
                counters.bump(Counter::StructuralTests);
                Ok(tree.live_parent(node).is_none())
            }
            SelectorOp::ValueState { kind, value } => {
                counters.bump(Counter::StateTests);
                let row = self.subject.row(node)?;
                Ok(match kind {
                    ValueStateTestKind::Directionality => self.subject.directionality_is(row, value),
                    ValueStateTestKind::CustomState => self.subject.has_custom_state(row, value),
                })
            }
            SelectorOp::Heading(levels) => {
                counters.bump(Counter::StructuralTests);
                let row = self.subject.row(node)?;
                let level = self.subject.heading_level(row);
                Ok((1..=9).contains(&level) && levels & (1 << (level - 1)) != 0)
            }
            // The existential answer: does any candidate on the query's axis satisfy its compound.
            // Only the Boolean matters, so the walk stops at the first witness it finds.
            SelectorOp::RelativeExists(query_id) => {
                counters.bump(Counter::RelationalTests);
                if let Some(answer) = self.subject.relative_answer(program, query_id, node) {
                    return Ok(answer);
                }
                let query = program.relative_query(query_id);
                // An in-shadow-tree query is anchored on the host and walked from the tree the host
                // opens. Binding the anchor to the shadow root as well as walking from it is what
                // ties a multi-compound chain back to the right place: the leftmost step of
                // `:host:has(> .child > .grand_child)` asks for the `.child`'s parent, and that is
                // the root, not the host.
                let anchor = match query.match_in_shadow_tree {
                    true => tree.shadow_root_of(node),
                    false => Some(node),
                };
                let Some(anchor) = anchor else {
                    return Ok(false);
                };
                let mut matched = Ok(false);
                let mut found = None;
                let enclosing_anchor = self.bindings.relative_anchor.replace(anchor);
                tree.for_each_on_axis(
                    query.axis,
                    query.witness_is_below_the_axis,
                    anchor,
                    |candidate| match self.matches_node(program, query.compound, candidate, counters) {
                        Ok(true) => {
                            matched = Ok(true);
                            found = Some(candidate);
                            false
                        }
                        Ok(false) => true,
                        Err(incomplete) => {
                            matched = Err(match query.axis {
                                RelativeAxis::Descendant => incomplete.in_descendant_scan(anchor, candidate),
                                RelativeAxis::FollowingSibling => incomplete.in_sibling_scan(candidate, None),
                                _ => incomplete,
                            });
                            false
                        }
                    },
                );
                self.bindings.relative_anchor = enclosing_anchor;
                // A walk that ended incomplete proved nothing either way.
                if let Ok(answer) = matched {
                    self.subject
                        .record_relative_answer(program, query_id, node, answer, found);
                }
                matched
            }
        }
    }

    fn matches_feature(&mut self, program: &SelectorProgram<S::Atoms>, test: FeatureTest, node: S::Node) -> Answer<S> {
        if matches!(test, FeatureTest::AnyElement) {
            return Ok(true);
        }
        let row = self.subject.row(node)?;
        Ok(matches_feature(
            &self.subject.features(row),
            test,
            |test, attribute, insensitive| self.matches_attribute_value(program, test, attribute, insensitive),
        ))
    }

    fn matches_attribute_value(
        &self,
        program: &SelectorProgram<S::Atoms>,
        test: AttributeTest,
        attribute: S::Attribute,
        insensitive: bool,
    ) -> bool {
        if test.operator == AttributeOperator::Presence {
            return true;
        }
        // An exact test on two interned values is an integer comparison, which is why the engine
        // carries no text for attributes only tested this way.
        if test.operator == AttributeOperator::Exact && !insensitive && !test.value_atom.is_none() {
            let value = self.subject.attribute_value_atom(attribute);
            if !value.is_none() {
                return test.value_atom == value;
            }
        }

        let literal = program.literal(test.value_offset, test.value_length);
        match self.subject.attribute_value_text(attribute) {
            Some(TokenizerInput::Ascii(value)) => attribute_value_matches(test.operator, value, literal, insensitive),
            Some(TokenizerInput::Utf16(value)) => attribute_value_matches(test.operator, value, literal, insensitive),
            None => false,
        }
    }

    pub(crate) fn matches_nth(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        position: NthPosition,
        node: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        if position.of_selector.is_none()
            && let Some(index) = self.subject.sibling_index(position, node)?
        {
            return Ok(matches_an_plus_b(position.step, position.offset, index));
        }
        // What S matches depends on the bindings, so its counts can only be kept while they are the ones every
        // count was made under.
        if let Some(selector) = position.of_selector
            && S::REMEMBERS_SIBLING_COUNTS_OF_SELECTORS
            && self.subject.remembers_relations(&self.bindings)
        {
            if !self.matches_node(program, selector, node, counters)? {
                return Ok(false);
            }
            let index = self.count_siblings_matching(program, selector, position.from_end, node, counters)?;
            return Ok(matches_an_plus_b(position.step, position.offset, i64::from(index)));
        }

        // https://drafts.csswg.org/selectors/#typedef-type-selector
        // An element's type is its qualified name, so two `p` elements in different namespaces are
        // different types and are counted in different sequences.
        let subject_type = match position.of_type {
            true => Some(self.subject.row(node)?),
            false => None,
        };

        // https://drafts.csswg.org/selectors/#child-index
        // A positional test counts the subject among its inclusive siblings. The root of a tree has none, which makes
        // it the one and only element of its sequence rather than absent from one: `:first-child`, `:last-child` and
        // `:only-child` all name it. The children of a document or a fragment have no parent element, but they are
        // siblings all the same.
        //
        // The subject has to be one of the counted siblings, or it has no position in the sequence.
        if !self.counts_in_sequence(program, position, subject_type, node, counters)? {
            return Ok(false);
        }
        let tree = self.subject.tree();

        // Count towards the near end only. The whole sequence is never needed, and a sequence of
        // thousands of siblings is what a long list or a table is, so materializing one per test
        // made `:first-child` cost the length of its parent's child list.
        let mut index: i64 = 1;
        let bounded = position.step == 0;
        let mut current = match position.from_end {
            true => tree.next_sibling(node),
            false => Some(tree.first_sibling(node)),
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
                Err(incomplete) => {
                    return Err(incomplete.in_sibling_scan(sibling, (!position.from_end).then_some(node)));
                }
            }
            current = tree.next_sibling(sibling);
        }
        Ok(matches_an_plus_b(position.step, position.offset, index))
    }

    /// How many siblings from the start of `node`'s sibling sequence (or from its end) through `node` match the
    /// selector at `selector`. A count walks only to the nearest sibling whose count the subject remembers, and the
    /// subject remembers the count of every sibling the walk passes, so counting every child of a parent walks its
    /// children once.
    fn count_siblings_matching(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        selector: SelectorNodeID,
        from_end: bool,
        node: S::Node,
        counters: &S::Counters,
    ) -> Result<u32, S::Incomplete> {
        let tree = self.subject.tree();
        let toward_start = |sibling| match from_end {
            true => tree.next_sibling(sibling),
            false => tree.previous_sibling(sibling),
        };
        let away_from_start = |sibling| match from_end {
            true => tree.previous_sibling(sibling),
            false => tree.next_sibling(sibling),
        };
        let mut count = 0;
        let mut sibling = node;
        let mut next = loop {
            if let Some(known) = self.subject.sibling_count_of(selector, from_end, sibling) {
                if sibling == node {
                    return Ok(known);
                }
                count = known;
                break away_from_start(sibling);
            }
            match toward_start(sibling) {
                Some(previous) => sibling = previous,
                None => break Some(sibling),
            }
        };
        while let Some(sibling) = next {
            count += u32::from(self.matches_node(program, selector, sibling, counters)?);
            self.subject.record_sibling_count_of(selector, from_end, sibling, count);
            if sibling == node {
                break;
            }
            next = away_from_start(sibling);
        }
        Ok(count)
    }

    /// Whether one sibling is counted by this positional test's sequence.
    fn counts_in_sequence(
        &mut self,
        program: &SelectorProgram<S::Atoms>,
        position: NthPosition,
        subject_type: Option<S::Row>,
        sibling: S::Node,
        counters: &S::Counters,
    ) -> Answer<S> {
        match (subject_type, position.of_selector) {
            (Some(subject_type), _) => {
                let row = self.subject.row(sibling)?;
                Ok(self.subject.same_type(row, subject_type))
            }
            (None, Some(selector)) => self.matches_node(program, selector, sibling, counters),
            (None, None) => Ok(true),
        }
    }
}
