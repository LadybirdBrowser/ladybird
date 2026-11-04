/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What selectors are matched against: a subject holding the elements and the facts a selector
//! tests, and the evaluator walking selector components over it.
//!
//! The style engine's subject reads its own storage, and the DOM query subject reads the live DOM.
//! Every matching rule lives in the evaluator, so two subjects can differ in what their storage
//! holds and never in what a selector means.
//!
//! A subject brings its accelerations along as hooks. A hook can only skip work: each default is
//! the answer the evaluation reaches without the shortcut.

use smallvec::SmallVec;

use super::index::StyleAtomID;
use super::input_types::StateFact;
use super::selector::NthPosition;
use super::selector::RelativeAxis;
use super::tree::StyleNodeID;
use crate::css::css_tokenizer::TokenizerInput;

/// Visit the candidate witnesses of a relative selector along `axis` from `anchor` in `tree`, in tree
/// order, until `visit` returns false.
pub(crate) fn walk_relative_axis<T: SelectorTree>(
    tree: T,
    axis: RelativeAxis,
    below_the_axis: bool,
    anchor: T::Node,
    mut visit: impl FnMut(T::Node) -> bool,
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
            visit_subtree(tree, anchor, false, &mut visit);
            return;
        }
        RelativeAxis::Child => (tree.first_child(anchor), true, false),
        RelativeAxis::NextSibling => (tree.next_sibling(anchor), false, false),
        RelativeAxis::FollowingSibling => (tree.next_sibling(anchor), true, false),
        // The witness lies under the sibling rather than being it, so the sibling's whole subtree is the
        // candidate range.
        RelativeAxis::NextSiblingSubtree => (tree.next_sibling(anchor), false, true),
        RelativeAxis::FollowingSiblingSubtree => (tree.next_sibling(anchor), true, true),
    };
    let mut sibling = first;
    while let Some(candidate) = sibling {
        let proceed = match subtree {
            true => visit_subtree(tree, candidate, !below_the_axis, &mut visit),
            false => visit(candidate),
        };
        if !proceed || !every_sibling {
            return;
        }
        sibling = tree.next_sibling(candidate);
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
    /// The parent in the tree as it stands now. Scope, slot and relational walks read it even where
    /// the combinators read an earlier state of the tree.
    fn live_parent(self, node: Self::Node) -> Option<Self::Node> {
        self.parent(node)
    }

    fn shadow_root_of(self, host: Self::Node) -> Option<Self::Node>;
    fn host_of(self, shadow_root: Self::Node) -> Option<Self::Node>;

    /// Whether the element stands for its shadow host's element-backed pseudo-element of `kind`.
    fn backs_pseudo_element(self, _node: Self::Node, _kind: u16) -> bool {
        false
    }

    /// The element of `host`'s shadow tree that stands for its element-backed pseudo-element of `kind`.
    fn backing_element_of(self, host: Self::Node, kind: u16) -> Option<Self::Node> {
        let shadow_root = self.shadow_root_of(host)?;
        let mut node = self.next_in_subtree(shadow_root, shadow_root);
        while let Some(current) = node {
            if self.backs_pseudo_element(current, kind) {
                return Some(current);
            }
            node = self.next_in_subtree(current, shadow_root);
        }
        None
    }

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

    /// The slot an element is assigned to.
    fn assigned_slot_of(self, _node: Self::Node) -> Option<Self::Node> {
        None
    }

    /// The host of the shadow tree a node is in.
    fn shadow_host_of(self, _node: Self::Node) -> Option<Self::Node> {
        None
    }
}

/// What a feature test reads of one element. Names are process atoms.
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

/// What a selector read of how an element stands among its siblings, or of whether it is empty: what a change to
/// its siblings or children can move the answer of.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum StructuralRead {
    /// The element's previous sibling, across a `+`.
    PreviousSibling,
    /// The element's earlier siblings, across a `~`.
    EarlierSiblings,
    /// Where the element stands among its siblings: counted from the end, among those of its type, and in a way that
    /// tells more than whether it stands first where `beyond_edge`.
    Position {
        from_end: bool,
        of_type: bool,
        beyond_edge: bool,
    },
    /// Whether the element is `:empty`.
    Emptiness,
    /// Where the element stands among its siblings, for a value of its style or of a pseudo-element's.
    SiblingPositionValue,
}

/// The elements a selector is matched against, and their facts.
///
/// Facts are read through a row, which a subject fetches once per element and the evaluator then
/// asks every test of. Names are process atoms. A subject answers what its
/// storage holds, and nothing else: which name, case or namespace a test compares is decided here.
pub(crate) trait SelectorSubject {
    type Node: Copy + Eq;
    type Tree: SelectorTree<Node = Self::Node>;
    type Row: Copy;
    type Attribute: Copy;
    type Features<'s>: ElementFeatures<Attribute = Self::Attribute>
    where
        Self: 's;
    type Incomplete: Copy;
    fn tree(&self) -> Self::Tree;

    fn row(&mut self, node: Self::Node) -> Result<Self::Row, Self::Incomplete>;
    /// Have the features of `row` answer for its attributes, which a test asks before it reads them.
    fn read_attributes(&mut self, _row: Self::Row) {}
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

    /// Whether the element carries the part name `part` in its own tree.
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

    /// Whether [`Self::may_match_required`] rejects elements, so that a walk up has the bits a compound requires
    /// worked out at all.
    const FILTERS_BY_REQUIRED_DISPATCH_BLOOM: bool = false;

    /// Whether `node` can possibly carry every dispatch key whose bloom bits `required` holds.
    fn may_match_required(&self, _required: u64, _node: Self::Node) -> bool {
        true
    }

    /// The element's 1-based index in the sequence a positional test with no `of S` counts.
    fn sibling_index(&mut self, _position: NthPosition, _node: Self::Node) -> Result<Option<i64>, Self::Incomplete> {
        Ok(None)
    }

    /// A remembered answer of whether an element relative to `anchor` matches the selector list `list`, the
    /// argument of a `:has()`, evaluated under `bindings`.
    fn relative_answer_of(
        &mut self,
        _list: SelectorListKey,
        _anchor: Self::Node,
        _bindings: &SelectorBindings<Self::Node>,
    ) -> Option<bool> {
        None
    }

    /// Whether the subject remembers what a walk up across a descendant combinator found.
    const REMEMBERS_ANCESTOR_ANSWERS: bool = false;

    /// Whether the subject remembers what a walk back across a subsequent-sibling combinator found.
    const REMEMBERS_SIBLING_ANSWERS: bool = false;

    /// What a walk remembers its answers in, found once per walk.
    type WalkAnswers: Copy;

    /// Where what walks from the compound `selector` names, under `bindings`, found is remembered, for a subject that
    /// remembers it.
    fn walk_answers(
        &mut self,
        _selector: SelectorKey,
        _bindings: &SelectorBindings<Self::Node>,
    ) -> Option<Self::WalkAnswers> {
        None
    }

    /// Whether `node`, or an element a walk goes on to from it, matches what `answers` are of, as a walk remembered it.
    fn walk_answer(&self, _answers: Self::WalkAnswers, _node: Self::Node) -> Option<bool> {
        None
    }

    /// Record in `answers` that each of `nodes`, or an element a walk goes on to from it, does or does not match.
    fn record_walk_answers(&mut self, _answers: Self::WalkAnswers, _nodes: &[Self::Node], _answer: bool) {}

    /// Note that a selector read `read` of `node`, for a subject that keeps what selectors read of the structure of
    /// its tree, whether or not the selector matched.
    fn note_structural_read(&self, _node: Self::Node, _read: StructuralRead) {}

    /// Whether the subject remembers the indices of elements among the siblings an `of S` list matches, which
    /// [`Self::of_index_of`] answers.
    fn remembers_of_indices(&self) -> bool {
        false
    }

    /// The 1-based index of `node`, which matches the selector list `list`, among its inclusive siblings that match
    /// it, counted from the end when `from_end`, as an earlier count remembered it.
    fn of_index_of(&self, _list: SelectorListKey, _from_end: bool, _node: Self::Node) -> Option<i64> {
        None
    }

    /// Record the index of `node` among its inclusive siblings that match `list`, as [`Self::of_index_of`] answers it.
    fn record_of_index(&mut self, _list: SelectorListKey, _from_end: bool, _node: Self::Node, _index: i64) {}

    /// Record the completed answer of a `:has()` argument list for `anchor`, evaluated under `bindings`.
    fn record_relative_answer_of(
        &mut self,
        _list: SelectorListKey,
        _anchor: Self::Node,
        _bindings: &SelectorBindings<Self::Node>,
        _answer: bool,
    ) {
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
    /// The scoping root of the innermost `@scope` level the last entry asked for matched in.
    pub(crate) matched_scope_root: Option<N>,
    /// Whether evaluation is inside the argument of a `:has()`.
    pub(crate) matching_relative_argument: bool,
}

pub(crate) use crate::css::selector_arena::SelectorKey;

pub(crate) type SelectorListKey = (
    crate::css::selector_arena::SelectorArenaSerial,
    crate::css::selector_arena::ListStart,
);

impl<N> Default for SelectorBindings<N> {
    fn default() -> Self {
        Self {
            scope_shadow_root: None,
            scope_root_instance: None,
            matching_host_argument: false,
            relative_anchor: None,
            matched_scope_root: None,
            matching_relative_argument: false,
        }
    }
}

/// Matches selectors against one subject.
pub(crate) struct SelectorEvaluator<S: SelectorSubject> {
    pub(crate) subject: S,
    pub(crate) bindings: SelectorBindings<S::Node>,
}
