/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The selector logical IR and its match program.
//!
//! Selectors compile into a compact logical IR that defines semantic equality independently of how
//! an operator is evaluated. Every selector then has two views of the same semantics: a match
//! program answering "does this candidate subject match", and a transpose program answering "which
//! subjects could have changed". This module owns the IR and the match program; the transpose
//! direction is built on the same nodes.
//!
//! The match program is exact and evaluates right to left. A complex selector's subject is its
//! rightmost compound, and every combinator becomes an operator constraining that subject
//! leftwards: `A B` is "the subject satisfies B, and some ancestor satisfies A". Compound operands
//! are ordered cheap and high-rejection first at compile time, so the common case is a failed
//! integer comparison rather than a tree walk.
//!
//! A compound with no relational operator allocates nothing per candidate: no witness, no context,
//! no summary, no engine node. That is the ordinary path, and it is the path that has to be fastest.
//!
//! Shadow, scope, pseudo-element, and relational operators are deliberately absent here. Each
//! arrives with the semantics it needs rather than as a placeholder that would have to be guessed
//! at now.

use super::capacity::capacity_bytes;
use super::column::Column;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::fast_hasher;
use super::index::DispatchKey;
use super::index::StyleAtomID;
use super::index::StyleNodeFacts;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use std::cell::Cell;
use std::cell::RefCell;
use std::hash::Hash;
use std::hash::Hasher;
use std::num::NonZeroU32;
use std::rc::Rc;

use super::TransactionFactSide;
use super::TransactionFactView;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::partial_view::Lookup;
use super::program::RuleID;
use super::program::SelectorProgramID;
use super::relative_selector::RelationalWitnessKey;
use super::relative_selector::RelationalWitnesses;
use super::relative_selector::RelativeAxis;
use super::relative_selector::RelativeQuery;
use super::relative_selector::RelativeQueryID;
use super::relative_selector::candidate_witnesses;
use super::relative_selector::traversal_anchor;
use super::transaction::StateFact;
use super::tree::PseudoElementTarget;
use super::tree::StyleNodeID;
use super::tree::StyleNodeTree;
use super::tree::TreeScopeID;
pub use crate::css::selector::Specificity;

#[cfg(feature = "style-recording")]
pub mod replay;
#[cfg(not(feature = "style-recording"))]
pub mod replay {
    use super::SelectorProgram;
    use crate::css::style::record_replay::PayloadWriter;

    pub fn write(_program: &SelectorProgram, _payload: &mut PayloadWriter) {}
}

define_id! {
    /// Index into one program's node arena.
    pub struct SelectorNodeID(pub);
}

/// How an attribute selector compares its value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum AttributeOperator {
    /// `[attr]`
    Presence,
    /// `[attr=value]`
    Exact,
    /// `[attr~=value]`
    Includes,
    /// `[attr|=value]`
    DashMatch,
    /// `[attr^=value]`
    Prefix,
    /// `[attr$=value]`
    Suffix,
    /// `[attr*=value]`
    Substring,
}

impl AttributeOperator {
    /// Whether the operator can be answered by comparing interned identities. Everything else has
    /// to read the value as text, which is why the batch only carries text for those attributes.
    #[must_use]
    pub fn is_answerable_by_atom(self) -> bool {
        matches!(self, Self::Presence | Self::Exact)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum AttributeCase {
    Sensitive,
    /// ASCII case-insensitive, whether from the `i` flag or from a language-defined rule.
    Insensitive,
    /// ASCII case-insensitive for an element in this namespace and case-sensitive for any other.
    ///
    /// https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
    /// A handful of attribute names compare their values ASCII case-insensitively, but only on an
    /// HTML element in an HTML document, so which rule applies is a fact about the subject rather
    /// than about the selector. The namespace is HTML's, and only in an HTML document: outside one
    /// the compiler emits `Sensitive` and this never appears.
    InsensitiveForNamespace(StyleAtomID),
}

/// One attribute test. The literal lives in the program: as an atom when it was worth interning,
/// and always as text when the operator cannot be answered by identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct AttributeTest {
    /// The atom the test names: an attribute's qualified form for `[ns|x]`, its bare local name for
    /// `[x]`, and the any-namespace form for `[*|x]`.
    pub name: StyleAtomID,
    /// Whether `name` is the any-namespace form, in which case it is matched against an attribute's
    /// shared `local` rather than against its own name. `[*|x]` names one attribute per namespace the
    /// element carries `x` in, and holds when any of them satisfies the test.
    pub any_namespace: bool,
    /// The ASCII-lowercase folding of `name`, equal to it when the name is already lowercase.
    ///
    /// An attribute name is matched case-insensitively against an HTML element in an HTML document
    /// and case-sensitively everywhere else, so which form applies is a property of the subject. Both
    /// are carried in one test for the same reason `TagTest` does it: a disjunction of two attribute
    /// tests would widen the enclosing compound's dispatch key to universal.
    pub folded: StyleAtomID,
    /// The namespace an element has to be in for `folded` to apply - the HTML namespace in an HTML
    /// document - or `NONE` where nothing folds.
    pub fold_in_namespace: StyleAtomID,
    pub operator: AttributeOperator,
    pub value_atom: StyleAtomID,
    pub value_offset: u32,
    pub value_length: u32,
    pub case: AttributeCase,
}

/// A type selector's name, in the form it was written and in its ASCII-lowercase folding.
///
/// Which of the two applies is a property of the element rather than of the selector: a type
/// selector matches an HTML element in an HTML document ASCII case-insensitively and everything
/// else case-sensitively. So `DIV` still has to reach `<div>`, while `foreignobject` must not reach
/// an SVG `foreignObject`. Carrying both forms in one test keeps that a single feature: expressing
/// it as a disjunction of two tag tests would widen the enclosing compound's dispatch key to
/// universal, and a compound that dispatches universally rejects nothing.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TagTest {
    pub written: StyleAtomID,
    /// Equal to `written` when the name is already ASCII-lowercase, which is the common case.
    pub folded: StyleAtomID,
    /// The namespace an element has to be in for the folded form to apply, or `NONE` where nothing
    /// folds. It is the HTML namespace in an HTML document and nothing at all in an XML one, where
    /// a type selector is case-sensitive however it was written.
    pub fold_in_namespace: StyleAtomID,
}

impl TagTest {
    /// A name that is its own folding.
    #[must_use]
    #[cfg(test)]
    pub fn exact(name: StyleAtomID) -> Self {
        Self {
            written: name,
            folded: name,
            fold_in_namespace: StyleAtomID::NONE,
        }
    }

    #[must_use]
    pub fn matches(self, tag: StyleAtomID, namespace: StyleAtomID) -> bool {
        if tag == self.written {
            return true;
        }
        tag == self.folded && !self.fold_in_namespace.is_none() && namespace == self.fold_in_namespace
    }
}

/// A test against one local fact of a candidate.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum FeatureTest {
    /// `*`
    AnyElement,
    /// A qualified name, as one interned tag/namespace atom.
    TagName(TagTest),
    Id(StyleAtomID),
    Class(StyleAtomID),
    Attribute(AttributeTest),
    /// What namespace the subject must be in. A type or universal selector written with a prefix
    /// carries one, and so does every one of them in a sheet that declared a default namespace.
    Namespace(NamespaceTest),
}

/// The namespace a qualified name constrains its subject to.
///
/// `*|x` places no constraint and compiles to nothing. The other two do, which is why they are the
/// only shapes here: a prefixed name names one namespace, and `|x` names the absence of one.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum NamespaceTest {
    /// `|x`, and `x` in a sheet with no default namespace declared: the subject is in no namespace.
    None,
    /// `ns|x`, and `x` in a sheet whose default namespace is `ns`.
    Named(StyleAtomID),
}

/// Where in a sibling sequence a subject can be, when a positional test bounds it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SubjectPosition {
    pub from_end: bool,
    /// How many positions from that end the subject can occupy. `u32::MAX` means unbounded.
    pub bound: u32,
}

impl SubjectPosition {
    pub const UNBOUNDED: Self = Self {
        from_end: false,
        bound: u32::MAX,
    };
}

/// An `an+b` positional test, optionally restricted to siblings matching a selector or sharing a
/// type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct NthPosition {
    pub step: i32,
    pub offset: i32,
    /// `:nth-last-child()` and `:nth-last-of-type()` count from the end.
    pub from_end: bool,
    /// `of <selector>`: only siblings matching this contribute to the count.
    pub of_selector: Option<SelectorNodeID>,
    /// `:nth-of-type()` family: only siblings sharing the subject's type contribute.
    pub of_type: bool,
}

/// A structural test a prefix compound can carry: truth is a function of the subject's tree
/// position or child content rather than its own interned facts, answered per node into the
/// automaton's truth bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PrefixStructuralTest {
    Nth(NthPosition),
    Empty,
}

impl NthPosition {
    /// Whether an element holding `count` matches this position's an+b test. The count must
    /// already be in this position's own terms: from the right end for `from_end`, over the
    /// qualified subsequence for `of_type` and `of_selector` forms.
    #[must_use]
    pub(super) fn matches_count(self, count: i64) -> bool {
        matches_an_plus_b(self.step, self.offset, count)
    }

    /// Whether shifting from `current` to any count in the inclusive old-count range could flip
    /// this position. Sequence routing retains only aggregate arrival and departure counts, so the
    /// range can be wider than the one count the element actually held but never narrower.
    #[must_use]
    pub(super) fn could_change_from_range(self, current: i64, first: i64, last: i64) -> bool {
        let current_matches = matches_an_plus_b(self.step, self.offset, current);
        match current_matches {
            true => !an_plus_b_matches_every_count(self.step, self.offset, first, last),
            false => an_plus_b_matches_any_count(self.step, self.offset, first, last),
        }
    }
}

fn an_plus_b_matches_any_count(step: i32, offset: i32, first: i64, last: i64) -> bool {
    debug_assert!(first <= last);
    let step = i64::from(step);
    let offset = i64::from(offset);
    match step.cmp(&0) {
        std::cmp::Ordering::Equal => (first..=last).contains(&offset),
        std::cmp::Ordering::Greater => {
            let n = if first <= offset {
                0
            } else {
                (first - offset + step - 1) / step
            };
            offset + step * n <= last
        }
        std::cmp::Ordering::Less => {
            if first > offset {
                return false;
            }
            let magnitude = -step;
            let upper = last.min(offset);
            let n = (offset - upper + magnitude - 1) / magnitude;
            offset - magnitude * n >= first
        }
    }
}

fn an_plus_b_matches_every_count(step: i32, offset: i32, first: i64, last: i64) -> bool {
    debug_assert!(first <= last);
    if first == last {
        return matches_an_plus_b(step, offset, first);
    }
    match step {
        1 => first >= i64::from(offset),
        -1 => last <= i64::from(offset),
        _ => false,
    }
}

/// One operator of the logical IR.
///
/// Combinators are expressed as constraints on the subject, which is the direction evaluation runs
/// in. `Ancestor(inner)` means "some ancestor of the subject satisfies inner", so the match
/// program for `A B` is `And[B, Ancestor(A)]`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SelectorOp {
    Feature(FeatureTest),
    State(StateFact),
    /// A compound, or an explicit conjunction. Operands are stored in a slice and ordered cheap
    /// first at compile time.
    And {
        first: u32,
        count: u32,
    },
    /// A selector list or `:is()`. Contributes the greatest specificity of its branches.
    Or {
        first: u32,
        count: u32,
    },
    /// `:where()`: evaluates its operand and contributes no specificity.
    Where(SelectorNodeID),
    Not(SelectorNodeID),
    /// `A > B`, evaluated from B.
    Parent(SelectorNodeID),
    /// `A B`, evaluated from B.
    Ancestor(SelectorNodeID),
    /// `A + B`, evaluated from B: the immediately preceding element sibling satisfies A.
    PreviousSibling(SelectorNodeID),
    /// `A ~ B`, evaluated from B: some preceding element sibling satisfies A.
    PrecedingSibling(SelectorNodeID),
    NthPosition(NthPosition),
    /// `:has()`: an existential query relating this subject, as anchor, to a witness.
    RelativeExists(RelativeQueryID),
    /// `:host` / `:host()`: the subject is the shadow host of the tree this rule is attached to,
    /// and satisfies the inner compound. Consumes the host relation, not DOM ancestry.
    Host(SelectorNodeID),
    /// `::slotted()`: the subject is assigned to a slot and satisfies the inner compound. Consumes
    /// the slot-assignment relation.
    Slotted(SelectorNodeID),
    /// `::part()`: the subject exposes the named part. Consumes the part-exposure relation.
    Part(StyleAtomID),
    /// `X::part(p)` from the part element: the shadow host of the tree this node is in satisfies
    /// `X`. A part compound describes two elements - the host and the element exposing the part -
    /// so the host half is a step, not a conjunct.
    ///
    /// The names are held here rather than beside this op because `exportparts` forwards a name
    /// outwards under a name of the host's choosing: each level of forwarding exposes its own names
    /// to its own host, so `p` and `X` only describe an element when one level answers both. Testing
    /// them as separate conjuncts would let a name from one level pair with a host from another.
    ExposedToHost {
        host: SelectorNodeID,
        parts: SelectorNodeID,
    },
    /// `:root`: the subject is the root of its tree.
    Root,
    /// `:empty`: the subject has no element or significant text children.
    Empty,
    /// `:scope`: the subject is the scoping root of the rule's `@scope`, or the document root.
    Scope,
    /// `slot::slotted(x)` from the slotted element: the slot it is assigned to satisfies the
    /// compound written before `::slotted()`.
    ///
    /// That compound describes the slot and not the subject - the subject is the element the slot
    /// stands in for - so it is a step, not a conjunct. A slot is itself a slottable, so the step
    /// follows the whole assignment chain and takes the slot that is in the tree being asked.
    AssignedSlot(SelectorNodeID),
    /// `:scope` inside a `<scope-end>`: the subject is the scoping root of the scope instance the
    /// limit is being checked against.
    ///
    /// One `@scope` builds one scope per element its `<scope-start>` matches, and a limit is
    /// relative to each of them separately. Compiling `:scope` to the root selector would ask
    /// whether *some* element matching it stands there, which excludes an element from a scope its
    /// own root does not exclude it from.
    ScopeRootInstance,
    /// The subject is the anchor of the relational query being evaluated.
    ///
    /// A relative selector's chain is compiled as constraints on the witness, which cannot say on
    /// its own where the chain started: `:has(+ div > .test)` would accept any `.test` whose parent
    /// is a `div` rather than one whose parent is the anchor's next sibling. Ending the chain here
    /// ties its leftmost step back to the anchor, which is what makes the query exact.
    RelativeAnchorInstance,
    /// The subject is one particular element, named by identity rather than by any feature.
    ///
    /// An `@scope` written without a `<scope-start>` roots at the parent of its sheet's owner node,
    /// which is an element and not a selector. Compiling it to an identity test is what lets the
    /// implied `:scope ` prefix, the proximity count and the limits all fall out of the machinery a
    /// written root already uses.
    IsNode(StyleNodeID),
    /// The subject is in one scope an `@scope` opens, and satisfies `inner` under that scope.
    ///
    /// A scope is not a constraint the subject can carry on its own. One `@scope` builds one scope
    /// per element its `<scope-start>` matches, and everything the rule says is relative to *one* of
    /// them: `:scope` names that element, a limit excludes relative to it, and proximity counts to
    /// it. So this walks the subject's inclusive ancestors for a root, binds each one in turn, and
    /// asks `inner` under the binding - which is the specification's "for each element matched by
    /// `<scope-start>`, create a scope using that element as the scoping root".
    ///
    /// `names_the_scope` says whether the subject may be the root itself. A scoped selector carries
    /// an implied `:scope ` prefix unless it names `:scope`, so it usually cannot.
    InScope {
        root: SelectorNodeID,
        /// The `<scope-end>`, when the scope has one. Nothing from the subject up to the root may
        /// match it.
        limit: Option<SelectorNodeID>,
        inner: SelectorNodeID,
        names_the_scope: bool,
    },
    /// A state whose value is a parameter rather than a boolean: `:dir()`, `:state()`.
    ValueState {
        kind: ValueStateTestKind,
        value: StyleAtomID,
    },
    /// `:lang()`: the subject's resolved language tag matches one of the extended language ranges
    /// named here. A range is not a name - `en` matches `en-GB` - so it cannot be an atom, and the
    /// ranges live in the program as literals compared against the tag itself.
    Language {
        first: u32,
        count: u32,
    },
    /// `:heading()`: a bitmask of the heading levels the subject may have, bit 0 being level 1.
    ///
    /// Levels run to nine, because a heading counts the heading offset its ancestors declare, and
    /// bare `:heading` is every one of them rather than the six an element can be written as.
    Heading(u16),
}

/// A selector state test whose value is an interned atom.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum ValueStateTestKind {
    /// `:dir()`
    Directionality,
    /// `:state()`, a custom element's state.
    CustomState,
}

impl ValueStateTestKind {
    fn routing_kind(self) -> ValueStateKind {
        match self {
            Self::Directionality => ValueStateKind::Directionality,
            Self::CustomState => ValueStateKind::CustomState,
        }
    }
}

/// A value-state input category used by routing.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum ValueStateKind {
    /// `:dir()`
    Directionality,
    /// `:lang()`. Every `:lang()` routes under this one key, because a range is not a name: this
    /// kind names the input, not a value, and no selector operator carries it.
    Language,
    /// `:state()`, a custom element's state.
    CustomState,
}

impl SelectorOp {
    /// A static cost rank used to order compound operands. Lower runs first, so a failed integer
    /// comparison rejects a candidate before any tree walk starts.
    #[must_use]
    fn cost_rank(self) -> u8 {
        match self {
            Self::Feature(FeatureTest::Id(_)) => 0,
            Self::Feature(FeatureTest::TagName(_)) => 1,
            Self::Feature(FeatureTest::Class(_)) => 2,
            // A handful of short subtag comparisons against a tag the element already carries.
            Self::Language { .. } => 6,
            Self::Feature(FeatureTest::AnyElement) => 3,
            // An ancestor walk with a nested evaluation at each step, so it runs after everything a
            // lookup can reject.
            Self::InScope { .. } => 15,
            // One integer comparison against a fact the element always carries.
            Self::Feature(FeatureTest::Namespace(_)) => 2,
            Self::Feature(FeatureTest::Attribute(test)) => {
                if test.operator.is_answerable_by_atom() {
                    4
                } else {
                    5
                }
            }
            Self::State(_) => 6,
            Self::Not(_) | Self::Where(_) => 7,
            Self::And { .. } | Self::Or { .. } => 8,
            Self::NthPosition(_) => 9,
            // Shadow operators are a lookup in a sparse relation: cheap, but only meaningful once
            // the local compound has already accepted the candidate.
            Self::Part(_) => 5,
            Self::ExposedToHost { .. } => 7,
            Self::Root | Self::Empty | Self::Scope | Self::ScopeRootInstance => 5,
            Self::IsNode(_) | Self::RelativeAnchorInstance => 0,
            Self::AssignedSlot(_) => 7,
            Self::ValueState { .. } | Self::Heading(_) => 6,
            Self::Slotted(_) => 6,
            Self::Host(_) => 11,
            // Relational queries traverse to answer, so they run after everything local.
            Self::RelativeExists(_) => 14,
            Self::PreviousSibling(_) => 10,
            Self::Parent(_) => 11,
            Self::PrecedingSibling(_) => 12,
            Self::Ancestor(_) => 13,
        }
    }
}

/// One top-level complex selector of a rule's selector list.
///
/// A pseudo-element target is metadata rather than an operator over the candidate. The entry's
/// program still matches the *originating element*; the target says where the resulting
/// declarations land. That is the whole projection: matching never has to consider pseudo identities
/// as candidates, and a rule targeting `::before` never applies to the element itself.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SelectorEntry {
    pub root: SelectorNodeID,
    pub specificity: Specificity,
    pub pseudo_element: Option<PseudoElementTarget>,
    /// What the rule's `@scope` names as a scoping root, when it has one. Which root a match
    /// resolved through decides its proximity, and the cascade compares that between two scoped
    /// declarations, so it is answered while matching rather than reconstructed afterwards.
    pub scope_root: Option<SelectorNodeID>,
    properties: SelectorEntryProperties,
}

/// One local compound in an ordinary descendant-or-child selector chain.
///
/// The compiled IR represents a combinator as one operand of the compound to its right. Keeping
/// the relation operand separate lets a top-down evaluator ask the local compound once and answer
/// the relation from the state inherited from its parent.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct SelectorPrefixLocal {
    pub(super) root: SelectorNodeID,
    relation: Option<SelectorNodeID>,
}

/// How one selector prefix is reached from the prefix immediately to its left.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) enum SelectorPrefixAxis {
    Root,
    Child,
    Descendant,
    /// The step's compound sits on the sibling immediately after its predecessor's.
    NextSibling,
    /// The step's compound sits on any sibling after its predecessor's.
    FollowingSibling,
}

/// One left-to-right step of an ordinary selector chain.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct SelectorPrefixStep {
    pub local: SelectorPrefixLocal,
    pub axis: SelectorPrefixAxis,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
struct SelectorEntryProperties {
    monotone_under_arrivals: bool,
    can_use_before_sibling_relations: bool,
    observes_sibling_relation: bool,
    has_prefix_chain: bool,
    prefix_chain_has_only_local_facts: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DispatchRelation {
    Subject,
    Parent,
    Ancestor,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DispatchQuery {
    Single,
    Alternatives,
    Required,
}

impl SelectorEntry {
    #[must_use]
    pub fn is_monotone_under_arrivals(self) -> bool {
        self.properties.monotone_under_arrivals
    }

    #[must_use]
    pub fn can_use_before_sibling_relations(self) -> bool {
        self.properties.can_use_before_sibling_relations
    }

    #[must_use]
    pub fn observes_sibling_relation(self) -> bool {
        self.properties.observes_sibling_relation
    }

    #[must_use]
    pub(super) fn has_prefix_chain(self) -> bool {
        self.properties.has_prefix_chain
    }

    #[must_use]
    pub(super) fn prefix_chain_has_only_local_facts(self) -> bool {
        self.properties.prefix_chain_has_only_local_facts
    }
}

/// A compiled selector program: one rule's selector list.
#[derive(Default, PartialEq, Eq, Hash)]
pub struct SelectorProgram {
    nodes: Vec<SelectorOp>,
    operands: Vec<SelectorNodeID>,
    text: Vec<u16>,
    entries: Vec<SelectorEntry>,
    relative_queries: Vec<RelativeQuery>,
    /// Ranges into `text`, one per extended language range a `:lang()` names.
    language_ranges: Vec<(u32, u32)>,
    /// Immutable dispatch analysis, retained so rebuilding scope dispatches does not repeatedly
    /// allocate the same per-entry key sets.
    subject_dispatch_keys: Vec<Box<[DispatchKey]>>,
    subject_required_keys: Vec<Box<[DispatchKey]>>,
    can_leave_scope: bool,
}

impl SelectorProgram {
    #[must_use]
    pub fn node(&self, id: SelectorNodeID) -> SelectorOp {
        self.nodes[id.0 as usize]
    }

    #[must_use]
    pub fn operands(&self, first: u32, count: u32) -> &[SelectorNodeID] {
        &self.operands[first as usize..(first + count) as usize]
    }

    #[must_use]
    pub fn entries(&self) -> &[SelectorEntry] {
        &self.entries
    }

    #[must_use]
    pub fn contains_relational_selector(&self) -> bool {
        self.nodes
            .iter()
            .any(|node| matches!(node, SelectorOp::RelativeExists(_)))
    }

    /// Whether any node in this subtree tests sibling position. A retained witness for a
    /// query containing one can go stale from a sibling mutation that never touches the
    /// witness itself, so such witnesses cannot prove an anchor unchanged.
    #[must_use]
    pub fn subtree_tests_position(&self, root: SelectorNodeID) -> bool {
        match self.node(root) {
            SelectorOp::NthPosition(_) => true,
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.subtree_tests_position(operand)),
            SelectorOp::Where(inner)
            | SelectorOp::Not(inner)
            | SelectorOp::Parent(inner)
            | SelectorOp::Ancestor(inner)
            | SelectorOp::PreviousSibling(inner)
            | SelectorOp::PrecedingSibling(inner) => self.subtree_tests_position(inner),
            SelectorOp::RelativeExists(query) => self.subtree_tests_position(self.relative_query(query).compound),
            _ => false,
        }
    }

    pub fn relative_query(&self, id: RelativeQueryID) -> RelativeQuery {
        self.relative_queries[id.0 as usize]
    }

    /// The query named by `id`, when a witness of it may be retained and later re-verified.
    ///
    /// Retention needs the query to be simple - one axis, one compound, a positive driving
    /// feature - and the compound to read only facts the witness itself publishes, so that a
    /// routing-time check can re-evaluate it with no scope, host, or anchor context. The bounds
    /// check stands in for identity churn: a retained key can outlive the program version that
    /// wrote it, and a key nothing answers for is not a proof of anything.
    #[must_use]
    pub(super) fn retainable_relative_query(&self, id: RelativeQueryID) -> Option<RelativeQuery> {
        let query = self.relative_queries.get(id.0 as usize).copied()?;
        (query.is_simple() && self.node_is_prefix_local(query.compound)).then_some(query)
    }

    #[must_use]
    pub fn literal(&self, offset: u32, length: u32) -> &[u16] {
        &self.text[offset as usize..(offset + length) as usize]
    }

    /// The extended language ranges one `:lang()` names.
    pub fn language_ranges(&self, first: u32, count: u32) -> impl Iterator<Item = &[u16]> {
        self.language_ranges[first as usize..(first + count) as usize]
            .iter()
            .map(|&(offset, length)| self.literal(offset, length))
    }

    #[must_use]
    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }

    /// The compact byte length of the program, which is what the document memory budget is written
    /// in. Allocator padding and optional acceleration are excluded on purpose.
    #[must_use]
    pub fn compact_bytes(&self) -> u64 {
        (self.nodes.len() * size_of::<SelectorOp>()
            + self.operands.len() * size_of::<SelectorNodeID>()
            + self.text.len() * size_of::<u16>()
            + self.entries.len() * size_of::<SelectorEntry>()
            + self.relative_queries.len() * size_of::<RelativeQuery>()
            + size_of::<bool>()) as u64
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.nodes,
                self.operands,
                self.text,
                self.entries,
                self.relative_queries,
                self.language_ranges,
                self.subject_dispatch_keys,
                self.subject_required_keys,
            ];
            cached [];
            nested [
                size_of::<bool>(),
                self.subject_dispatch_keys
                    .iter()
                    .map(|keys| size_of_val(keys.as_ref()) as u64)
                    .sum::<u64>(),
                self.subject_required_keys
                    .iter()
                    .map(|keys| size_of_val(keys.as_ref()) as u64)
                    .sum::<u64>(),
            ];
            skip [self.can_leave_scope];
        }
    }
}

/// Builds one selector program. C++ parses the selector text and drives this; the IR and its
/// ordering are decided here, while specificity comes from the immutable compiled selector.
#[derive(Default)]
pub struct SelectorProgramBuilder {
    program: SelectorProgram,
}

impl SelectorProgramBuilder {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The program as built so far, for a compiler that has to inspect what it just emitted.
    #[must_use]
    pub fn program(&self) -> &SelectorProgram {
        &self.program
    }

    pub fn push(&mut self, op: SelectorOp) -> SelectorNodeID {
        self.program.nodes.push(op);
        SelectorNodeID(u32::try_from(self.program.nodes.len() - 1).expect("selector node space exhausted"))
    }

    pub fn push_feature(&mut self, test: FeatureTest) -> SelectorNodeID {
        self.push(SelectorOp::Feature(test))
    }

    /// Add a UTF-16 literal to the program and return its range. The representation is the same one
    /// the C++ side holds, so nothing is converted crossing the boundary.
    pub fn push_literal(&mut self, text: &[u16]) -> (u32, u32) {
        let offset = u32::try_from(self.program.text.len()).expect("selector literal space exhausted");
        self.program.text.extend_from_slice(text);
        (
            offset,
            u32::try_from(text.len()).expect("selector literal space exhausted"),
        )
    }

    /// Add the extended language ranges of one `:lang()` and return their range.
    pub fn push_language_ranges(&mut self, ranges: &[&[u16]]) -> (u32, u32) {
        let first = u32::try_from(self.program.language_ranges.len()).expect("language range space exhausted");
        for range in ranges {
            let literal = self.push_literal(range);
            self.program.language_ranges.push(literal);
        }
        (
            first,
            u32::try_from(ranges.len()).expect("language range space exhausted"),
        )
    }

    /// Build a compound. Operands are sorted by static cost so evaluation rejects on the cheapest
    /// discriminating test it has.
    pub fn push_compound(&mut self, operands: &[SelectorNodeID]) -> SelectorNodeID {
        assert!(!operands.is_empty(), "a compound needs at least one operand");
        if operands.len() == 1 {
            return operands[0];
        }
        // Conjunction is associative, and one compound is one node: routing reads a compound's
        // operands to find what the selector says about the subject's parent, and an operand that
        // is itself a conjunction would hide the rest of the compound from the walk that reads it.
        let mut flattened: Vec<SelectorNodeID> = Vec::with_capacity(operands.len());
        for &operand in operands {
            match self.program.node(operand) {
                SelectorOp::And { first, count } => {
                    flattened.extend_from_slice(self.program.operands(first, count));
                }
                _ => flattened.push(operand),
            }
        }
        let mut sorted = flattened;
        sorted.sort_by_key(|&operand| self.program.node(operand).cost_rank());
        let first = u32::try_from(self.program.operands.len()).expect("selector operand space exhausted");
        self.program.operands.extend_from_slice(&sorted);
        self.push(SelectorOp::And {
            first,
            count: u32::try_from(sorted.len()).expect("selector operand space exhausted"),
        })
    }

    /// Add a relative query and return the operator that evaluates it.
    pub fn push_relative_exists(&mut self, query: RelativeQuery) -> SelectorNodeID {
        self.program.relative_queries.push(query);
        let id = RelativeQueryID(
            u32::try_from(self.program.relative_queries.len() - 1).expect("relative query space exhausted"),
        );
        self.push(SelectorOp::RelativeExists(id))
    }

    /// Build a disjunction: a selector list argument, or `:is()`.
    pub fn push_any_of(&mut self, operands: &[SelectorNodeID]) -> SelectorNodeID {
        assert!(!operands.is_empty(), "a disjunction needs at least one operand");
        // A disjunction of one is that one. It is worth collapsing rather than tidy, because
        // absolutizing a nested rule wraps the parent selector in `:is()`, so every `&` in every
        // stylesheet arrives here with a single branch - and a disjunction has no dispatch key, so
        // leaving it wrapped puts every nested rule in front of every element in the document.
        if let [only] = operands {
            return *only;
        }
        let first = u32::try_from(self.program.operands.len()).expect("selector operand space exhausted");
        self.program.operands.extend_from_slice(operands);
        self.push(SelectorOp::Or {
            first,
            count: u32::try_from(operands.len()).expect("selector operand space exhausted"),
        })
    }

    /// Add an ancestor step: the subject has an ancestor satisfying `inner`.
    pub fn push_ancestor(&mut self, inner: SelectorNodeID) -> SelectorNodeID {
        self.push(SelectorOp::Ancestor(inner))
    }

    /// Add a test nothing satisfies, which is what an unsatisfiable compound compiles to.
    ///
    /// It has to be a test rather than nothing at all: a compound with no operands constrains
    /// nothing, and a compound that constrains nothing matches every element in the document.
    pub fn push_never(&mut self) -> SelectorNodeID {
        let anything = self.push_feature(FeatureTest::AnyElement);
        self.push(SelectorOp::Not(anything))
    }

    /// Name one element by identity, which is how an `@scope` with no `<scope-start>` says where it
    /// roots.
    pub fn push_is_node(&mut self, node: StyleNodeID) -> SelectorNodeID {
        self.push(SelectorOp::IsNode(node))
    }

    /// Name what the entry's `@scope` treats as a scoping root.
    pub fn set_entry_scope_root(&mut self, entry: usize, scope_root: SelectorNodeID) {
        self.program.entries[entry].scope_root = Some(scope_root);
    }

    /// Replace an entry's root, as a scope does when it wraps everything the rule wrote.
    pub fn set_entry_root(&mut self, entry: usize, root: SelectorNodeID) {
        self.program.entries[entry].root = root;
    }

    /// The node an entry's evaluation starts from.
    #[must_use]
    pub fn entry_root(&self, entry: usize) -> SelectorNodeID {
        self.program.entries[entry].root
    }

    /// Add one top-level complex selector to the rule's selector list.
    pub fn push_entry(&mut self, root: SelectorNodeID) -> usize {
        self.push_entry_for_pseudo(root, None)
    }

    /// Add a top-level complex selector that targets a pseudo-element. The program still matches
    /// the originating element; the target projects the result onto the pseudo style node.
    pub fn push_entry_for_pseudo(
        &mut self,
        root: SelectorNodeID,
        pseudo_element: Option<PseudoElementTarget>,
    ) -> usize {
        self.program.entries.push(SelectorEntry {
            root,
            specificity: Specificity::default(),
            scope_root: None,
            pseudo_element,
            properties: SelectorEntryProperties::default(),
        });
        self.program.entries.len() - 1
    }

    /// Attach the specificity computed from the immutable compiled selector. IR rewrites for
    /// scopes and routing must not derive it again.
    pub fn set_entry_specificity(&mut self, entry: usize, specificity: Specificity) {
        self.program.entries[entry].specificity = specificity;
    }

    #[must_use]
    pub fn finish(mut self) -> SelectorProgram {
        let properties: Vec<SelectorEntryProperties> = self
            .program
            .entries
            .iter()
            .map(|entry| {
                let unified_chain = self.program.unified_chain(entry);
                SelectorEntryProperties {
                    monotone_under_arrivals: self.program.entry_is_monotone_under_arrivals(entry.root),
                    can_use_before_sibling_relations: self.program.entry_can_use_before_sibling_relations(entry.root),
                    observes_sibling_relation: self.program.entry_observes_sibling_relation(entry.root),
                    has_prefix_chain: unified_chain.is_some(),
                    prefix_chain_has_only_local_facts: unified_chain.as_ref().is_some_and(|chain| {
                        // A canonical positional step counts as local here: its truth rides the
                        // positional bits of every transition and completion key, so the retained
                        // walk answers it exactly like an interned fact.
                        chain
                            .iter()
                            .all(|step| self.program.prefix_local_has_canonical_form(step.local))
                    }),
                }
            })
            .collect();
        for (entry, properties) in self.program.entries.iter_mut().zip(properties) {
            entry.properties = properties;
        }
        self.program.can_leave_scope = self
            .program
            .entries
            .iter()
            .any(|entry| self.program.leaves_its_scope(entry.root));
        self.program.cache_subject_dispatch_analysis();
        self.program
    }
}

impl SelectorProgram {
    fn cache_subject_dispatch_analysis(&mut self) {
        let (subject_dispatch_keys, subject_required_keys): (Vec<_>, Vec<_>) = (0..self.entries.len())
            .map(|entry| {
                let dispatch = self.compute_subject_dispatch_keys(entry);
                let required = self.compute_subject_required_keys(entry, &dispatch);
                (dispatch.into_boxed_slice(), required.into_boxed_slice())
            })
            .unzip();
        self.subject_dispatch_keys = subject_dispatch_keys;
        self.subject_required_keys = subject_required_keys;
    }

    /// Decompose a selector whose tree relations are its linear chain over all four axes:
    /// descendant, child, adjacent, and general sibling. This is the form the prefix automaton
    /// registers.
    ///
    /// Logical operators are allowed inside one local compound as long as they stay local. A
    /// combinator hidden inside `:is()` or `:not()` is not a linear chain and remains on the exact
    /// match evaluator.
    #[must_use]
    pub(super) fn unified_chain(&self, entry: &SelectorEntry) -> Option<Vec<SelectorPrefixStep>> {
        let mut chain = Vec::new();
        self.append_chain(entry.root, &mut chain)?;
        Some(chain)
    }

    fn append_chain(&self, root: SelectorNodeID, chain: &mut Vec<SelectorPrefixStep>) -> Option<()> {
        let mut relation = None;
        let mut needs_canonical_form = false;
        let operands = match self.node(root) {
            SelectorOp::And { first, count } => self.operands(first, count),
            _ => std::slice::from_ref(&root),
        };
        for &operand in operands {
            match self.node(operand) {
                SelectorOp::Parent(inner) => {
                    if relation.replace((operand, SelectorPrefixAxis::Child, inner)).is_some() {
                        return None;
                    }
                }
                SelectorOp::Ancestor(inner) => {
                    if relation
                        .replace((operand, SelectorPrefixAxis::Descendant, inner))
                        .is_some()
                    {
                        return None;
                    }
                }
                SelectorOp::PreviousSibling(inner) => {
                    if relation
                        .replace((operand, SelectorPrefixAxis::NextSibling, inner))
                        .is_some()
                    {
                        return None;
                    }
                }
                SelectorOp::PrecedingSibling(inner) => {
                    if relation
                        .replace((operand, SelectorPrefixAxis::FollowingSibling, inner))
                        .is_some()
                    {
                        return None;
                    }
                }
                _ if self.node_is_prefix_local(operand) => {}
                // Anything else is admissible only through the canonical form below: structural
                // tests become truth bits, and everything the canonical collector rejects keeps
                // the compound on the exact match evaluator. The fallback program predicate
                // must never carry a structural test, because it would intern one answer per
                // fact cohort for truth that is per index or per child list.
                _ => {
                    needs_canonical_form = true;
                }
            }
        }

        let local = SelectorPrefixLocal {
            root,
            relation: relation.map(|(relation, _, _)| relation),
        };
        if needs_canonical_form && !self.prefix_local_has_canonical_form(local) {
            return None;
        }

        let axis = match relation {
            Some((_, axis, inner)) => {
                self.append_chain(inner, chain)?;
                axis
            }
            None => SelectorPrefixAxis::Root,
        };
        chain.push(SelectorPrefixStep { local, axis });
        Some(())
    }

    fn node_is_prefix_local(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .all(|&operand| self.node_is_prefix_local(operand)),
            SelectorOp::Where(inner) | SelectorOp::Not(inner) => self.node_is_prefix_local(inner),
            SelectorOp::Feature(_)
            | SelectorOp::State(_)
            | SelectorOp::Root
            | SelectorOp::ValueState { .. }
            | SelectorOp::Language { .. }
            | SelectorOp::Heading(_) => true,
            SelectorOp::Parent(_)
            | SelectorOp::Ancestor(_)
            | SelectorOp::PreviousSibling(_)
            | SelectorOp::PrecedingSibling(_)
            | SelectorOp::NthPosition(_)
            | SelectorOp::RelativeExists(_)
            | SelectorOp::Host(_)
            | SelectorOp::Slotted(_)
            | SelectorOp::Part(_)
            | SelectorOp::ExposedToHost { .. }
            | SelectorOp::Empty
            | SelectorOp::Scope
            | SelectorOp::IsNode(_)
            | SelectorOp::AssignedSlot(_)
            | SelectorOp::ScopeRootInstance
            | SelectorOp::RelativeAnchorInstance
            | SelectorOp::InScope { .. } => false,
        }
    }

    #[must_use]
    pub(super) fn prefix_dispatch_key(&self, local: SelectorPrefixLocal) -> DispatchKey {
        match self.node(local.root) {
            SelectorOp::And { first, count } => self
                .operands(first, count)
                .iter()
                .copied()
                .filter(|&operand| Some(operand) != local.relation)
                .map(|operand| self.dispatch_key_of(operand))
                .min_by_key(|key| dispatch_selectivity(*key))
                .unwrap_or(DispatchKey::Universal),
            _ => self.dispatch_key_of(local.root),
        }
    }

    /// The canonical feature list of one prefix step's local compound, plus at most one
    /// positional test. Positional truth is a pure function of the visit index and the
    /// sequence length, both of which the rightward walk knows, so a step carrying one stays
    /// automaton-expressible without entering state identity.
    ///
    /// Program-relative text offsets are irrelevant to presence tests and atom-exact comparisons,
    /// so clearing them lets equivalent compounds in different programs share one prefix step.
    pub(super) fn canonical_prefix_features_and_position(
        &self,
        local: SelectorPrefixLocal,
    ) -> Option<(Vec<FeatureTest>, Vec<PrefixStructuralTest>)> {
        let mut features = Vec::new();
        let mut structural_tests = Vec::new();
        let operands = match self.node(local.root) {
            SelectorOp::And { first, count } => self.operands(first, count),
            _ => std::slice::from_ref(&local.root),
        };
        for &operand in operands {
            if Some(operand) == local.relation {
                continue;
            }
            self.visit_canonical_prefix_operand(operand, &mut |feature| features.push(feature), &mut |test| {
                structural_tests.push(test);
            })?;
        }
        features.sort_unstable();
        features.dedup();
        Some((features, structural_tests))
    }

    fn prefix_local_has_canonical_form(&self, local: SelectorPrefixLocal) -> bool {
        let operands = match self.node(local.root) {
            SelectorOp::And { first, count } => self.operands(first, count),
            _ => std::slice::from_ref(&local.root),
        };
        for &operand in operands {
            if Some(operand) == local.relation {
                continue;
            }
            if self
                .visit_canonical_prefix_operand(operand, &mut |_| {}, &mut |_| {})
                .is_none()
            {
                return false;
            }
        }
        true
    }

    /// Fold one conjunctive prefix operand into the canonical form: feature tests answered
    /// from interned facts and structural tests answered as truth bits. `:where()` is
    /// match-transparent, and nested conjunctions arise from pseudo-classes that compile to
    /// several tests, `:only-of-type` being a first-of-type and a last-of-type in one. An
    /// an+b test with an of-selector evaluates an arbitrary program against every sibling,
    /// so it is not canonical, and neither is anything disjunctive or negated.
    fn visit_canonical_prefix_operand(
        &self,
        operand: SelectorNodeID,
        visit_feature: &mut impl FnMut(FeatureTest),
        visit_structural_test: &mut impl FnMut(PrefixStructuralTest),
    ) -> Option<()> {
        let feature = match self.node(operand) {
            SelectorOp::And { first, count } => {
                for &inner in self.operands(first, count) {
                    self.visit_canonical_prefix_operand(inner, visit_feature, visit_structural_test)?;
                }
                return Some(());
            }
            SelectorOp::Where(inner) => {
                return self.visit_canonical_prefix_operand(inner, visit_feature, visit_structural_test);
            }
            // Only step-free an+b tests are canonical: their truth flips at a bounded number
            // of positions per mutation, so the convergence walk maintains them cheaply. A
            // step-bearing test flips truth across a whole moved side at once, which
            // fragments transition states per index and displaces retained answers out of
            // Tier 3; it stays with the sequence router's moved-position narrowing until
            // transitions are flat-priced.
            SelectorOp::NthPosition(nth) if nth.of_selector.is_none() && nth.step == 0 => {
                visit_structural_test(PrefixStructuralTest::Nth(nth));
                return Some(());
            }
            SelectorOp::Empty => {
                visit_structural_test(PrefixStructuralTest::Empty);
                return Some(());
            }
            SelectorOp::Feature(feature) => feature,
            _ => return None,
        };
        let mut feature = feature;
        if let FeatureTest::Attribute(ref mut attribute) = feature {
            match attribute.operator {
                AttributeOperator::Presence => {
                    attribute.value_atom = StyleAtomID::NONE;
                    attribute.value_offset = 0;
                    attribute.value_length = 0;
                    attribute.case = AttributeCase::Sensitive;
                }
                AttributeOperator::Exact
                    if attribute.case == AttributeCase::Sensitive && !attribute.value_atom.is_none() =>
                {
                    attribute.value_offset = 0;
                    attribute.value_length = 0;
                }
                _ => return None,
            }
        }
        visit_feature(feature);
        Some(())
    }

    /// The rightmost distinguishing feature of one entry: the most selective local fact its subject
    /// compound requires.
    ///
    /// Only the subject's own features count. A combinator operand constrains an ancestor or a
    /// sibling, not the candidate, so bucketing by it would send candidates to rules that cannot
    /// match them and - worse - would miss candidates that can.
    #[must_use]
    pub fn dispatch_key(&self, entry: &SelectorEntry) -> DispatchKey {
        self.dispatch_key_of(entry.root)
    }

    /// The exact attribute value this entry's subject requires, when it requires one by identity.
    ///
    /// An attribute bucket holds every rule naming that attribute, and a page where one attribute
    /// is on every element hands the evaluator all of them. A rule that tests the value exactly and
    /// case-sensitively against an interned literal can be rejected by comparing two atoms, which
    /// is what this is for: it is only ever read for an entry whose dispatch key is that attribute.
    #[must_use]
    pub fn required_attribute_value(&self, entry: &SelectorEntry, name: StyleAtomID) -> StyleAtomID {
        self.required_attribute_value_of(entry.root, name)
    }

    fn required_attribute_value_of(&self, id: SelectorNodeID, name: StyleAtomID) -> StyleAtomID {
        match self.node(id) {
            SelectorOp::Feature(FeatureTest::Attribute(test))
                if test.name == name
                    && test.operator == AttributeOperator::Exact
                    && test.case == AttributeCase::Sensitive =>
            {
                test.value_atom
            }
            // Every operand of a conjunction has to hold, so a value one of them requires is
            // required. A disjunction or a negation requires nothing.
            SelectorOp::And { first, count } => self
                .operands(first, count)
                .iter()
                .map(|&operand| self.required_attribute_value_of(operand, name))
                .find(|value| !value.is_none())
                .unwrap_or(StyleAtomID::NONE),
            SelectorOp::Where(inner) => self.required_attribute_value_of(inner, name),
            _ => StyleAtomID::NONE,
        }
    }

    /// How far into a sibling sequence this entry's subjects can be.
    ///
    /// A positional test with no step names a bounded prefix or suffix of the sequence, which is
    /// the only thing left to narrow a child region by when the compound has no feature.
    #[must_use]
    pub fn subject_position(&self, entry: usize) -> SubjectPosition {
        self.position_of(self.entries()[entry].root)
    }

    fn position_of(&self, id: SelectorNodeID) -> SubjectPosition {
        match self.node(id) {
            SelectorOp::NthPosition(position) if position.step == 0 && position.offset >= 0 => SubjectPosition {
                from_end: position.from_end,
                bound: position.offset as u32,
            },
            SelectorOp::Where(inner) => self.position_of(inner),
            // Every operand of a conjunction is necessary, so the tightest bound any of them names
            // bounds the whole. Descending rather than reading one level is what keeps the bound
            // from depending on how deeply the compound happens to be nested.
            SelectorOp::And { first, count } => self
                .operands(first, count)
                .iter()
                .map(|&operand| self.position_of(operand))
                .min_by_key(|position| position.bound)
                .unwrap_or(SubjectPosition::UNBOUNDED),
            _ => SubjectPosition::UNBOUNDED,
        }
    }

    /// What the subject's parent must be, when the entry says so with a child combinator.
    ///
    /// A positional selector often has nothing on its subject to enumerate by - the user-agent
    /// sheet is full of `mfrac > :nth-child(2)` - but it does say what the parent is, and a whole
    /// sibling sequence shares one parent. So one lookup rejects the sequence outright.
    #[must_use]
    pub fn subject_parent_dispatch(&self, entry: usize) -> Vec<DispatchKey> {
        let mut keys = Vec::new();
        self.dispatch_analysis(
            self.entries()[entry].root,
            DispatchRelation::Parent,
            DispatchQuery::Alternatives,
            &mut keys,
        );
        keys
    }

    /// The one key the subject's parent must carry, when a child combinator names exactly one.
    ///
    /// A set of more than one key is a disjunction - `:is(ol, ul) > .item` names three - and the
    /// parent has to carry some member of it rather than any particular one, so no single key
    /// rejects soundly and the entry keeps no filter at all.
    #[must_use]
    pub fn subject_parent_dispatch_key(&self, entry: usize) -> Option<DispatchKey> {
        match self.subject_parent_dispatch(entry).as_slice() {
            [key] => Some(*key),
            _ => None,
        }
    }

    /// What some ancestor of the subject must be, when the entry says so with a descendant
    /// combinator.
    ///
    /// Weaker than the parent constraint and used only when there is none: it bounds the subject to
    /// a subtree rather than to a child list. `.preview :not(ol)` has nothing on its subject to
    /// enumerate by, but every element it can match is under a `.preview`.
    #[must_use]
    pub fn subject_ancestor_dispatch(&self, entry: usize) -> Vec<DispatchKey> {
        let mut keys = Vec::new();
        self.dispatch_analysis(
            self.entries()[entry].root,
            DispatchRelation::Ancestor,
            DispatchQuery::Alternatives,
            &mut keys,
        );
        keys
    }

    /// The one key some ancestor of the subject must carry. Disjunctions keep no filter, for the
    /// reason `subject_parent_dispatch_key` gives.
    #[must_use]
    pub fn subject_ancestor_dispatch_key(&self, entry: usize) -> Option<DispatchKey> {
        match self.subject_ancestor_dispatch(entry).as_slice() {
            [key] => Some(*key),
            _ => None,
        }
    }

    /// The witnesses of a relative query the subject has to satisfy, and the axis they stand on.
    ///
    /// A subject that is nothing but `:has(.error)` has no feature to enumerate by, but it can only
    /// match an anchor of that query - and the witness compound does have a posting. The axis is what
    /// turns a set of witnesses back into the set of anchors that can see them.
    ///
    /// Only a query the subject must satisfy bounds it. A negation says the opposite (an element with
    /// no `.error` under it is not bounded by the `.error` elements) and a disjunction is satisfied by
    /// branches this one knows nothing about, so neither is descended into.
    #[must_use]
    pub fn subject_relative_anchor(&self, entry: usize) -> Option<(RelativeAxis, Vec<DispatchKey>)> {
        let root = self.entries()[entry].root;
        let operands = match self.node(root) {
            SelectorOp::And { first, count } => self.operands(first, count),
            _ => std::slice::from_ref(&root),
        };
        for &operand in operands {
            let id = match self.node(operand) {
                SelectorOp::RelativeExists(id) => id,
                SelectorOp::Where(inner) => match self.node(inner) {
                    SelectorOp::RelativeExists(id) => id,
                    _ => continue,
                },
                _ => continue,
            };
            let query = self.relative_query(id);
            let mut keys = Vec::new();
            if !self.dispatch_keys_of(query.compound, &mut keys) {
                continue;
            }
            return Some((query.axis, keys));
        }
        None
    }

    /// Whether this entry's subject can only ever be the document's root element.
    ///
    /// `:root` has no feature to enumerate by, so it dispatches universally and a rule carrying it
    /// would otherwise be answered with the whole document - when it names exactly one element.
    #[must_use]
    pub fn subject_is_only_the_root(&self, entry: usize) -> bool {
        self.subject_is_the_document_root(self.entries()[entry].root)
    }

    fn subject_is_the_document_root(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Root => true,
            SelectorOp::Where(inner) => self.subject_is_the_document_root(inner),
            SelectorOp::And { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.subject_is_the_document_root(operand)),
            _ => false,
        }
    }

    /// The same, as the set of keys the subject can be reached by. Empty means it has none.
    #[must_use]
    pub fn subject_dispatch_keys(&self, entry: usize) -> &[DispatchKey] {
        &self.subject_dispatch_keys[entry]
    }

    fn compute_subject_dispatch_keys(&self, entry: usize) -> Vec<DispatchKey> {
        let mut keys = Vec::new();
        if !self.dispatch_keys_of(self.entries()[entry].root, &mut keys) {
            keys.clear();
        }
        keys
    }

    /// The independently necessary keys a compound requires beyond its candidate dispatch.
    ///
    /// Dispatch keys are a disjunction used to find candidates: either key of `:is(.a, .b)` admits
    /// one. A compound is also a conjunction, though, and every directly conjoined feature is a
    /// separate necessary condition. Retaining the remaining conditions lets transpose routing
    /// reject a `div` candidate for `span.target` after finding it through `.target`.
    ///
    /// Operators that cannot express one independently necessary key contribute nothing. That is
    /// the conservative direction: the candidate survives and the exact matcher settles it.
    #[must_use]
    pub fn subject_required_keys(&self, entry: usize) -> &[DispatchKey] {
        &self.subject_required_keys[entry]
    }

    fn compute_subject_required_keys(&self, entry: usize, dispatch: &[DispatchKey]) -> Vec<DispatchKey> {
        let mut keys = Vec::new();
        self.required_dispatch_keys_of(self.entries()[entry].root, &mut keys);
        if let [already_required] = dispatch {
            keys.retain(|key| key != already_required);
        }
        keys.sort_unstable();
        keys.dedup();
        keys.sort_unstable_by_key(|&key| dispatch_selectivity(key));
        keys.truncate(MAX_REQUIRED_DISPATCH_KEYS);
        keys
    }

    fn dispatch_analysis(
        &self,
        id: SelectorNodeID,
        relation: DispatchRelation,
        query: DispatchQuery,
        out: &mut Vec<DispatchKey>,
    ) -> bool {
        if relation == DispatchRelation::Subject && query == DispatchQuery::Single {
            out.push(self.single_dispatch_key_of(id));
            return true;
        }
        if relation != DispatchRelation::Subject {
            let operands = match self.node(id) {
                SelectorOp::And { first, count } => self.operands(first, count),
                _ => std::slice::from_ref(&id),
            };
            for &operand in operands {
                match (relation, self.node(operand)) {
                    (DispatchRelation::Parent, SelectorOp::Parent(inner)) => {
                        if self.mentions_the_host(inner) {
                            if query == DispatchQuery::Required {
                                continue;
                            }
                            return false;
                        }
                        return self.dispatch_analysis(inner, DispatchRelation::Subject, query, out);
                    }
                    (
                        DispatchRelation::Parent,
                        SelectorOp::PreviousSibling(inner) | SelectorOp::PrecedingSibling(inner),
                    ) => {
                        let start = out.len();
                        self.dispatch_analysis(inner, relation, query, out);
                        if query != DispatchQuery::Required && out.len() != start {
                            return true;
                        }
                    }
                    (DispatchRelation::Ancestor, SelectorOp::Parent(inner) | SelectorOp::Ancestor(inner))
                        if query == DispatchQuery::Required =>
                    {
                        if self.mentions_the_host(inner) {
                            continue;
                        }
                        self.dispatch_analysis(inner, DispatchRelation::Subject, query, out);
                        self.dispatch_analysis(inner, relation, query, out);
                    }
                    (DispatchRelation::Ancestor, SelectorOp::Ancestor(inner)) => {
                        if self.mentions_the_host(inner) {
                            return false;
                        }
                        return self.dispatch_analysis(inner, DispatchRelation::Subject, query, out);
                    }
                    (
                        DispatchRelation::Ancestor,
                        SelectorOp::Parent(inner)
                        | SelectorOp::PreviousSibling(inner)
                        | SelectorOp::PrecedingSibling(inner),
                    ) => {
                        let start = out.len();
                        self.dispatch_analysis(inner, relation, query, out);
                        if out.len() != start {
                            return true;
                        }
                    }
                    _ => {}
                }
            }
            return query == DispatchQuery::Required;
        }

        match self.node(id) {
            SelectorOp::Feature(test) => {
                let key = Self::dispatch_key_for_feature(test);
                match query {
                    DispatchQuery::Single => out.push(key),
                    DispatchQuery::Alternatives | DispatchQuery::Required if key == DispatchKey::Universal => {
                        return query == DispatchQuery::Required;
                    }
                    DispatchQuery::Alternatives | DispatchQuery::Required => out.push(key),
                }
                true
            }
            SelectorOp::And { first, count } => match query {
                DispatchQuery::Required => {
                    for &operand in self.operands(first, count) {
                        self.dispatch_analysis(operand, relation, query, out);
                    }
                    true
                }
                DispatchQuery::Single => {
                    let key = self
                        .operands(first, count)
                        .iter()
                        .map(|&operand| {
                            let mut candidate = Vec::new();
                            self.dispatch_analysis(operand, relation, query, &mut candidate);
                            candidate[0]
                        })
                        .min_by_key(|key| dispatch_selectivity(*key))
                        .unwrap_or(DispatchKey::Universal);
                    out.push(key);
                    true
                }
                DispatchQuery::Alternatives => {
                    let mut best: Option<Vec<DispatchKey>> = None;
                    let mut candidate = Vec::new();
                    for &operand in self.operands(first, count) {
                        candidate.clear();
                        if !self.dispatch_analysis(operand, relation, query, &mut candidate) {
                            continue;
                        }
                        if best
                            .as_ref()
                            .is_none_or(|current| dispatch_set_cost(&candidate) < dispatch_set_cost(current))
                        {
                            match &mut best {
                                Some(best) => std::mem::swap(best, &mut candidate),
                                None => best = Some(std::mem::take(&mut candidate)),
                            }
                        }
                    }
                    let Some(keys) = best else {
                        return false;
                    };
                    out.extend_from_slice(&keys);
                    true
                }
            },
            SelectorOp::Or { first, count } => match query {
                DispatchQuery::Single => {
                    out.push(DispatchKey::Universal);
                    true
                }
                DispatchQuery::Alternatives => {
                    let start = out.len();
                    for &operand in self.operands(first, count) {
                        if !self.dispatch_analysis(operand, relation, query, out)
                            || out.len() - start > MAX_DISPATCH_KEYS
                        {
                            out.truncate(start);
                            return false;
                        }
                    }
                    out[start..].sort_unstable();
                    let mut tail = out.split_off(start);
                    tail.dedup();
                    out.extend_from_slice(&tail);
                    true
                }
                DispatchQuery::Required => {
                    let mut keys = Vec::new();
                    if self.dispatch_analysis(id, relation, DispatchQuery::Alternatives, &mut keys) && keys.len() == 1 {
                        out.push(keys[0]);
                    }
                    true
                }
            },
            SelectorOp::Where(inner)
            | SelectorOp::Host(inner)
            | SelectorOp::Slotted(inner)
            | SelectorOp::ExposedToHost { parts: inner, .. } => {
                if query == DispatchQuery::Single
                    && matches!(self.node(id), SelectorOp::Host(_) | SelectorOp::Slotted(_))
                {
                    out.push(DispatchKey::Universal);
                    return true;
                }
                self.dispatch_analysis(inner, relation, query, out)
            }
            SelectorOp::InScope { inner, .. } if query != DispatchQuery::Required => {
                self.dispatch_analysis(inner, relation, query, out)
            }
            SelectorOp::Part(part) => {
                out.push(DispatchKey::Part(part));
                true
            }
            SelectorOp::ValueState {
                kind: ValueStateTestKind::CustomState,
                value,
            } => {
                out.push(DispatchKey::CustomState(value));
                true
            }
            SelectorOp::ValueState {
                kind: ValueStateTestKind::Directionality,
                value,
            } => {
                out.push(DispatchKey::Directionality(value));
                true
            }
            SelectorOp::Root if query == DispatchQuery::Single => {
                out.push(DispatchKey::Root);
                true
            }
            SelectorOp::State(state) if query == DispatchQuery::Single => {
                out.push(DispatchKey::State(state));
                true
            }
            SelectorOp::Heading(_) if query == DispatchQuery::Single => {
                out.push(DispatchKey::Heading);
                true
            }
            _ if query == DispatchQuery::Single => {
                out.push(DispatchKey::Universal);
                true
            }
            _ => query == DispatchQuery::Required,
        }
    }

    fn dispatch_key_of(&self, id: SelectorNodeID) -> DispatchKey {
        self.single_dispatch_key_of(id)
    }

    fn single_dispatch_key_of(&self, id: SelectorNodeID) -> DispatchKey {
        match self.node(id) {
            SelectorOp::Feature(test) => Self::dispatch_key_for_feature(test),
            SelectorOp::And { first, count } => self
                .operands(first, count)
                .iter()
                .map(|&operand| self.single_dispatch_key_of(operand))
                .min_by_key(|&key| dispatch_selectivity(key))
                .unwrap_or(DispatchKey::Universal),
            SelectorOp::Where(inner)
            | SelectorOp::InScope { inner, .. }
            | SelectorOp::ExposedToHost { parts: inner, .. } => self.single_dispatch_key_of(inner),
            SelectorOp::Part(part) => DispatchKey::Part(part),
            SelectorOp::ValueState {
                kind: ValueStateTestKind::CustomState,
                value,
            } => DispatchKey::CustomState(value),
            SelectorOp::ValueState {
                kind: ValueStateTestKind::Directionality,
                value,
            } => DispatchKey::Directionality(value),
            SelectorOp::Root => DispatchKey::Root,
            SelectorOp::State(state) => DispatchKey::State(state),
            SelectorOp::Heading(_) => DispatchKey::Heading,
            _ => DispatchKey::Universal,
        }
    }

    /// How many siblings back of this node's subject a leading adjacent chain reaches.
    ///
    /// Asked of a relative argument's compound, it counts the argument's own adjacent steps on top of
    /// the axis it starts on. A `~` step in that chain reaches every preceding sibling, so the answer
    /// is unbounded.
    fn adjacent_reach(&self, compound: SelectorNodeID) -> u32 {
        let operands = match self.node(compound) {
            SelectorOp::And { first, count } => self.operands(first, count),
            _ => std::slice::from_ref(&compound),
        };
        for &operand in operands {
            match self.node(operand) {
                SelectorOp::PreviousSibling(inner) => return self.adjacent_reach(inner).saturating_add(1),
                SelectorOp::PrecedingSibling(_) => return u32::MAX,
                _ => {}
            }
        }
        1
    }

    /// Whether the compound written before a `::part()` names the host of the tree its rule is in.
    ///
    /// A rule reaches the parts of the trees hosted below its own, and one written `:host::part(p)`
    /// reaches the parts of its own tree as well - those are the ones an inner tree forwarded out
    /// through `exportparts`. Which of the two a rule asks for is what its compound says.
    #[must_use]
    fn mentions_the_host(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Host(_) => true,
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.mentions_the_host(operand)),
            SelectorOp::Where(inner) | SelectorOp::Not(inner) => self.mentions_the_host(inner),
            _ => false,
        }
    }

    /// Whether satisfying this node requires the subject to carry a fact it would publish.
    ///
    /// Every fact an element has is published as it connects, so a compound resting on one is
    /// reached from that fact and needs nothing from the tree delta. A negation is satisfied by
    /// absence and `*` by existence alone; a step leads away from the subject and says nothing about
    /// it. A disjunction rests on a fact only when every branch does, since any branch can be the
    /// one that holds.
    fn tests_a_published_fact(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            // Neither is a fact an arrival can be recognised by. `*` names none, and a namespace
            // routes from nothing: an element's is fixed when it is created, so no input ever moves
            // it and no entry point registers under it.
            SelectorOp::Feature(FeatureTest::AnyElement | FeatureTest::Namespace(_)) => false,
            // A position is not a fact the element carries: the child sequence decides it, and an
            // element arriving publishes nothing that says where in one it landed.
            SelectorOp::NthPosition(_) => false,
            SelectorOp::State(fact) => state_is_published_on_arrival(fact),
            SelectorOp::IsNode(_) | SelectorOp::ScopeRootInstance | SelectorOp::RelativeAnchorInstance => false,
            // A scope bounds where the subject is; whether the subject is recognisable on arrival is
            // what the rule writes inside it.
            SelectorOp::InScope { inner, .. } => self.tests_a_published_fact(inner),
            SelectorOp::Feature(_)
            | SelectorOp::ValueState { .. }
            | SelectorOp::Language { .. }
            | SelectorOp::Heading(_)
            | SelectorOp::Part(_)
            | SelectorOp::Root
            | SelectorOp::Empty
            | SelectorOp::Scope => true,
            SelectorOp::And { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.tests_a_published_fact(operand)),
            SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .all(|&operand| self.tests_a_published_fact(operand)),
            SelectorOp::Where(inner) | SelectorOp::Host(inner) | SelectorOp::Slotted(inner) => {
                self.tests_a_published_fact(inner)
            }
            // The names were conjuncts of the enclosing compound before they moved under this op,
            // where a published fact anywhere in it answered for the whole.
            SelectorOp::ExposedToHost { host, parts } => {
                self.tests_a_published_fact(host) || self.tests_a_published_fact(parts)
            }
            SelectorOp::Not(_)
            | SelectorOp::Parent(_)
            | SelectorOp::Ancestor(_)
            | SelectorOp::PreviousSibling(_)
            | SelectorOp::PrecedingSibling(_)
            | SelectorOp::AssignedSlot(_)
            | SelectorOp::RelativeExists(_) => false,
        }
    }

    /// Whether nothing an element publishes as it arrives can turn this entry's answer off.
    ///
    /// An arriving element is routed from the facts it publishes, and for most shapes those facts
    /// can only add a match: the entry either did not hold before and holds now, or held all along.
    /// Three shapes break that and have to be answered on both sides like any other change. A
    /// negation is satisfied by the absence of what arrived; `:empty` stops holding on the parent an
    /// element arrives under; and a positional test moves under every element placed beside the one
    /// it counts. `:has()` is not one of them - a witness arriving can only add one - so its
    /// argument is descended into rather than refused.
    #[must_use]
    fn entry_is_monotone_under_arrivals(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Not(_) | SelectorOp::Empty | SelectorOp::NthPosition(_) => false,
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .all(|&operand| self.entry_is_monotone_under_arrivals(operand)),
            SelectorOp::Where(inner)
            | SelectorOp::Parent(inner)
            | SelectorOp::Ancestor(inner)
            | SelectorOp::PreviousSibling(inner)
            | SelectorOp::PrecedingSibling(inner)
            | SelectorOp::Host(inner)
            | SelectorOp::Slotted(inner)
            | SelectorOp::AssignedSlot(inner) => self.entry_is_monotone_under_arrivals(inner),
            SelectorOp::ExposedToHost { host, parts } => {
                self.entry_is_monotone_under_arrivals(host) && self.entry_is_monotone_under_arrivals(parts)
            }
            SelectorOp::RelativeExists(query) => {
                self.entry_is_monotone_under_arrivals(self.relative_query(query).compound)
            }
            // A scope's limit excludes what matches it, which is a negation by another name, and
            // which root a subject resolves through is what binds `:scope`. Neither is worth
            // proving here.
            SelectorOp::InScope { .. } => false,
            SelectorOp::Feature(_)
            | SelectorOp::State(_)
            | SelectorOp::Part(_)
            | SelectorOp::Root
            | SelectorOp::Scope
            | SelectorOp::ScopeRootInstance
            | SelectorOp::RelativeAnchorInstance
            | SelectorOp::IsNode(_)
            | SelectorOp::ValueState { .. }
            | SelectorOp::Language { .. }
            | SelectorOp::Heading(_) => true,
        }
    }

    /// Whether the transaction view carries every before-side tree relation this entry can observe.
    ///
    /// The before side reconstructs old parent and sibling relations, not departed subtrees, shadow
    /// relations, positional state, or scope instances. Restrict exact old-side evaluation to the
    /// operators whose complete inputs it therefore holds.
    #[must_use]
    fn entry_can_use_before_sibling_relations(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .all(|&operand| self.entry_can_use_before_sibling_relations(operand)),
            SelectorOp::Where(inner)
            | SelectorOp::Parent(inner)
            | SelectorOp::Ancestor(inner)
            | SelectorOp::PreviousSibling(inner)
            | SelectorOp::PrecedingSibling(inner) => self.entry_can_use_before_sibling_relations(inner),
            SelectorOp::NthPosition(position) => position
                .of_selector
                .is_none_or(|selector| self.entry_can_use_before_sibling_relations(selector)),
            SelectorOp::Feature(_)
            | SelectorOp::State(_)
            | SelectorOp::Root
            | SelectorOp::IsNode(_)
            | SelectorOp::Heading(_) => true,
            SelectorOp::Not(_)
            | SelectorOp::RelativeExists(_)
            | SelectorOp::Host(_)
            | SelectorOp::Slotted(_)
            | SelectorOp::Part(_)
            | SelectorOp::ExposedToHost { .. }
            | SelectorOp::Empty
            | SelectorOp::Scope
            | SelectorOp::AssignedSlot(_)
            | SelectorOp::ScopeRootInstance
            | SelectorOp::RelativeAnchorInstance
            | SelectorOp::ValueState { .. }
            | SelectorOp::Language { .. }
            | SelectorOp::InScope { .. } => false,
        }
    }

    #[must_use]
    fn entry_observes_sibling_relation(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.entry_observes_sibling_relation(operand)),
            SelectorOp::PreviousSibling(_) | SelectorOp::PrecedingSibling(_) => true,
            SelectorOp::Where(inner)
            | SelectorOp::Not(inner)
            | SelectorOp::Parent(inner)
            | SelectorOp::Ancestor(inner)
            | SelectorOp::Host(inner)
            | SelectorOp::Slotted(inner)
            | SelectorOp::AssignedSlot(inner) => self.entry_observes_sibling_relation(inner),
            SelectorOp::ExposedToHost { host, parts } => {
                self.entry_observes_sibling_relation(host) || self.entry_observes_sibling_relation(parts)
            }
            SelectorOp::RelativeExists(query) => {
                self.entry_observes_sibling_relation(self.relative_query(query).compound)
            }
            SelectorOp::NthPosition(_) => true,
            SelectorOp::InScope { root, limit, inner, .. } => {
                self.entry_observes_sibling_relation(inner)
                    || self.entry_observes_sibling_relation(root)
                    || limit.is_some_and(|limit| self.entry_observes_sibling_relation(limit))
            }
            SelectorOp::Feature(_)
            | SelectorOp::State(_)
            | SelectorOp::Part(_)
            | SelectorOp::Root
            | SelectorOp::Empty
            | SelectorOp::Scope
            | SelectorOp::ScopeRootInstance
            | SelectorOp::RelativeAnchorInstance
            | SelectorOp::IsNode(_)
            | SelectorOp::ValueState { .. }
            | SelectorOp::Language { .. }
            | SelectorOp::Heading(_) => false,
        }
    }

    /// Whether this node's subject can lie outside the scope its rule is attached to.
    ///
    /// `:host`, `::slotted()` and `::part()` are the constructs a scope reaches out of - a sheet in a
    /// shadow root styles its host and the nodes slotted into it through them, and nothing else. A
    /// program with none of them decides only inside its own scope, which is what lets a shadow
    /// sheet's rules be narrowed to the scope instead of to every element in the document carrying
    /// the same class.
    fn leaves_its_scope(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Host(_) | SelectorOp::Slotted(_) | SelectorOp::ExposedToHost { .. } => true,
            SelectorOp::Part(_) => true,
            SelectorOp::Language { .. } => false,
            SelectorOp::ScopeRootInstance | SelectorOp::RelativeAnchorInstance => false,
            // The step lands on the slot, which is inside the scope; what stands outside it is the
            // subject, and the `::slotted()` beside this says so.
            SelectorOp::AssignedSlot(_) => false,
            // An implicit scoping root is the parent of its sheet's owner node, which for a sheet
            // that is a direct child of a shadow root is the host - outside the tree. Which element
            // it is is not a property of the program, so this is the widest of the two answers and
            // the evaluation settles it.
            SelectorOp::IsNode(_) => true,
            // A scope can be rooted outside the tree its sheet is in - `@scope (:host)` roots at the
            // host - and then everything inside it decides for an element the scope does not hold.
            SelectorOp::InScope { root, limit, inner, .. } => {
                self.leaves_its_scope(inner)
                    || self.leaves_its_scope(root)
                    || limit.is_some_and(|limit| self.leaves_its_scope(limit))
            }
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.leaves_its_scope(operand)),
            SelectorOp::Where(inner)
            | SelectorOp::Not(inner)
            | SelectorOp::Parent(inner)
            | SelectorOp::Ancestor(inner)
            | SelectorOp::PreviousSibling(inner)
            | SelectorOp::PrecedingSibling(inner) => self.leaves_its_scope(inner),
            SelectorOp::RelativeExists(query) => self.leaves_its_scope(self.relative_query(query).compound),
            SelectorOp::NthPosition(position) => position
                .of_selector
                .is_some_and(|selector| self.leaves_its_scope(selector)),
            SelectorOp::Feature(_)
            | SelectorOp::State(_)
            | SelectorOp::Root
            | SelectorOp::Empty
            | SelectorOp::Scope
            | SelectorOp::ValueState { .. }
            | SelectorOp::Heading(_) => false,
        }
    }

    /// Whether any of this program's entries can decide outside the scope it is attached to.
    #[must_use]
    pub fn can_leave_its_scope(&self) -> bool {
        self.can_leave_scope
    }

    /// Whether the host itself is a subject of this program, rather than only a step on the way to
    /// one. `:host` selects the host; `:host *` selects its descendants and says nothing about it.
    #[must_use]
    pub fn host_is_a_subject(&self) -> bool {
        self.entries().iter().any(|entry| self.subject_is_the_host(entry.root))
    }

    /// Whether this compound's own subject is the shadow host. Combinators are not descended into: a
    /// `:host` beneath one constrains something else, not the subject.
    pub fn subject_is_the_host(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Host(_) => true,
            // An implicit scoping root can be the host, and nothing in the program says whether
            // this one is. Asking the host is the widest of the two answers.
            SelectorOp::IsNode(_) => true,
            // `:scope` is explicitly allowed to match a featureless host when that host is the
            // scoping root. The inner selector otherwise decides which element is the subject.
            SelectorOp::InScope {
                root,
                inner,
                names_the_scope,
                ..
            } => self.subject_is_the_host(inner) || (names_the_scope && self.subject_is_the_host(root)),
            // Every simple selector in a compound has to be allowed to match a featureless host.
            // `:has()` is the one exception: it is allowed when another simple selector in the
            // compound is allowed, so it does not disqualify an otherwise valid host compound.
            SelectorOp::And { first, count } => {
                let operands = self.operands(first, count);
                operands.iter().any(|&operand| {
                    !matches!(
                        self.node(operand),
                        SelectorOp::Feature(FeatureTest::AnyElement) | SelectorOp::RelativeExists(_)
                    ) && self.subject_is_the_host(operand)
                }) && operands.iter().all(|&operand| {
                    matches!(
                        self.node(operand),
                        SelectorOp::Feature(FeatureTest::AnyElement) | SelectorOp::RelativeExists(_)
                    ) || self.subject_is_the_host(operand)
                })
            }
            SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.subject_is_the_host(operand)),
            SelectorOp::Where(inner) | SelectorOp::Not(inner) => self.subject_is_the_host(inner),
            SelectorOp::ScopeRootInstance => true,
            _ => false,
        }
    }

    /// Whether this entry's own subject is a node slotted into the rule's tree.
    ///
    /// A shadow tree reaches a slotted element through `::slotted()` and through nothing else: its
    /// ordinary selectors decide inside the tree, and the slotted element is not in it. So a pass
    /// that asks a tree's rules of an element slotted into it asks only these.
    #[must_use]
    pub fn subject_is_slotted(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Slotted(_) => true,
            SelectorOp::InScope { inner, .. } => self.subject_is_slotted(inner),
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.subject_is_slotted(operand)),
            SelectorOp::Where(inner) | SelectorOp::Not(inner) => self.subject_is_slotted(inner),
            _ => false,
        }
    }

    /// Whether this entry's own subject is a part exposed out of the rule's tree.
    ///
    /// A scope outside a shadow tree reaches into it through `::part()` and through nothing else,
    /// so a pass that asks an outer scope's rules of a part asks only these.
    #[must_use]
    pub fn subject_is_a_part(&self, id: SelectorNodeID) -> bool {
        match self.node(id) {
            SelectorOp::Part(_) | SelectorOp::ExposedToHost { .. } => true,
            SelectorOp::InScope { inner, .. } => self.subject_is_a_part(inner),
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .any(|&operand| self.subject_is_a_part(operand)),
            SelectorOp::Where(inner) | SelectorOp::Not(inner) => self.subject_is_a_part(inner),
            _ => false,
        }
    }

    /// How many sibling relations reaching this node's subject passes through.
    ///
    /// Asked of a relative argument's compound, it says whether a change to a child sequence can flip
    /// the argument's truth for a witness that did not itself change - once the step the axis itself
    /// contributes is discounted. A nested `:has()` cannot occur, so its argument is not descended
    /// into.
    fn count_sibling_steps(&self, id: SelectorNodeID) -> usize {
        match self.node(id) {
            SelectorOp::PreviousSibling(inner) | SelectorOp::PrecedingSibling(inner) => {
                1 + self.count_sibling_steps(inner)
            }
            SelectorOp::Language { .. }
            | SelectorOp::IsNode(_)
            | SelectorOp::AssignedSlot(_)
            | SelectorOp::ScopeRootInstance
            | SelectorOp::RelativeAnchorInstance => 0,
            SelectorOp::InScope { inner, .. } => self.count_sibling_steps(inner),
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => self
                .operands(first, count)
                .iter()
                .map(|&operand| self.count_sibling_steps(operand))
                .max()
                .unwrap_or(0),
            SelectorOp::Where(inner)
            | SelectorOp::Not(inner)
            | SelectorOp::Parent(inner)
            | SelectorOp::Ancestor(inner)
            | SelectorOp::Host(inner)
            | SelectorOp::Slotted(inner) => self.count_sibling_steps(inner),
            SelectorOp::ExposedToHost { host, parts } => {
                self.count_sibling_steps(host).max(self.count_sibling_steps(parts))
            }
            SelectorOp::NthPosition(position) => position
                .of_selector
                .map_or(0, |selector| self.count_sibling_steps(selector)),
            SelectorOp::Feature(_)
            | SelectorOp::State(_)
            | SelectorOp::RelativeExists(_)
            | SelectorOp::Part(_)
            | SelectorOp::Root
            | SelectorOp::Empty
            | SelectorOp::Scope
            | SelectorOp::ValueState { .. }
            | SelectorOp::Heading(_) => 0,
        }
    }

    /// The set of dispatch keys a compound can be reached by, or `false` when no useful set exists.
    ///
    /// A single key cannot describe a disjunction. `:is(button, select):hover` is satisfied by
    /// either branch, so the only sound single key for it is the universal one - and a compound
    /// that dispatches universally rejects nothing, which is how a user-agent rule about buttons
    /// ends up routed from a hover on a paragraph. A set describes it exactly: an element carrying
    /// none of these keys cannot satisfy the compound.
    ///
    /// The set is bounded. Past [`MAX_DISPATCH_KEYS`] the membership checks cost more than the
    /// rejections save, and the compound is universal again.
    fn dispatch_keys_of(&self, id: SelectorNodeID, out: &mut Vec<DispatchKey>) -> bool {
        self.dispatch_analysis(id, DispatchRelation::Subject, DispatchQuery::Alternatives, out)
    }

    fn required_dispatch_keys_of(&self, id: SelectorNodeID, out: &mut Vec<DispatchKey>) {
        self.dispatch_analysis(id, DispatchRelation::Subject, DispatchQuery::Required, out);
    }

    fn dispatch_key_for_feature(test: FeatureTest) -> DispatchKey {
        dispatch_key_for_feature(test)
    }
}

/// The key a feature test dispatches on.
#[must_use]
fn dispatch_key_for_feature(test: FeatureTest) -> DispatchKey {
    match test {
        FeatureTest::Id(id) => DispatchKey::Id(id),
        FeatureTest::Class(class) => DispatchKey::Class(class),
        FeatureTest::Attribute(attribute) => DispatchKey::AttributeName(attribute.folded),
        // Dispatch on the folded form, because that is the one every element the test can match is
        // reachable by: an element's own facts carry both forms when they differ.
        FeatureTest::TagName(tag) => DispatchKey::TagName(tag.folded),
        // A namespace constrains a name rather than distinguishing an element, and every element
        // carries one, so it buckets with the universal selector and rejects in the compound.
        FeatureTest::AnyElement | FeatureTest::Namespace(_) => DispatchKey::Universal,
    }
}

/// The semantic input represented by a dispatch key, when one can move independently.
#[must_use]
fn routing_key_for_dispatch(key: DispatchKey) -> Option<RoutingKey> {
    match key {
        DispatchKey::TagName(tag) => Some(RoutingKey::TagName(tag)),
        DispatchKey::Id(id) => Some(RoutingKey::Id(id)),
        DispatchKey::Class(class) => Some(RoutingKey::Class(class)),
        DispatchKey::AttributeName(attribute) => Some(RoutingKey::AttributeName(attribute)),
        DispatchKey::State(state) => Some(RoutingKey::State(state)),
        DispatchKey::Part(part) => Some(RoutingKey::Part(part)),
        DispatchKey::CustomState(state) => Some(RoutingKey::ValueState(ValueStateKind::CustomState, state)),
        DispatchKey::Directionality(direction) => {
            Some(RoutingKey::ValueState(ValueStateKind::Directionality, direction))
        }
        DispatchKey::Root | DispatchKey::Heading => Some(RoutingKey::Structural),
        DispatchKey::Universal => None,
    }
}

/// How many keys a compound may carry before checking them all costs more than the rejections save.
const MAX_DISPATCH_KEYS: usize = 8;

/// How many independent compound requirements transpose routing retains.
const MAX_REQUIRED_DISPATCH_KEYS: usize = 8;

/// How much a whole key set costs to check, so the cheapest of a conjunction's operands wins.
fn dispatch_set_cost(keys: &[DispatchKey]) -> u32 {
    keys.iter().map(|&key| u32::from(dispatch_selectivity(key)) + 1).sum()
}

/// How selective a dispatch key is expected to be, lowest first. An ID names at most a handful of
/// elements; the universal bucket names all of them.
fn dispatch_selectivity(key: DispatchKey) -> u8 {
    match key {
        DispatchKey::Part(_) => 0,
        DispatchKey::CustomState(_) => 0,
        DispatchKey::Id(_) => 0,
        DispatchKey::Class(_) => 1,
        DispatchKey::AttributeName(_) => 2,
        DispatchKey::TagName(_) => 3,
        // A document usually resolves to one direction, so this names most of it.
        DispatchKey::Directionality(_) => 4,
        // These three are ranked behind every name a compound can carry, even though `:root` names
        // one element and most states name none. A name has a posting to enumerate from and these
        // do not, so preferring one would trade a tighter reaction batch for a dispatch that is
        // no more selective in practice: `a:hover` is as well found in the `a` bucket. What they
        // are for is the compound that carries no name at all, which is where a bare `:root` or
        // `:hover` rule would otherwise sit in front of every element in the document.
        DispatchKey::Root => 5,
        DispatchKey::State(_) => 5,
        DispatchKey::Heading => 5,
        DispatchKey::Universal => 6,
    }
}

/// The document's compiled selector programs.
///
/// Programs are Tier-2 shared semantic IR: bounded relative to the compact parsed stylesheet
/// program, and never a place for selector-result state to accumulate.
pub struct SelectorPrograms {
    programs: Vec<Option<SelectorProgram>>,
    vacant_programs: Vec<SelectorProgramID>,
    /// Open-addressed structural interning table. Reclamation rebuilds it, so lookup never has to
    /// carry tombstones for vacant program identities.
    program_index: Vec<Option<SelectorProgramID>>,
    /// What the programs themselves reserve, accumulated as they arrive. A program is immutable
    /// once compiled, so this is settled by the one place that can move it - which matters because
    /// compiling a rule settles the charge, and deriving it by walking the list would make a sheet
    /// cost a pass over every rule already in the document for each rule it adds.
    program_memory: MemoryLease,
    memory: MemoryLease,
}

impl Default for SelectorPrograms {
    fn default() -> Self {
        Self {
            programs: Vec::new(),
            vacant_programs: Vec::new(),
            program_index: Vec::new(),
            program_memory: MemoryLease::new(MemoryCategory::RuleProgram),
            memory: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

impl SelectorPrograms {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn add(&mut self, program: SelectorProgram) -> SelectorProgramID {
        self.add_with_status(program).0
    }

    pub(super) fn add_with_status(&mut self, program: SelectorProgram) -> (SelectorProgramID, bool) {
        let live_program_count = self.programs.len() - self.vacant_programs.len();
        if self.program_index.is_empty() || (live_program_count + 1) * 2 > self.program_index.len() {
            self.rebuild_program_index();
        }
        let mut bucket = Self::program_hash(&program) as usize & (self.program_index.len() - 1);
        loop {
            match self.program_index[bucket] {
                Some(id) if self.get(id) == &program => return (id, false),
                Some(_) => bucket = (bucket + 1) & (self.program_index.len() - 1),
                None => break,
            }
        }

        let id = self.vacant_programs.pop().unwrap_or_else(|| {
            SelectorProgramID(u32::try_from(self.programs.len()).expect("selector program space exhausted"))
        });
        self.program_memory.grow_committed(program.capacity_bytes());
        if id.0 as usize == self.programs.len() {
            self.programs.push(Some(program));
        } else {
            self.programs[id.0 as usize] = Some(program);
        }
        self.program_index[bucket] = Some(id);
        (id, true)
    }

    fn rebuild_program_index(&mut self) {
        let capacity = ((self.programs.len() + 1) * 4).next_power_of_two().max(16);
        self.program_index.clear();
        self.program_index.resize(capacity, None);
        for (index, program) in self
            .programs
            .iter()
            .enumerate()
            .filter_map(|(index, program)| program.as_ref().map(|program| (index, program)))
        {
            let id = SelectorProgramID(u32::try_from(index).expect("selector program space exhausted"));
            let mut bucket = Self::program_hash(program) as usize & (capacity - 1);
            while self.program_index[bucket].is_some() {
                bucket = (bucket + 1) & (capacity - 1);
            }
            self.program_index[bucket] = Some(id);
        }
    }

    fn program_hash(program: &SelectorProgram) -> u64 {
        let mut hasher = fast_hasher();
        program.hash(&mut hasher);
        hasher.finish()
    }

    #[must_use]
    pub fn get(&self, id: SelectorProgramID) -> &SelectorProgram {
        self.programs[id.0 as usize]
            .as_ref()
            .expect("a selector program identity must remain live while referenced")
    }

    pub fn sweep_unreferenced(&mut self, referenced: &[bool]) {
        self.vacant_programs.clear();
        for (index, slot) in self.programs.iter_mut().enumerate() {
            if referenced.get(index).copied().unwrap_or(false) {
                assert!(slot.is_some(), "a referenced selector program must remain live");
                continue;
            }
            if let Some(program) = slot.take() {
                self.program_memory.shrink_committed(program.capacity_bytes());
            }
            self.vacant_programs.push(SelectorProgramID(
                u32::try_from(index).expect("selector program space exhausted"),
            ));
        }
        self.rebuild_program_index();
    }

    #[must_use]
    pub(super) fn has_unreferenced_programs(&self, referenced: &[bool]) -> bool {
        self.programs
            .iter()
            .enumerate()
            .any(|(index, program)| program.is_some() && !referenced.get(index).copied().unwrap_or(false))
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.programs.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.programs.len() == self.vacant_programs.len()
    }

    /// Compact program bytes, which is the stylesheet term of the document memory budget. It
    /// deliberately measures the minimal encoding rather than the allocated capacity, so
    /// acceleration overhead can never inflate its own allowance.
    #[must_use]
    pub fn compact_bytes(&self) -> u64 {
        self.programs.iter().flatten().map(SelectorProgram::compact_bytes).sum()
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.programs, self.vacant_programs, self.program_index];
            cached [self.program_memory.bytes()];
            nested [];
            skip [self.memory];
        }
    }

    pub fn settle_memory(&mut self, memory: &mut MemoryController) {
        self.program_memory.settle_committed(memory);
        let header = self.capacity_bytes() - self.program_memory.bytes();
        self.memory.resize_required_to(memory, header);
    }
}

// -- Transpose direction -----------------------------------------------------------------------

/// One inverse relation step: from an element where a selector input occurs, towards the subjects
/// whose match truth that input can change.
///
/// A transpose program may over-approximate but must never omit a subject, so each step names the
/// widest set the corresponding combinator can reach. Resulting subjects are checked by the match
/// program before any selector-truth delta is emitted, which is what keeps over-approximation a
/// cost rather than a correctness problem.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum InverseStep {
    /// `A B` from A: descendants of the changed element.
    Descendants,
    /// `A > B` from A: element children of the changed element.
    Children,
    /// `A + B` from A: the immediately following element sibling.
    NextSibling,
    /// `A ~ B` from A: following element siblings.
    FollowingSiblings,
    /// A change inside a positional argument moves indices within the whole child sequence.
    SiblingSequence,
    /// From a possible `:has()` witness to its possible anchors: the element parent.
    AnchorParent,
    /// From a possible witness to its ancestors, up to the query's scope boundary.
    AnchorAncestors,
    /// From a possible witness to the immediately preceding element sibling.
    AnchorPreviousSibling,
    /// From a possible witness to the preceding element siblings in the same child sequence.
    AnchorPrecedingSiblings,
    /// From a shadow host to the tree it hosts. Deliberately not a descendant step: a generic
    /// descendant walk does not pierce a shadow root.
    HostedTree,
    /// From a shadow host to every tree nested below the one it hosts, shadow roots included.
    ///
    /// `exportparts` carries a part name outwards through any number of hosts, so the element a
    /// `::part()` rule names can sit several shadow roots below the host its outer compound
    /// describes - past boundaries `HostedTree` stops at.
    HostedTrees,
    /// From a slotted node to the slot it is assigned to, and thence into the shadow tree.
    SlotAssignment,
    /// From a slot to the light-DOM elements it assigns.
    SlotAssignees,
}

/// The semantic input a transpose route is routed from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RoutingKey {
    TagName(StyleAtomID),
    Id(StyleAtomID),
    Class(StyleAtomID),
    /// Attribute presence and value share one key, because one mutation changes both.
    AttributeName(StyleAtomID),
    State(StateFact),
    /// The entry depends on the shape of a child sequence rather than on any local fact.
    Structural,
    /// One exposed part name.
    Part(StyleAtomID),
    /// One parameterized state and the value it tests.
    ValueState(ValueStateKind, StyleAtomID),
}

/// How a possible witness reaches the anchors whose relational truth it can flip.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct RelativeAnchor {
    pub axis: RelativeAxis,
    /// Which of the program's relative queries this is, which is how a candidate anchor's
    /// retained witness is looked up before the anchor is routed.
    pub query: RelativeQueryID,
    /// The rightmost distinguishing feature of the compound the `:has()` sits in. An ancestor that
    /// does not carry it is not an anchor of this query, which is what keeps the anchor set small.
    pub anchor_dispatch: DispatchKey,
    /// The distinguishing feature of the query's own compound: what a witness of it must carry.
    pub witness_dispatch: DispatchKey,
    /// Whether the input occurs on the witness itself.
    ///
    /// The anchors are normally found by walking the inverse of the query's axis from the changed
    /// element, which only works while that element is a possible witness. `:has(#d:is(.a .b))`
    /// puts `.a` a step away from one, and that step can leave the anchor's subtree entirely - the
    /// element carrying `.a` is then an *ancestor* of the anchor, and no walk from it finds one.
    pub input_is_on_the_witness: bool,
    /// How many siblings back of a witness this query's anchor can be, on an adjacent axis.
    ///
    /// `X:has(+ B)` anchors on the element beside the witness and `X:has(+ A + B)` two beside it, so
    /// an element landing in a long sequence asks that many of the elements before it whether they
    /// are anchors, and not all of them. A `~` step anywhere in the chain unbounds the reach.
    pub adjacent_reach: u32,
    /// Whether the query's own compound has no fact for an arriving element to be named by.
    ///
    /// An element publishes every fact that holds on it as it connects - its tag, its id, its
    /// classes, its attributes, the states it arrives in - so a witness compound that tests any of
    /// those is reached from the fact. `:has(:not(.test))` and `:has(*)` test nothing an element can
    /// publish, and a query like that is invisible to an arrival unless the arrival is asked.
    pub witness_is_featureless: bool,
    /// Whether the argument reaches a witness across a sibling relation of its own, past the axis it
    /// starts on.
    ///
    /// `:has(#d:is(.p + .c ~ .d .e))` holds because of an adjacency two levels away from anything the
    /// query names, and any element landing in that sequence breaks it while carrying nothing at all.
    /// Such a query cannot be reached from the features of a tree change, so it is asked directly
    /// whenever a child sequence moves. An argument whose own steps are all ancestral needs none of
    /// that: an element arriving does not change what anything else is a descendant of.
    pub argument_spans_siblings: bool,
    /// See `RelativeQuery::match_in_shadow_tree`. The axis was walked from a shadow root, so walking
    /// it back from a witness arrives at that root and the anchor is whatever hosts it.
    pub match_in_shadow_tree: bool,
}

/// How to get from one semantic input in a selector entry to the subjects it can affect.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransposeRoute {
    pub program: SelectorProgramID,
    pub entry: u32,
    /// Whether the selector entry can be answered by the top-down prefix automaton.
    pub has_prefix_chain: bool,
    /// Whether every prefix compound reads only facts on its own element.
    pub prefix_chain_has_only_local_facts: bool,
    /// The IR node containing a structural operator, which distinguishes how the route is planned.
    pub structural_node: Option<SelectorNodeID>,
    /// Set when the input is a possible relational witness, in which case the path applies from an
    /// anchor rather than from the changed node.
    pub anchor: Option<RelativeAnchor>,
    path_offset: u32,
    path_length: u32,
    /// The distinguishing features of the compound the input occurs in.
    ///
    /// An input reaches this route only if the element it happened to also satisfies the rest
    /// of its own compound. Hovering a paragraph cannot change anything through `.action:hover *`,
    /// because the paragraph is not an `.action`, and rejecting that here costs one posting lookup
    /// instead of a subtree walk. Empty means the compound distinguishes nothing and rejects
    /// nothing.
    origin_offset: u32,
    origin_length: u32,
    /// Independently necessary features of the compound containing the input.
    origin_required_offset: u32,
    origin_required_length: u32,
    /// What the selector says the parent of that compound must be.
    ///
    /// A positional test names a place in a child sequence, and a whole sequence shares one parent,
    /// so `#list > .row:first-child .leaf` is rejected for every sequence whose parent is not the
    /// list - however far the subject is from the test.
    parent_offset: u32,
    parent_length: u32,
    /// The subject's own bounded position in its sibling sequence, when it has one.
    ///
    /// `:first-child` names one element of a sequence however long it is, and a compound with no
    /// feature to enumerate by has nothing else to narrow a child region with.
    subject_position: SubjectPosition,
    /// The subject's own distinguishing features.
    ///
    /// This is what turns a region into a candidate list. `.guard:hover .target` reaches a whole
    /// subtree through its combinator, but only the `.target` elements in that subtree can change,
    /// and these keys are how they are found without streaming the rest.
    subject_offset: u32,
    subject_length: u32,
    /// Independently necessary features of the subject compound.
    subject_required_offset: u32,
    subject_required_length: u32,
    /// One key per step of the path: what the node that step lands on must carry.
    ///
    /// A region names where a change can reach; it does not say what the selector required on the
    /// way. `[data-active] [class] span` reaches every `span` under the changed element, but only a
    /// `span` with a `[class]` ancestor can match it, and the rest are recomputed for nothing.
    /// Requiring each of these of some ancestor is a necessary condition of the selector, so it can
    /// only remove subjects that could not have matched either before or after the change.
    ///
    /// Empty unless every step of the path is ancestral, since a sibling or shadow step is not
    /// answered by walking a subject's ancestors. Compounds that distinguish nothing, and the ones
    /// whose set is wider than a single key, contribute no requirement rather than a loose one.
    waypoint_offset: u32,
    waypoint_length: u32,
}

/// Stable identity of one canonical transpose route.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RouteID(NonZeroU32);

impl RouteID {
    fn from_index(index: usize) -> Self {
        let one_based = u32::try_from(index)
            .expect("transpose route space exhausted")
            .checked_add(1)
            .expect("transpose route space exhausted");
        Self(NonZeroU32::new(one_based).unwrap())
    }

    pub(super) fn index(self) -> usize {
        (self.0.get() - 1) as usize
    }
}

/// The complete semantic identity of one transpose route.
///
/// All variable-length fields borrow the registry's canonical arenas or the compiler's temporary
/// slices. Looking up this value therefore allocates and copies nothing.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct RouteDescriptor<'a> {
    rule: RuleID,
    program: SelectorProgramID,
    entry: u32,
    structural_node: Option<SelectorNodeID>,
    subject_dispatch: &'a [DispatchKey],
    subject_required: &'a [DispatchKey],
    subject_position: SubjectPosition,
    origin_dispatch: &'a [DispatchKey],
    origin_required: &'a [DispatchKey],
    parent_dispatch: &'a [DispatchKey],
    waypoints: &'a [DispatchKey],
    anchor: Option<RelativeAnchor>,
    path: &'a [InverseStep],
}

impl SelectorProgram {
    /// Walk one entry and report every semantic input it mentions, with the inverse path from that
    /// input to the entry's subjects.
    ///
    /// The path is produced in application order: walking down the IR collects the combinators
    /// outermost first, and going back up applies them innermost first, so the collected steps are
    /// reversed before being handed out.
    pub fn collect_transpose_entry_points(&self, entry: usize, mut visit: impl FnMut(TransposeSite<'_>)) {
        let root = self.entries()[entry].root;
        let mut walk = TransposeWalk::default();
        self.walk_transpose(root, root, None, &mut walk, &mut visit);
    }

    fn walk_transpose(
        &self,
        id: SelectorNodeID,
        enclosing: SelectorNodeID,
        anchor: Option<(RelativeAnchor, usize)>,
        walk: &mut TransposeWalk,
        visit: &mut impl FnMut(TransposeSite<'_>),
    ) {
        let emit = |walk: &mut TransposeWalk, key: RoutingKey, visit: &mut dyn FnMut(TransposeSite<'_>)| {
            walk.origin_dispatch.clear();
            if !self.dispatch_keys_of(enclosing, &mut walk.origin_dispatch) {
                walk.origin_dispatch.clear();
            }
            walk.origin_required.clear();
            self.required_dispatch_keys_of(enclosing, &mut walk.origin_required);
            if let [already_required] = walk.origin_dispatch.as_slice() {
                walk.origin_required.retain(|key| key != already_required);
            }
            walk.origin_required.sort_unstable();
            walk.origin_required.dedup();
            walk.origin_required
                .sort_unstable_by_key(|&key| dispatch_selectivity(key));
            walk.origin_required.truncate(MAX_REQUIRED_DISPATCH_KEYS);
            let mut parent_dispatch = Vec::new();
            self.dispatch_analysis(
                enclosing,
                DispatchRelation::Parent,
                DispatchQuery::Alternatives,
                &mut parent_dispatch,
            );
            // Steps taken inside a relative argument lead from the anchor to a possible witness,
            // not from the anchor to the entry's subjects. Only the steps already on the stack when
            // the query was entered describe that, so the rest are dropped: keeping them would send
            // `.red:has(#d:is(.a .b))` looking for its own subject among its descendants.
            let depth = anchor.map_or(walk.path.len(), |(_, depth)| depth);
            walk.applied_path.clear();
            walk.applied_path.extend(walk.path[..depth].iter().rev().copied());
            let anchor = anchor.map(|(anchor, depth)| RelativeAnchor {
                input_is_on_the_witness: depth == walk.path.len(),
                ..anchor
            });
            // The waypoints were collected subject-first, like the path, and the last one is the
            // subject's own compound rather than an intermediate. Reversed alongside the path, each
            // one constrains the node the step of the same index lands on, and `Universal` means
            // that compound constrains nothing.
            walk.applied_waypoints.clear();
            if depth == walk.path.len() {
                walk.applied_waypoints.extend(
                    walk.waypoints[..depth]
                        .iter()
                        .rev()
                        .map(|key| key.unwrap_or(DispatchKey::Universal)),
                );
            }
            visit(TransposeSite {
                key,
                node: id,
                path: &walk.applied_path,
                anchor,
                origin_dispatch: &walk.origin_dispatch,
                origin_required: &walk.origin_required,
                parent_dispatch: &parent_dispatch,
                waypoints: &walk.applied_waypoints,
            });
        };

        match self.node(id) {
            SelectorOp::Feature(test) => {
                if let Some(key) = routing_key_for_dispatch(dispatch_key_for_feature(test)) {
                    emit(walk, key, visit);
                } else if anchor.is_some() {
                    // `*` names no fact, so as a subject it needs no key: the only thing that can
                    // change it is the element appearing or disappearing, which its own tree delta
                    // names. As the witness of a `:has()` it is not the element that moved - the
                    // anchor is somewhere above or beside it, and its answer flips the moment
                    // anything at all arrives. The shape of the tree is what moved, so that is what
                    // the query registers under, which is also the key the arrival is routed by.
                    emit(walk, RoutingKey::Structural, visit);
                }
            }
            SelectorOp::State(fact) => emit(walk, RoutingKey::State(fact), visit),
            SelectorOp::And { first, count } | SelectorOp::Or { first, count } => {
                // A compound naming `:host` is reached at the shadow root and describes the host one
                // crossing beyond it, so every other conjunct's facts sit on the host too and the way
                // back to the subject has to cross into the tree exactly as `:host` itself does.
                // Without it `:host:has(.x) .y` and `:host:hover .y` route from the host through its
                // light-DOM descendants, which hold no subject of theirs, and reach nothing.
                //
                // An empty path means the subject is the host, so there is nothing to cross.
                let crosses_to_the_host = matches!(self.node(id), SelectorOp::And { .. })
                    && !walk.path.is_empty()
                    && self
                        .operands(first, count)
                        .iter()
                        .any(|&operand| self.mentions_the_host(operand));
                for index in 0..count {
                    let operand = self.operands(first, count)[index as usize];
                    // Operands of a compound share it: it is the constraint their subject carries.
                    match crosses_to_the_host && !self.mentions_the_host(operand) {
                        // `:host` applies the step for its own operand already.
                        true => self.walk_step(operand, id, InverseStep::HostedTree, anchor, walk, visit),
                        false => self.walk_transpose(operand, id, anchor, walk, visit),
                    }
                }
            }
            SelectorOp::Where(inner) | SelectorOp::Not(inner) => {
                self.walk_transpose(inner, enclosing, anchor, walk, visit);
            }
            SelectorOp::Parent(inner) => self.walk_step(inner, enclosing, InverseStep::Children, anchor, walk, visit),
            SelectorOp::Ancestor(inner) => {
                self.walk_step(inner, enclosing, InverseStep::Descendants, anchor, walk, visit);
            }
            SelectorOp::PreviousSibling(inner) => {
                self.walk_step(inner, enclosing, InverseStep::NextSibling, anchor, walk, visit);
            }
            SelectorOp::PrecedingSibling(inner) => {
                self.walk_step(inner, enclosing, InverseStep::FollowingSiblings, anchor, walk, visit);
            }
            SelectorOp::NthPosition(position) => {
                // The position itself depends on the child sequence, not on any local fact.
                emit(walk, RoutingKey::Structural, visit);
                if let Some(selector) = position.of_selector {
                    self.walk_step(selector, enclosing, InverseStep::SiblingSequence, anchor, walk, visit);
                }
            }
            // A change to a possible witness does not fold into the path. Folding it would compose
            // "the ancestors of the changed node" with whatever the outer selector adds, and that
            // composition degenerates: ancestors-then-descendants is the document. The anchor step
            // is recorded separately so routing can resolve the anchors first and continue the
            // remaining path from each one that can actually flip.
            SelectorOp::RelativeExists(query) => {
                let compiled = self.relative_query(query);
                let anchor = RelativeAnchor {
                    axis: compiled.axis,
                    query,
                    witness_dispatch: self.dispatch_key_of(compiled.compound),
                    input_is_on_the_witness: true,
                    // The anchor has to satisfy the compound the `:has()` sits in, which is what
                    // separates the handful of real anchors from every ancestor of the witness.
                    anchor_dispatch: self.dispatch_key_of(enclosing),
                    adjacent_reach: self.adjacent_reach(compiled.compound),
                    witness_is_featureless: !self.tests_a_published_fact(compiled.compound),
                    // A witness under a sibling reaches its anchor through a sibling step inside the
                    // compound, which is the axis itself and not a step of the argument's own.
                    argument_spans_siblings: self.count_sibling_steps(compiled.compound)
                        > usize::from(matches!(
                            compiled.axis,
                            RelativeAxis::NextSiblingSubtree | RelativeAxis::FollowingSiblingSubtree
                        )),
                    match_in_shadow_tree: compiled.match_in_shadow_tree,
                };
                self.walk_transpose(
                    compiled.compound,
                    compiled.compound,
                    Some((anchor, walk.path.len())),
                    walk,
                    visit,
                );
            }
            // `:host()` is a guard on the host, not a combinator: the match program tests the
            // same node it was reached on. Which node that is depends on where the guard sits. In
            // `:host(.x)` the subject is the host itself, so a change to `.x` reaches the host and
            // no step applies. In `:host(.x) .y` a combinator has already been taken, and the
            // subject is inside the shadow tree, which a DOM-ancestry step cannot reach - so the
            // step to the hosted tree is what carries it there.
            SelectorOp::Host(inner) => {
                if walk.path.is_empty() {
                    self.walk_transpose(inner, enclosing, anchor, walk, visit);
                } else {
                    self.walk_step(inner, enclosing, InverseStep::HostedTree, anchor, walk, visit);
                }
            }
            // `::slotted()` likewise tests the assigned element itself, which is the subject.
            // A change on the slot reaches the elements assigned to it, which is the same relation
            // `::slotted()` steps through.
            SelectorOp::AssignedSlot(inner) => {
                self.walk_step(inner, enclosing, InverseStep::SlotAssignees, anchor, walk, visit);
            }
            SelectorOp::Slotted(inner) => {
                if walk.path.is_empty() {
                    self.walk_transpose(inner, enclosing, anchor, walk, visit);
                } else {
                    self.walk_step(inner, enclosing, InverseStep::SlotAssignment, anchor, walk, visit);
                }
            }
            SelectorOp::Part(part) => emit(walk, RoutingKey::Part(part), visit),
            // A change on the host reaches the elements exposing parts inside the trees it hosts.
            // `exportparts` forwards a name outwards, so the element the rule names can sit any
            // number of shadow roots below the host rather than in the one tree it hosts directly,
            // and the step has to pierce them all.
            //
            // The names are walked without a step: they are facts about the subject itself, and a
            // change to one reaches the rule from the element that publishes it.
            SelectorOp::ExposedToHost { host, parts } => {
                self.walk_step(host, enclosing, InverseStep::HostedTrees, anchor, walk, visit);
                self.walk_transpose(parts, enclosing, anchor, walk, visit);
            }
            // Root, emptiness, scope membership and the place of a named element all change with
            // the shape of the tree.
            SelectorOp::Root
            | SelectorOp::Empty
            | SelectorOp::Scope
            | SelectorOp::IsNode(_)
            | SelectorOp::ScopeRootInstance => {
                emit(walk, RoutingKey::Structural, visit);
            }
            // Which element is the anchor is not a fact anything publishes, and the relation the
            // chain steps through is not one an input moves: a mutation that reshapes it is a tree
            // delta, which routing sees through the relations it records rather than through a key.
            // The compounds of the chain carry the keys the query is reachable by.
            SelectorOp::RelativeAnchorInstance => {}
            SelectorOp::ValueState { kind, value } => {
                emit(walk, RoutingKey::ValueState(kind.routing_kind(), value), visit);
            }
            // The subject is reached through what the rule writes inside the scope, so that is the
            // path. An element becoming or ceasing to be a scoping root moves the scope of
            // everything below it, and an element becoming a scoping limit takes itself and
            // everything under it out - so both are walked through a descendant step, and the limit
            // as a subject as well, since it leaves the scope itself.
            SelectorOp::InScope { root, limit, inner, .. } => {
                self.walk_transpose(inner, enclosing, anchor, walk, visit);
                // The subject can be the root itself as well as below it, so an element becoming or
                // ceasing to be one reaches itself and its descendants.
                self.walk_transpose(root, enclosing, anchor, walk, visit);
                self.walk_step(root, enclosing, InverseStep::Descendants, anchor, walk, visit);
                if let Some(limit) = limit {
                    self.walk_transpose(limit, enclosing, anchor, walk, visit);
                    self.walk_step(limit, enclosing, InverseStep::Descendants, anchor, walk, visit);
                }
            }
            // A range is not a name, so every `:lang()` registers under one key and a resolved
            // language moving reaches all of them.
            SelectorOp::Language { .. } => emit(
                walk,
                RoutingKey::ValueState(ValueStateKind::Language, StyleAtomID::NONE),
                visit,
            ),
            SelectorOp::Heading(_) => emit(walk, RoutingKey::Structural, visit),
        }
    }

    fn walk_step(
        &self,
        inner: SelectorNodeID,
        enclosing: SelectorNodeID,
        step: InverseStep,
        anchor: Option<(RelativeAnchor, usize)>,
        walk: &mut TransposeWalk,
        visit: &mut impl FnMut(TransposeSite<'_>),
    ) {
        // The compound being stepped away from is what a subject reached through this step had to
        // satisfy at this point of the selector. A compound whose set is not a single key describes
        // no requirement that a subject's ancestor walk can check one lookup at a time.
        let mut keys = Vec::new();
        let waypoint = match self.dispatch_keys_of(enclosing, &mut keys) && keys.len() == 1 {
            true => Some(keys[0]),
            false => None,
        };
        walk.path.push(step);
        walk.waypoints.push(waypoint);
        self.walk_transpose(inner, inner, anchor, walk, visit);
        walk.waypoints.pop();
        walk.path.pop();
    }
}

/// The stack a transpose walk carries: the inverse steps taken from the input so far and, for each
/// one, the single distinguishing key of the compound it stepped away from.
#[derive(Default)]
struct TransposeWalk {
    path: Vec<InverseStep>,
    waypoints: Vec<Option<DispatchKey>>,
    applied_path: Vec<InverseStep>,
    applied_waypoints: Vec<DispatchKey>,
    origin_dispatch: Vec<DispatchKey>,
    origin_required: Vec<DispatchKey>,
}

/// One semantic input a selector entry mentions, with everything routing needs to transpose it.
pub struct TransposeSite<'a> {
    pub key: RoutingKey,
    /// The IR node the input occurs at.
    pub node: SelectorNodeID,
    /// The inverse path from the input to the entry's subjects.
    pub path: &'a [InverseStep],
    /// Set when the input is a possible relational witness.
    pub anchor: Option<RelativeAnchor>,
    /// The distinguishing features of the compound the input occurs in.
    pub origin_dispatch: &'a [DispatchKey],
    /// Independently necessary features of the compound containing the input.
    pub origin_required: &'a [DispatchKey],
    /// What the selector says the parent of the compound the input occurs in must be.
    pub parent_dispatch: &'a [DispatchKey],
    /// One key per intermediate compound of a purely ancestral path, in origin-to-subject order.
    pub waypoints: &'a [DispatchKey],
}

/// Whether an element publishes this state as it connects.
///
/// A state that arrives with the element reaches whatever rests on it through the publication, so a
/// `:has()` witness testing one needs nothing from the tree delta. A state that is only published
/// when it later moves cannot be reached that way: the element was already in it when it arrived and
/// nothing said so, which is why an arrival has to be asked about those directly.
///
/// This mirrors what `record_element_initial_features` says on the C++ side. A state missing from
/// there and listed here would be silently unreachable on arrival; the safe direction is to leave a
/// state out of this list, which costs the question and answers it.
#[must_use]
fn state_is_published_on_arrival(fact: StateFact) -> bool {
    matches!(
        fact,
        StateFact::AnyLink
            | StateFact::Checked
            | StateFact::Defined
            | StateFact::Disabled
            | StateFact::Enabled
            | StateFact::Focus
            | StateFact::FocusVisible
            | StateFact::FocusWithin
            | StateFact::Invalid
            | StateFact::Link
            | StateFact::LocalLink
            | StateFact::Optional
            | StateFact::Required
            | StateFact::Unchecked
            | StateFact::UserInvalid
            | StateFact::UserValid
            | StateFact::Valid
            | StateFact::Visited
    )
}

/// Maps each semantic input key to the canonical transpose routes that mention it.
///
/// This is required program state, not an evictable view. Scanning every selector header for every
/// ordinary mutation would change the hot path's asymptotics, so routing has to be an index.
/// Feature postings may accelerate subject enumeration on top of it, but evicting a posting never
/// changes which routes run.
pub struct RoutingRegistry {
    routes: Vec<TransposeRoute>,
    /// Every route that sits inside a relational argument. A subtree leaving takes its facts
    /// with it, so there is no feature left to route from and these have to be asked directly.
    relational: Vec<RouteID>,
    /// Every route whose path leaves the element it starts from through a sibling. Those are
    /// the only paths that can reach outside a departing subtree without being relational, so a
    /// departure is routed through them and no others.
    sibling_first: Vec<RouteID>,
    /// Sibling-first routes indexed by a distinguishing feature of their left compound.
    sibling_first_by_origin: HashMap<DispatchKey, Vec<RouteID>>,
    rules: Vec<RuleID>,
    by_input: HashMap<RoutingKey, Vec<RouteID>>,
    arrival_by_input: HashMap<RoutingKey, Vec<RouteID>>,
    canonical_routes: HashMap<u64, RouteID>,
    canonical_next: Vec<Option<RouteID>>,
    paths: Vec<InverseStep>,
    keys: Vec<DispatchKey>,
    memory: MemoryLease,
    nested_memory: MemoryLease,
}

impl Default for RoutingRegistry {
    fn default() -> Self {
        Self {
            routes: Vec::new(),
            relational: Vec::new(),
            sibling_first: Vec::new(),
            sibling_first_by_origin: HashMap::default(),
            rules: Vec::new(),
            by_input: HashMap::default(),
            arrival_by_input: HashMap::default(),
            canonical_routes: HashMap::default(),
            canonical_next: Vec::new(),
            paths: Vec::new(),
            keys: Vec::new(),
            memory: MemoryLease::new(MemoryCategory::RoutingRegistry),
            nested_memory: MemoryLease::new(MemoryCategory::RoutingRegistry),
        }
    }
}

impl RoutingRegistry {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    fn push_route(memory: &mut MemoryLease, routes: &mut Vec<RouteID>, route: RouteID) {
        let capacity_before = routes.capacity();
        routes.push(route);
        memory.grow_committed(((routes.capacity() - capacity_before) * size_of::<RouteID>()) as u64);
    }

    fn insert(&mut self, key: RoutingKey, descriptor: RouteDescriptor<'_>, selector_entry: SelectorEntry) -> RouteID {
        let route_hash = Self::route_hash(&descriptor);
        let starts_through_a_sibling = descriptor.anchor.is_none()
            && matches!(
                descriptor.path.first(),
                Some(InverseStep::NextSibling | InverseStep::FollowingSiblings)
            );
        let mut candidate = self.canonical_routes.get(&route_hash).copied();
        let mut canonical_route = None;
        while let Some(route) = candidate {
            if self.route_descriptor(route) == descriptor {
                canonical_route = Some(route);
                break;
            }
            candidate = self.canonical_next[route.index()];
        }
        let RouteDescriptor {
            rule,
            program,
            entry,
            structural_node,
            subject_dispatch,
            subject_required,
            subject_position,
            origin_dispatch,
            origin_required,
            parent_dispatch,
            waypoints,
            anchor,
            path,
        } = descriptor;
        let route = canonical_route.unwrap_or_else(|| {
            let route = RouteID::from_index(self.routes.len());
            let path_offset = u32::try_from(self.paths.len()).expect("transpose path space exhausted");
            self.paths.extend_from_slice(path);
            let origin_offset = u32::try_from(self.keys.len()).expect("dispatch key space exhausted");
            self.keys.extend_from_slice(origin_dispatch);
            let origin_required_offset = u32::try_from(self.keys.len()).expect("dispatch key space exhausted");
            self.keys.extend_from_slice(origin_required);
            let parent_offset = u32::try_from(self.keys.len()).expect("dispatch key space exhausted");
            self.keys.extend_from_slice(parent_dispatch);
            let subject_offset = u32::try_from(self.keys.len()).expect("dispatch key space exhausted");
            self.keys.extend_from_slice(subject_dispatch);
            let subject_required_offset = u32::try_from(self.keys.len()).expect("dispatch key space exhausted");
            self.keys.extend_from_slice(subject_required);
            let waypoint_offset = u32::try_from(self.keys.len()).expect("dispatch key space exhausted");
            self.keys.extend_from_slice(waypoints);
            self.routes.push(TransposeRoute {
                program,
                entry,
                has_prefix_chain: selector_entry.has_prefix_chain(),
                prefix_chain_has_only_local_facts: selector_entry.prefix_chain_has_only_local_facts(),
                structural_node,
                anchor,
                path_offset,
                path_length: u32::try_from(path.len()).expect("transpose path space exhausted"),
                origin_offset,
                origin_length: u32::try_from(origin_dispatch.len()).expect("dispatch key space exhausted"),
                origin_required_offset,
                origin_required_length: u32::try_from(origin_required.len()).expect("dispatch key space exhausted"),
                parent_offset,
                parent_length: u32::try_from(parent_dispatch.len()).expect("dispatch key space exhausted"),
                subject_offset,
                subject_length: u32::try_from(subject_dispatch.len()).expect("dispatch key space exhausted"),
                subject_required_offset,
                subject_required_length: u32::try_from(subject_required.len()).expect("dispatch key space exhausted"),
                subject_position,
                waypoint_offset,
                waypoint_length: u32::try_from(waypoints.len()).expect("dispatch key space exhausted"),
            });
            self.rules.push(rule);
            if anchor.is_some() {
                self.relational.push(route);
            }
            if starts_through_a_sibling {
                self.sibling_first.push(route);
                if origin_dispatch.is_empty() {
                    let routes = self.sibling_first_by_origin.entry(DispatchKey::Universal).or_default();
                    Self::push_route(&mut self.nested_memory, routes, route);
                } else {
                    for &origin in origin_dispatch {
                        let routes = self.sibling_first_by_origin.entry(origin).or_default();
                        Self::push_route(&mut self.nested_memory, routes, route);
                    }
                }
            }
            let previous = self.canonical_routes.insert(route_hash, route);
            self.canonical_next.push(previous);
            route
        });

        if anchor.is_some() || starts_through_a_sibling {
            let arrivals = self.arrival_by_input.entry(key).or_default();
            if arrivals.last() != Some(&route) {
                Self::push_route(&mut self.nested_memory, arrivals, route);
            }
        }
        let entries = self.by_input.entry(key).or_default();
        if entries.last() != Some(&route) {
            Self::push_route(&mut self.nested_memory, entries, route);
        }
        route
    }

    fn route_hash(descriptor: &RouteDescriptor<'_>) -> u64 {
        let mut hasher = fast_hasher();
        descriptor.hash(&mut hasher);
        hasher.finish()
    }

    fn route_descriptor(&self, route: RouteID) -> RouteDescriptor<'_> {
        let point = self.route(route);
        RouteDescriptor {
            rule: self.rule_of(route),
            program: point.program,
            entry: point.entry,
            structural_node: point.structural_node,
            subject_dispatch: self.subject_dispatch_of(route),
            subject_required: self.subject_required_of(route),
            subject_position: point.subject_position,
            origin_dispatch: self.origin_dispatch_of(route),
            origin_required: self.origin_required_of(route),
            parent_dispatch: self.parent_dispatch_of(route),
            waypoints: self.waypoints_of(route),
            anchor: point.anchor,
            path: self.path_of(route),
        }
    }

    /// Every route inside a relational argument, in a stable order.
    #[must_use]
    pub fn relational_routes(&self) -> &[RouteID] {
        &self.relational
    }

    /// Every route whose path steps to a sibling first, in a stable order.
    #[must_use]
    pub fn sibling_first_routes(&self) -> &[RouteID] {
        &self.sibling_first
    }

    /// Sibling-first routes whose left compound can match a node carrying `origin`.
    #[must_use]
    pub fn sibling_first_routes_for_origin(&self, origin: DispatchKey) -> &[RouteID] {
        self.sibling_first_by_origin.get(&origin).map_or(&[], Vec::as_slice)
    }

    /// Routes reached from one semantic input, in a stable order.
    #[must_use]
    pub fn routes_for(&self, key: RoutingKey) -> &[RouteID] {
        self.by_input.get(&key).map_or(&[], Vec::as_slice)
    }

    /// Routes for which an arriving element's facts can affect nodes outside its subtree.
    #[must_use]
    pub fn arrival_routes_for(&self, key: RoutingKey) -> &[RouteID] {
        self.arrival_by_input.get(&key).map_or(&[], Vec::as_slice)
    }

    #[must_use]
    pub fn route(&self, route: RouteID) -> TransposeRoute {
        self.routes[route.index()]
    }

    #[must_use]
    pub fn rule_of(&self, route: RouteID) -> RuleID {
        self.rules[route.index()]
    }

    #[must_use]
    pub fn path_of(&self, route: RouteID) -> &[InverseStep] {
        let point = self.route(route);
        &self.paths[point.path_offset as usize..(point.path_offset + point.path_length) as usize]
    }

    /// The features the element an input happened to must carry for this route to be
    /// reachable from it. Empty means it rejects nothing.
    #[must_use]
    pub fn origin_dispatch_of(&self, route: RouteID) -> &[DispatchKey] {
        let point = self.route(route);
        &self.keys[point.origin_offset as usize..(point.origin_offset + point.origin_length) as usize]
    }

    /// Independently necessary features of the compound containing the input.
    #[must_use]
    pub fn origin_required_of(&self, route: RouteID) -> &[DispatchKey] {
        let point = self.route(route);
        &self.keys[point.origin_required_offset as usize
            ..(point.origin_required_offset + point.origin_required_length) as usize]
    }

    /// What the parent of the compound an input occurs in must be. Empty means it rejects nothing.
    #[must_use]
    pub fn parent_dispatch_of(&self, route: RouteID) -> &[DispatchKey] {
        let point = self.route(route);
        &self.keys[point.parent_offset as usize..(point.parent_offset + point.parent_length) as usize]
    }

    /// How far into a sibling sequence the entry's subjects can be, when a positional test says so.
    #[must_use]
    pub fn subject_position_of(&self, route: RouteID) -> SubjectPosition {
        self.route(route).subject_position
    }

    /// The features the entry's subjects carry, which is what a region is narrowed to. Empty means
    /// the subjects have no feature to enumerate them by.
    #[must_use]
    pub fn subject_dispatch_of(&self, route: RouteID) -> &[DispatchKey] {
        let point = self.route(route);
        &self.keys[point.subject_offset as usize..(point.subject_offset + point.subject_length) as usize]
    }

    /// Independently necessary features of the entry's subject compound.
    #[must_use]
    pub fn subject_required_of(&self, route: RouteID) -> &[DispatchKey] {
        let point = self.route(route);
        &self.keys[point.subject_required_offset as usize
            ..(point.subject_required_offset + point.subject_required_length) as usize]
    }

    /// Keys some ancestor of a subject must carry for this route to reach it. Empty rejects
    /// nothing.
    #[must_use]
    pub fn waypoints_of(&self, route: RouteID) -> &[DispatchKey] {
        let point = self.route(route);
        &self.keys[point.waypoint_offset as usize..(point.waypoint_offset + point.waypoint_length) as usize]
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.routes.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.routes.is_empty()
    }

    #[must_use]
    #[cfg(test)]
    pub fn routed_input_count(&self) -> usize {
        self.by_input.len()
    }

    /// Add every transpose route of one attached rule.
    pub fn add_rule(&mut self, rule: RuleID, program: SelectorProgramID, compiled: &SelectorProgram) {
        for entry in 0..compiled.entries().len() {
            let selector_entry = compiled.entries()[entry];
            let subject_dispatch = compiled.subject_dispatch_keys(entry);
            let subject_required = compiled.subject_required_keys(entry);
            let subject_position = compiled.subject_position(entry);
            compiled.collect_transpose_entry_points(entry, |site| {
                self.insert(
                    site.key,
                    RouteDescriptor {
                        rule,
                        program,
                        entry: u32::try_from(entry).expect("selector entry space exhausted"),
                        structural_node: (site.key == RoutingKey::Structural).then_some(site.node),
                        subject_dispatch,
                        subject_required,
                        subject_position,
                        origin_dispatch: site.origin_dispatch,
                        origin_required: site.origin_required,
                        parent_dispatch: site.parent_dispatch,
                        waypoints: site.waypoints,
                        anchor: site.anchor,
                        path: site.path,
                    },
                    selector_entry,
                );
            });
        }
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.routes,
                self.relational,
                self.sibling_first,
                self.sibling_first_by_origin,
                self.rules,
                self.paths,
                self.keys,
                self.by_input,
                self.arrival_by_input,
                self.canonical_routes,
                self.canonical_next,
            ];
            cached [self.nested_memory.bytes()];
            nested [];
            skip [self.memory];
        }
    }

    pub fn settle_memory(&mut self, memory: &mut MemoryController) {
        self.nested_memory.settle_committed(memory);
        let header = self.capacity_bytes() - self.nested_memory.bytes();
        self.memory.resize_required_to(memory, header);
    }

    pub fn release_memory(&mut self) {
        self.memory.release();
        self.nested_memory.release();
    }
}

/// Why a match evaluation could not produce an exact answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Incomplete {
    /// The fact batch does not cover a style node the evaluation had to read. This is never a
    /// negative answer: the caller widens the batch or asks a different question.
    MissingFacts(StyleNodeID),
    /// A descendant scan reached a row the fact batch does not cover. Every node from `first`
    /// through the remainder of `root`'s preorder is the range the scan can still read.
    MissingDescendantFacts { root: StyleNodeID, first: StyleNodeID },
    /// A sibling scan reached a row the fact batch does not cover. Every node from `first` up to,
    /// but not including, `last_exclusive` is the remaining range that scan can read.
    MissingSiblingFacts {
        first: StyleNodeID,
        last_exclusive: Option<StyleNodeID>,
    },
}

impl Incomplete {
    #[must_use]
    pub fn first_missing_node(self) -> StyleNodeID {
        match self {
            Self::MissingFacts(node) => node,
            Self::MissingDescendantFacts { first, .. } => first,
            Self::MissingSiblingFacts { first, .. } => first,
        }
    }
}

impl TransactionFactView {
    pub(super) fn insert_before_sibling_sequence(&mut self, parent: StyleNodeID, children: Vec<StyleNodeID>) {
        let (sequence, _, _) = self.before_sibling_geometry.insert_sequence(children);
        debug_assert_eq!(sequence as usize, self.before_sibling_parents_by_sequence.len());
        self.before_sibling_sequence_by_parent.push((parent, sequence));
        self.before_sibling_parents_by_sequence.push(parent);
    }

    pub(super) fn mark_before_absent(&mut self, node: StyleNodeID) {
        self.before_absent_nodes.push(node);
    }

    pub(super) fn finish_before_sibling_relations(&mut self) {
        self.before_sibling_sequence_by_parent
            .sort_unstable_by_key(|&(parent, _)| parent);
        debug_assert!(
            self.before_sibling_sequence_by_parent
                .windows(2)
                .all(|entries| entries[0].0 != entries[1].0)
        );
        self.before_absent_nodes.sort_unstable();
        self.before_absent_nodes.dedup();
        self.before_sibling_relations_available = true;
    }

    pub(super) fn clear_before_sibling_relations(&mut self) {
        self.before_sibling_geometry = SiblingSequenceGeometry::default();
        self.before_sibling_sequence_by_parent = Vec::new();
        self.before_sibling_parents_by_sequence = Vec::new();
        self.before_absent_nodes = Vec::new();
        self.before_sibling_relations_available = false;
    }

    pub(super) fn is_present(&self, tree: &StyleNodeTree, side: TransactionFactSide, node: StyleNodeID) -> bool {
        match side {
            TransactionFactSide::Before => {
                debug_assert!(self.before_sibling_relations_available);
                self.before_absent_nodes.binary_search(&node).is_err()
            }
            TransactionFactSide::After => tree.is_live(node),
        }
    }

    fn parent_of(&self, tree: &StyleNodeTree, side: TransactionFactSide, node: StyleNodeID) -> Option<StyleNodeID> {
        if matches!(side, TransactionFactSide::After) {
            return tree.parent(node);
        }
        debug_assert!(self.before_sibling_relations_available);
        if !self.is_present(tree, side, node) {
            return None;
        }
        // A captured membership names the parent the node started under, which the live tree
        // cannot: a node that moved between parents is live under its new one.
        self.before_sibling_geometry
            .memberships
            .get(node.element_index()? as usize)
            .and_then(|membership| {
                self.before_sibling_parents_by_sequence
                    .get(membership.sequence as usize)
                    .copied()
            })
            .or_else(|| tree.parent(node))
    }

    fn previous_sibling_of(
        &self,
        tree: &StyleNodeTree,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<StyleNodeID> {
        if matches!(side, TransactionFactSide::After) {
            return tree.previous_element_sibling(node);
        }
        self.parent_of(tree, side, node)?;
        let Some((sequence, ordinal)) = self.before_sibling_geometry.sequence_and_ordinal(node) else {
            return tree.previous_element_sibling(node);
        };
        ordinal.checked_sub(1).map(|previous| sequence[previous as usize])
    }

    fn next_sibling_of(
        &self,
        tree: &StyleNodeTree,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<StyleNodeID> {
        if matches!(side, TransactionFactSide::After) {
            return tree.next_element_sibling(node);
        }
        self.parent_of(tree, side, node)?;
        let Some((sequence, ordinal)) = self.before_sibling_geometry.sequence_and_ordinal(node) else {
            return tree.next_element_sibling(node);
        };
        sequence.get(ordinal as usize + 1).copied()
    }

    fn children_of<'a>(
        &'a self,
        tree: &'a StyleNodeTree,
        side: TransactionFactSide,
        parent: StyleNodeID,
    ) -> SiblingChildren<'a> {
        if matches!(side, TransactionFactSide::After) {
            return SiblingChildren::Live(tree.children(parent));
        }
        debug_assert!(self.before_sibling_relations_available);
        if !self.is_present(tree, side, parent) {
            return SiblingChildren::Overlay([].iter().copied());
        }
        match self
            .before_sibling_sequence_by_parent
            .binary_search_by_key(&parent, |&(candidate, _)| candidate)
        {
            Ok(index) => {
                let sequence = self.before_sibling_sequence_by_parent[index].1;
                SiblingChildren::Overlay(
                    self.before_sibling_geometry.sequences[sequence as usize]
                        .iter()
                        .copied(),
                )
            }
            Err(_) => SiblingChildren::Live(tree.children(parent)),
        }
    }

    fn sibling_positions(&self, side: TransactionFactSide, node: StyleNodeID) -> Option<SiblingPositions> {
        match side {
            TransactionFactSide::Before => self.before_sibling_geometry.sibling_positions(node),
            TransactionFactSide::After => None,
        }
    }

    fn sibling_sequence(&self, side: TransactionFactSide, node: StyleNodeID) -> Option<Rc<[StyleNodeID]>> {
        match side {
            TransactionFactSide::Before => self.before_sibling_geometry.sibling_sequence(node),
            TransactionFactSide::After => None,
        }
    }
}

enum SiblingChildren<'a> {
    Live(super::tree::Children<'a>),
    Overlay(std::iter::Copied<std::slice::Iter<'a, StyleNodeID>>),
}

impl Iterator for SiblingChildren<'_> {
    type Item = StyleNodeID;

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Live(children) => children.next(),
            Self::Overlay(children) => children.next(),
        }
    }
}

/// Evaluates match programs against the live tree and a batch of local facts.
pub struct MatchEvaluator<'a> {
    tree: &'a StyleNodeTree,
    facts: &'a StyleNodeFacts,
    transaction_fact_view: Option<(&'a TransactionFactView, TransactionFactSide)>,
    /// The shadow root of the tree whose rules are being evaluated, when it is one. `:host` names
    /// the host of the tree its rule is in, so a rule from the document scope names no host at all.
    scope_shadow_root: Option<StyleNodeID>,
    /// The outer tree scope asking about a part exposed from a shadow tree.
    part_exposure_scope: Option<TreeScopeID>,
    /// The scoping root a `<scope-end>` is currently being checked against. Bound only while the
    /// limit walk runs, because that is the only place one scope instance is distinguishable from
    /// another. Interior mutability so that evaluation stays a shared borrow.
    scope_root_instance: Cell<Option<StyleNodeID>>,
    /// Whether evaluation is inside the argument of `:host()`. A nested `:host` does not describe
    /// a feature of the host and must not match there.
    matching_host_argument: Cell<bool>,
    root_matches_parentless_node: bool,
    /// The anchor of the relational query currently being evaluated. Bound only while the witness
    /// walk runs, and restored after, so that a nested `:has()` names its own anchor.
    relative_anchor: Cell<Option<StyleNodeID>>,
    match_workspace: Option<(&'a MatchEvaluationWorkspace, MatchEvaluationSide)>,
    transitive_relation_program: Cell<Option<SelectorProgramID>>,
    /// Where a completed simple relational evaluation records its outcome, when the caller is
    /// evaluating the live tree and current facts. See `MatchEvaluator::observing_witnesses`.
    witnesses: Option<&'a RefCell<RelationalWitnesses>>,
}

/// Transaction-local answers for repeated match-program relations.
///
/// Exact invalidation evaluates the same selector entry over a region. In tree order, an ancestor
/// or preceding-sibling relation differs from the preceding candidate by one edge, so retaining
/// that answer turns repeated prefix walks into a dynamic program.
#[derive(Default)]
struct MatchRelationCache {
    answers: RefCell<MatchRelationAnswers>,
    preceding_sibling_parent_ids: RefCell<HashMap<StyleNodeID, PrecedingSiblingParentID>>,
    preceding_sibling_prefixes: RefCell<PrecedingSiblingPrefixes>,
}

#[derive(Default)]
struct MatchRelationAnswers {
    columns: ProgramRelationColumns<RelationAnswerColumn>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct MatchRelationAnswerGap {
    program: SelectorProgramID,
    relation: SelectorNodeID,
    node: StyleNodeID,
}

/// Prefix progress indexed by dense program, relation, and transaction-local parent identities.
///
/// Only a small subset of document parents normally owns a sibling sequence under evaluation.
/// Intern those parents once at the cache boundary, then keep every compiled relation's repeatedly
/// updated prefix state in a direct column rather than repeating the three identities in a hash key.
#[derive(Default)]
struct PrecedingSiblingPrefixes {
    columns: ProgramRelationColumns<PrecedingSiblingPrefixColumn>,
}

define_id! { struct PrecedingSiblingParentID(); }

#[derive(Clone, Copy, Default)]
struct PrecedingSiblingPrefix {
    next: Option<StyleNodeID>,
    answer: bool,
}

struct ProgramRelationColumns<C> {
    programs: Column<Option<Box<ProgramColumns<C>>>>,
    footprint: MemoryLease,
}

impl<C> Default for ProgramRelationColumns<C> {
    fn default() -> Self {
        Self {
            programs: Column::default(),
            footprint: MemoryLease::new(MemoryCategory::BatchScratch),
        }
    }
}

#[derive(Default)]
struct ProgramColumns<C> {
    relations: Column<Option<C>>,
}

impl<C: Default> ProgramRelationColumns<C> {
    fn get(&self, program: SelectorProgramID, relation: SelectorNodeID) -> Option<&C> {
        self.programs
            .get(program.0 as usize)?
            .as_ref()?
            .relations
            .get(relation.0 as usize)?
            .as_ref()
    }

    fn column_mut(&mut self, program: SelectorProgramID, relation: SelectorNodeID) -> (&mut C, bool) {
        let program_index = program.0 as usize;
        self.footprint.grow_committed(self.programs.ensure(program_index));

        let program_was_absent = self.programs[program_index].is_none();
        let program_columns = self.programs[program_index].get_or_insert_with(Box::default);
        self.footprint
            .grow_committed((usize::from(program_was_absent) * size_of::<ProgramColumns<C>>()) as u64);

        let relation_index = relation.0 as usize;
        self.footprint
            .grow_committed(program_columns.relations.ensure(relation_index));

        let column_was_absent = program_columns.relations[relation_index].is_none();
        (
            program_columns.relations[relation_index].get_or_insert_with(C::default),
            column_was_absent,
        )
    }

    #[must_use]
    fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [];
            cached [self.footprint.bytes()];
            nested [];
            skip [self.programs];
        }) as usize
    }
}

const RELATION_ANSWER_PAGE_SHIFT: usize = 9;
const RELATION_ANSWER_PAGE_BITS: usize = 1 << RELATION_ANSWER_PAGE_SHIFT;
const RELATION_ANSWER_PAGE_WORDS: usize = RELATION_ANSWER_PAGE_BITS / u64::BITS as usize;

/// A sparse two-bit answer column for one compiled transitive relation.
///
/// Element identities are dense, so a page needs only a known bit and an answer bit per identity.
/// Program and relation identities index directly into the outer tables instead of being repeated
/// in every answer.
type RelationAnswerColumn = PagedColumn<RelationAnswerPage>;

#[derive(Default)]
struct RelationAnswerPage {
    known: [u64; RELATION_ANSWER_PAGE_WORDS],
    answers: [u64; RELATION_ANSWER_PAGE_WORDS],
}

impl PagedColumnPage for RelationAnswerPage {
    type Value = bool;

    const SHIFT: usize = RELATION_ANSWER_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<bool> {
        let word = index / u64::BITS as usize;
        let mask = 1 << (index % u64::BITS as usize);
        if self.known[word] & mask == 0 {
            None
        } else {
            Some(self.answers[word] & mask != 0)
        }
    }

    fn insert(&mut self, index: usize, answer: bool) {
        let word = index / u64::BITS as usize;
        let mask = 1 << (index % u64::BITS as usize);
        self.known[word] |= mask;
        match answer {
            true => self.answers[word] |= mask,
            false => self.answers[word] &= !mask,
        }
    }
}

impl MatchRelationAnswers {
    fn lookup(
        &self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        node: StyleNodeID,
    ) -> Lookup<(), MatchRelationAnswerGap> {
        let gap = MatchRelationAnswerGap {
            program,
            relation,
            node,
        };
        let Some(column) = self.columns.get(program, relation) else {
            return Lookup::Missing(gap);
        };
        match column.get(node.element_index().unwrap() as usize) {
            Some(true) => Lookup::Known(()),
            Some(false) => Lookup::KnownAbsent,
            None => Lookup::Missing(gap),
        }
    }

    /// Publish one answer and report whether this created a relation column and a node page.
    fn insert(
        &mut self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        node: StyleNodeID,
        answer: bool,
    ) -> (bool, bool) {
        let (column_was_absent, page_was_absent, bytes_grown) = {
            let (column, column_was_absent) = self.columns.column_mut(program, relation);
            let column_bytes_before = column.capacity_bytes();
            let page_was_absent = column.insert(node.element_index().unwrap() as usize, answer).1;
            (
                column_was_absent,
                page_was_absent,
                column.capacity_bytes() - column_bytes_before,
            )
        };
        self.columns.footprint.grow_committed(bytes_grown);
        (column_was_absent, page_was_absent)
    }

    #[must_use]
    fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [];
            cached [self.columns.capacity_bytes()];
            nested [];
            skip [];
        }) as usize
    }
}

impl PrecedingSiblingPrefixes {
    fn get(
        &self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        parent: PrecedingSiblingParentID,
    ) -> Option<PrecedingSiblingPrefix> {
        self.columns.get(program, relation)?.get(parent.0 as usize)
    }

    fn insert(
        &mut self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        parent: PrecedingSiblingParentID,
        prefix: PrecedingSiblingPrefix,
    ) {
        let bytes_grown = {
            let (column, _) = self.columns.column_mut(program, relation);
            let column_bytes_before = column.capacity_bytes();
            column.insert(parent.0 as usize, prefix);
            column.capacity_bytes() - column_bytes_before
        };
        self.columns.footprint.grow_committed(bytes_grown);
    }

    #[must_use]
    fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [];
            cached [self.columns.capacity_bytes()];
            nested [];
            skip [];
        }) as usize
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub(super) struct SiblingPositions {
    pub(super) from_start: u32,
    pub(super) from_end: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct SiblingSequenceMembership {
    sequence: u32,
    ordinal: u32,
}

const VALUE_PAGE_SHIFT: usize = 6;
const VALUE_PAGE_SIZE: usize = 1 << VALUE_PAGE_SHIFT;

struct ValuePage<T: Copy + Default> {
    known: u64,
    values: [T; VALUE_PAGE_SIZE],
}

impl<T: Copy + Default> Default for ValuePage<T> {
    fn default() -> Self {
        Self {
            known: 0,
            values: [T::default(); VALUE_PAGE_SIZE],
        }
    }
}

impl<T: Copy + Default> PagedColumnPage for ValuePage<T> {
    type Value = T;

    const SHIFT: usize = VALUE_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<T> {
        (self.known & (1 << index) != 0).then_some(self.values[index])
    }

    fn insert(&mut self, index: usize, value: T) {
        self.known |= 1 << index;
        self.values[index] = value;
    }
}

type PrecedingSiblingPrefixColumn = PagedColumn<ValuePage<PrecedingSiblingPrefix>>;
type SiblingPositionColumn = PagedColumn<ValuePage<SiblingPositions>>;
type SiblingSequenceMembershipColumn = PagedColumn<ValuePage<SiblingSequenceMembership>>;

#[derive(Default)]
pub(super) struct SiblingSequenceGeometry {
    sequences: Vec<Rc<[StyleNodeID]>>,
    memberships: SiblingSequenceMembershipColumn,
    /// Bytes held by the sequences themselves, kept running so `capacity_bytes` is O(1) on the
    /// per-candidate before-and-after accounting path.
    sequence_bytes: usize,
}

impl SiblingSequenceGeometry {
    fn insert_sequence(&mut self, children: Vec<StyleNodeID>) -> (u32, Rc<[StyleNodeID]>, usize) {
        let children: Rc<[StyleNodeID]> = children.into();
        let sequence = u32::try_from(self.sequences.len()).expect("sibling sequence index space exhausted");
        self.sequence_bytes += children.len() * size_of::<StyleNodeID>() + 2 * size_of::<usize>();
        self.sequences.push(children.clone());
        let mut pages = 0;
        for (ordinal, &child) in children.iter().enumerate() {
            pages += usize::from(
                self.memberships
                    .insert(
                        child
                            .element_index()
                            .expect("only elements belong to sibling sequences") as usize,
                        SiblingSequenceMembership {
                            sequence,
                            ordinal: u32::try_from(ordinal).expect("sibling sequence space exhausted"),
                        },
                    )
                    .1,
            );
        }
        (sequence, children, pages)
    }

    fn sequence_and_ordinal(&self, node: StyleNodeID) -> Option<(&Rc<[StyleNodeID]>, u32)> {
        let membership = self.memberships.get(node.element_index()? as usize)?;
        Some((&self.sequences[membership.sequence as usize], membership.ordinal))
    }

    fn sibling_sequence(&self, node: StyleNodeID) -> Option<Rc<[StyleNodeID]>> {
        self.sequence_and_ordinal(node).map(|(sequence, _)| sequence.clone())
    }

    pub(super) fn sibling_positions(&self, node: StyleNodeID) -> Option<SiblingPositions> {
        let (sequence, ordinal) = self.sequence_and_ordinal(node)?;
        let count = u32::try_from(sequence.len()).expect("sibling sequence space exhausted");
        Some(SiblingPositions {
            from_start: ordinal + 1,
            from_end: count - ordinal,
        })
    }

    pub(super) fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [self.sequences];
            cached [self.sequence_bytes];
            nested [self.memberships.capacity_bytes()];
            skip [];
        }) as usize
    }
}

/// The semantic side of a match-program evaluation.
#[derive(Clone, Copy)]
#[repr(usize)]
pub(super) enum MatchEvaluationSide {
    Current = 0,
    OldTree = 1,
    OldFacts = 2,
}

impl MatchEvaluationSide {
    fn tree_side(self) -> usize {
        usize::from(matches!(self, Self::OldTree))
    }
}

/// Transaction-local workspace shared by match-program evaluations.
///
/// Exact comparison has three semantic sides, but only two tree geometries. Current matching and
/// old-fact matching share the current sequence positions; the old-tree side has its own. Type
/// positions and selector answers additionally depend on the fact side and therefore remain
/// distinct.
#[derive(Default)]
pub struct MatchEvaluationWorkspace {
    relations_by_evaluation_side: [MatchRelationCache; 3],
    sibling_geometry_by_tree_side: [RefCell<SiblingSequenceGeometry>; 2],
    type_positions_by_evaluation_side: [RefCell<SiblingPositionColumn>; 3],
    /// Canonical plain an+b answers shared across independently compiled selector programs.
    positional_answers_by_evaluation_side: [RefCell<PositionalAnswers>; 3],
}

#[derive(Default)]
struct PositionalAnswers {
    by_test: Vec<(NthPosition, HashMap<StyleNodeID, bool>)>,
}

impl PositionalAnswers {
    fn get(&self, position: NthPosition, node: StyleNodeID) -> Option<bool> {
        self.by_test
            .iter()
            .find(|(candidate, _)| *candidate == position)?
            .1
            .get(&node)
            .copied()
    }

    fn insert(&mut self, position: NthPosition, node: StyleNodeID, answer: bool) {
        match self.by_test.iter_mut().find(|(candidate, _)| *candidate == position) {
            Some((_, answers)) => {
                answers.insert(node, answer);
            }
            None => {
                self.by_test.push((position, HashMap::from_iter([(node, answer)])));
            }
        }
    }

    fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [self.by_test];
            cached [];
            nested [self.by_test.iter().map(|(_, answers)| {
                capacity_bytes! {
                    shallow [*answers];
                    cached [];
                    nested [];
                    skip [];
                }
            }).sum::<u64>()];
            skip [];
        }) as usize
    }
}

impl MatchRelationCache {
    #[must_use]
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [];
            nested [
                self.answers.borrow().capacity_bytes(),
                {
                    let parent_ids = self.preceding_sibling_parent_ids.borrow();
                    capacity_bytes! {
                        shallow [*parent_ids];
                        cached [];
                        nested [];
                        skip [];
                    }
                },
                self.preceding_sibling_prefixes.borrow().capacity_bytes(),
            ];
            skip [];
        }
    }

    /// Discard selector-specific answers before a following matching traversal.
    ///
    /// Exact planning asks a narrow set of selector entries, while matching asks every candidate
    /// entry for each element being styled. Carrying the planner's sparse selector-answer maps into
    /// that much wider probe set makes almost every lookup a miss and displaces useful matching
    /// state.
    fn clear_selector_answers(&mut self) {
        *self.answers.get_mut() = MatchRelationAnswers::default();
        *self.preceding_sibling_prefixes.get_mut() = PrecedingSiblingPrefixes::default();
    }

    fn lookup(
        &self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        node: StyleNodeID,
    ) -> Lookup<(), MatchRelationAnswerGap> {
        self.answers.borrow().lookup(program, relation, node)
    }

    fn insert(&self, program: SelectorProgramID, relation: SelectorNodeID, node: StyleNodeID, answer: bool) {
        self.answers.borrow_mut().insert(program, relation, node, answer);
    }

    fn preceding_sibling_prefix(
        &self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        parent: StyleNodeID,
    ) -> (PrecedingSiblingParentID, Option<PrecedingSiblingPrefix>) {
        let parent = {
            let mut parent_ids = self.preceding_sibling_parent_ids.borrow_mut();
            match parent_ids.get(&parent).copied() {
                Some(parent) => parent,
                None => {
                    let id = PrecedingSiblingParentID(
                        u32::try_from(parent_ids.len()).expect("preceding sibling parent space exhausted"),
                    );
                    parent_ids.insert(parent, id);
                    id
                }
            }
        };
        (
            parent,
            self.preceding_sibling_prefixes.borrow().get(program, relation, parent),
        )
    }

    fn insert_preceding_sibling_prefix(
        &self,
        program: SelectorProgramID,
        relation: SelectorNodeID,
        parent: PrecedingSiblingParentID,
        prefix: PrecedingSiblingPrefix,
    ) {
        self.preceding_sibling_prefixes
            .borrow_mut()
            .insert(program, relation, parent, prefix);
    }
}

impl MatchEvaluationWorkspace {
    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [];
            nested [
                self.relations_by_evaluation_side
                    .iter()
                    .map(MatchRelationCache::capacity_bytes)
                    .sum::<u64>(),
                self
                .sibling_geometry_by_tree_side
                .iter()
                .map(|geometry| geometry.borrow().capacity_bytes())
                .sum::<usize>(),
                self
                .type_positions_by_evaluation_side
                .iter()
                .map(|positions| positions.borrow().capacity_bytes())
                .sum::<u64>(),
                self.positional_answers_by_evaluation_side
                .iter()
                .map(|answers| answers.borrow().capacity_bytes())
                .sum::<usize>(),
            ];
            skip [];
        }
    }

    /// Keep only reusable current-tree, current-fact data for the following matching traversal.
    pub(super) fn retain_current_for_matching(&mut self) {
        self.relations_by_evaluation_side[MatchEvaluationSide::Current as usize].clear_selector_answers();
        self.clear_old_evaluation_sides();
    }

    /// Old-side answers describe one transaction's before state, so they must not survive into
    /// the next transaction, whose before state is a different topology.
    pub(super) fn clear_old_evaluation_sides(&mut self) {
        self.relations_by_evaluation_side[MatchEvaluationSide::OldTree as usize] = MatchRelationCache::default();
        self.relations_by_evaluation_side[MatchEvaluationSide::OldFacts as usize] = MatchRelationCache::default();
        *self.sibling_geometry_by_tree_side[MatchEvaluationSide::OldTree.tree_side()].get_mut() =
            SiblingSequenceGeometry::default();
        *self.type_positions_by_evaluation_side[MatchEvaluationSide::OldTree as usize].get_mut() =
            SiblingPositionColumn::default();
        *self.type_positions_by_evaluation_side[MatchEvaluationSide::OldFacts as usize].get_mut() =
            SiblingPositionColumn::default();
        *self.positional_answers_by_evaluation_side[MatchEvaluationSide::OldTree as usize].get_mut() =
            PositionalAnswers::default();
        *self.positional_answers_by_evaluation_side[MatchEvaluationSide::OldFacts as usize].get_mut() =
            PositionalAnswers::default();
    }

    fn relations(&self, side: MatchEvaluationSide) -> &MatchRelationCache {
        &self.relations_by_evaluation_side[side as usize]
    }

    pub(super) fn sibling_position(
        &self,
        node: StyleNodeID,
        side: MatchEvaluationSide,
        of_type: bool,
    ) -> Option<SiblingPositions> {
        match of_type {
            true => self.type_positions_by_evaluation_side[side as usize]
                .borrow()
                .get(node.element_index()? as usize),
            false => self.sibling_geometry_by_tree_side[side.tree_side()]
                .borrow()
                .sibling_positions(node),
        }
    }

    fn insert_type_position(&self, node: StyleNodeID, side: MatchEvaluationSide, position: SiblingPositions) -> bool {
        self.type_positions_by_evaluation_side[side as usize]
            .borrow_mut()
            .insert(
                node.element_index().expect("only elements have sibling positions") as usize,
                position,
            )
            .1
    }

    fn positional_answer(&self, position: NthPosition, node: StyleNodeID, side: MatchEvaluationSide) -> Option<bool> {
        self.positional_answers_by_evaluation_side[side as usize]
            .borrow()
            .get(position, node)
    }

    fn insert_positional_answer(
        &self,
        position: NthPosition,
        node: StyleNodeID,
        side: MatchEvaluationSide,
        answer: bool,
    ) {
        self.positional_answers_by_evaluation_side[side as usize]
            .borrow_mut()
            .insert(position, node, answer);
    }

    fn sibling_sequence(&self, node: StyleNodeID, side: MatchEvaluationSide) -> Option<Rc<[StyleNodeID]>> {
        self.sibling_geometry_by_tree_side[side.tree_side()]
            .borrow()
            .sibling_sequence(node)
    }

    /// Publish one tree-side sibling sequence and return its shared child range, allocated pages,
    /// and whether the sequence was new.
    pub(super) fn publish_sibling_sequence(
        &self,
        children: Vec<StyleNodeID>,
        side: MatchEvaluationSide,
    ) -> (Rc<[StyleNodeID]>, usize, bool) {
        if let Some(&first) = children.first()
            && let Some(sequence) = self.sibling_sequence(first, side)
        {
            return (sequence, 0, false);
        }

        let (_, children, pages) = self.sibling_geometry_by_tree_side[side.tree_side()]
            .borrow_mut()
            .insert_sequence(children);
        (children, pages, true)
    }
}

impl<'a> MatchEvaluator<'a> {
    #[must_use]
    pub fn new(tree: &'a StyleNodeTree, facts: &'a StyleNodeFacts) -> Self {
        Self {
            tree,
            facts,
            transaction_fact_view: None,
            scope_shadow_root: None,
            part_exposure_scope: None,
            scope_root_instance: Cell::new(None),
            matching_host_argument: Cell::new(false),
            root_matches_parentless_node: true,
            relative_anchor: Cell::new(None),
            match_workspace: None,
            transitive_relation_program: Cell::new(None),
            witnesses: None,
        }
    }

    /// The same, evaluating rules attached to one shadow tree rather than to the document.
    #[must_use]
    pub fn in_shadow_tree(mut self, shadow_root: StyleNodeID) -> Self {
        self.scope_shadow_root = Some(shadow_root);
        self
    }

    /// Evaluate a part through the tree scope it is exposed to.
    #[must_use]
    pub fn for_a_part_exposed_in(mut self, scope: TreeScopeID) -> Self {
        self.part_exposure_scope = Some(scope);
        self
    }

    #[must_use]
    pub fn with_scope_root(self, scope_root: StyleNodeID) -> Self {
        self.scope_root_instance.set(Some(scope_root));
        self
    }

    #[must_use]
    pub fn without_document_root(mut self) -> Self {
        self.root_matches_parentless_node = false;
        self
    }

    #[must_use]
    pub(super) fn with_transaction_fact_view(
        mut self,
        view: &'a TransactionFactView,
        side: TransactionFactSide,
    ) -> Self {
        self.transaction_fact_view = Some((view, side));
        self
    }

    #[must_use]
    pub(super) fn with_match_workspace(
        mut self,
        workspace: &'a MatchEvaluationWorkspace,
        side: MatchEvaluationSide,
    ) -> Self {
        self.match_workspace = Some((workspace, side));
        self
    }

    /// Record completed simple relational evaluations in `witnesses`.
    ///
    /// Only an evaluator reading the live tree and the current facts may observe: retention's
    /// soundness rests on every entry having been written by an evaluation whose answer is the
    /// current truth, so an evaluator with a before-side transaction view or workspace must never
    /// call this.
    #[must_use]
    pub(super) fn observing_witnesses(mut self, witnesses: &'a RefCell<RelationalWitnesses>) -> Self {
        debug_assert!(self.transaction_fact_view.is_none());
        debug_assert!(
            self.match_workspace
                .is_none_or(|(_, side)| matches!(side, MatchEvaluationSide::Current))
        );
        self.witnesses = Some(witnesses);
        self
    }

    fn parent_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.transaction_fact_view.map_or_else(
            || self.tree.parent(node),
            |(view, side)| view.parent_of(self.tree, side, node),
        )
    }

    fn previous_sibling_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.transaction_fact_view.map_or_else(
            || self.tree.previous_element_sibling(node),
            |(view, side)| view.previous_sibling_of(self.tree, side, node),
        )
    }

    fn next_sibling_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.transaction_fact_view.map_or_else(
            || self.tree.next_element_sibling(node),
            |(view, side)| view.next_sibling_of(self.tree, side, node),
        )
    }

    fn children_of(&self, parent: StyleNodeID) -> SiblingChildren<'_> {
        match self.transaction_fact_view {
            Some((view, side)) => view.children_of(self.tree, side, parent),
            None => SiblingChildren::Live(self.tree.children(parent)),
        }
    }

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
        &self,
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

    /// How far the subject is from the scoping root its `@scope` resolved through.
    ///
    /// The specification creates one scope per element matching `<scope-start>`, and a declaration
    /// from the nearest one wins, so this is the hop count to the nearest inclusive ancestor the
    /// root selector names. A rule with no `@scope` is infinitely far, which is what makes an
    /// unscoped declaration lose the proximity comparison to every scoped one.
    pub fn scope_proximity_of(
        &self,
        program: &SelectorProgram,
        entry: &SelectorEntry,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<u32, Incomplete> {
        let Some(scope_root) = entry.scope_root else {
            return Ok(u32::MAX);
        };
        let mut proximity = 0;
        let mut candidate = Some(node);
        while let Some(current) = candidate {
            if self.matches_node(program, scope_root, current, counters)? {
                return Ok(proximity);
            }
            proximity += 1;
            candidate = self.tree.parent(current);
        }
        Ok(u32::MAX)
    }

    /// Whether `node` matches any entry of `program`, and which entry contributes the greatest
    /// specificity among those that do.
    ///
    /// Short-circuiting is only allowed where it cannot hide a later entry with a greater cascade
    /// contribution, so every entry is evaluated rather than stopping at the first match.
    pub fn match_entries(
        &self,
        program: &SelectorProgram,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<Option<SelectorEntry>, Incomplete> {
        let mut best: Option<SelectorEntry> = None;
        for entry in program.entries() {
            if self.matches_node(program, entry.root, node, counters)? {
                let better = best.is_none_or(|current| entry.specificity > current.specificity);
                if better {
                    best = Some(*entry);
                }
            }
        }
        Ok(best)
    }

    /// Whether the subject satisfies one scope instance: not excluded by its limit, and matching
    /// what the rule writes inside it.
    ///
    /// The limit is checked from the subject up to the root, which is the specification's
    /// "descendant of the scoping root and not of any scoping limit". The binding is already in
    /// place, so `:scope` in either the limit or the selector names this root.
    fn subject_is_in_scope(
        &self,
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

    /// Whether `node` matches one entry.
    pub fn matches_entry(
        &self,
        program: &SelectorProgram,
        entry: &SelectorEntry,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        self.matches_node(program, entry.root, node, counters)
    }

    /// Whether `node` matches one entry after its dispatch posting already proved `known`.
    ///
    /// Dispatch selects a subject-local feature from the entry root. Do not ask the fact store for
    /// that same feature again while materializing the posting's exact selector incidence.
    pub(super) fn matches_entry_after_dispatch(
        &self,
        program: &SelectorProgram,
        entry: &SelectorEntry,
        known: DispatchKey,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let is_known_operand = |operand| {
            matches!(program.node(operand), SelectorOp::Feature(_) | SelectorOp::State(_))
                && program.dispatch_key_of(operand) == known
        };
        if is_known_operand(entry.root) {
            return Ok(true);
        }
        let SelectorOp::And { first, count } = program.node(entry.root) else {
            return self.matches_node(program, entry.root, node, counters);
        };
        let mut skipped = false;
        for &operand in program.operands(first, count) {
            if !skipped && is_known_operand(operand) {
                skipped = true;
                continue;
            }
            if !self.matches_node(program, operand, node, counters)? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Whether `node` matches one entry, reusing transitive relation answers from other nodes
    /// evaluated against the same selector program.
    pub fn matches_entry_for_program(
        &self,
        program_id: SelectorProgramID,
        program: &SelectorProgram,
        entry: &SelectorEntry,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let previous = self.transitive_relation_program.replace(Some(program_id));
        let result = self.matches_node(program, entry.root, node, counters);
        self.transitive_relation_program.set(previous);
        result
    }

    /// Evaluate one entry without admitting its primitive and transitive relation answers to the
    /// shared program caches. Narrow exact comparisons consume the answer once, so they keep the
    /// workspace's positional geometry but avoid canonicalization and sparse-column traffic.
    pub(super) fn matches_entry_without_program_caches(
        &self,
        program: &SelectorProgram,
        entry: &SelectorEntry,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        self.matches_node(program, entry.root, node, counters)
    }

    /// Whether `node` matches one selector IR node. Routing's retained-witness check uses this to
    /// re-evaluate a simple query's compound on the one retained witness.
    pub(super) fn matches_selector_node(
        &self,
        program: &SelectorProgram,
        id: SelectorNodeID,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        self.matches_node(program, id, node, counters)
    }

    /// Match the local half of one top-down selector-prefix step.
    pub(super) fn matches_prefix_local(
        &self,
        program_id: SelectorProgramID,
        program: &SelectorProgram,
        local: SelectorPrefixLocal,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let previous = self.transitive_relation_program.replace(Some(program_id));
        let SelectorOp::And { first, count } = program.node(local.root) else {
            let result = self.matches_node(program, local.root, node, counters);
            self.transitive_relation_program.set(previous);
            return result;
        };
        let result = (|| {
            for &operand in program.operands(first, count) {
                if Some(operand) == local.relation {
                    continue;
                }
                if !self.matches_node(program, operand, node, counters)? {
                    return Ok(false);
                }
            }
            Ok(true)
        })();
        self.transitive_relation_program.set(previous);
        result
    }

    /// Whether `node` is known not to carry `key`.
    ///
    /// A node the fact store has no row for is a shadow root, which publishes nothing and answers
    /// no question about what it carries. That is not a rejection: a candidate filter that read it
    /// as one would abandon the whole document over `.wrap > .child` inside any shadow tree.
    #[must_use]
    pub fn node_cannot_carry_dispatch_key(&self, key: DispatchKey, node: StyleNodeID) -> bool {
        self.node_carries_dispatch_key(key, node).is_ok_and(|carries| !carries)
    }

    fn node_carries_dispatch_key(&self, key: DispatchKey, node: StyleNodeID) -> Result<bool, Incomplete> {
        let row = self.row_of(node)?;
        Ok(self
            .facts
            .carries_dispatch_key(row, key, self.tree.parent(node).is_none()))
    }

    #[inline]
    fn matches_compound(
        &self,
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
        &self,
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

    #[inline]
    fn matches_relation_target(
        &self,
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

    fn matches_transitive_relation(
        &self,
        program: &SelectorProgram,
        relation: SelectorNodeID,
        inner: SelectorNodeID,
        node: StyleNodeID,
        preceding_sibling: bool,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        if preceding_sibling {
            return self.matches_preceding_sibling_prefix(program, relation, inner, node, counters);
        }
        let (workspace, side) = self.match_workspace.unwrap();
        let cache = workspace.relations(side);
        let program_id = self.transitive_relation_program.get().unwrap();
        match cache.lookup(program_id, relation, node) {
            Lookup::Known(()) => return Ok(true),
            Lookup::KnownAbsent => return Ok(false),
            Lookup::Missing(_) => {}
        }

        let mut current = node;
        let mut traversed = Vec::new();
        let mut incomplete = None;
        let answer = loop {
            traversed.push(current);
            let adjacent = match preceding_sibling {
                true => self.previous_sibling_of(current),
                false => self.parent_of(current),
            };
            let Some(adjacent) = adjacent else {
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
            match cache.lookup(program_id, relation, adjacent) {
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
            cache.insert(program_id, relation, traversed_node, answer);
        }
        Ok(answer)
    }

    fn matches_preceding_sibling_prefix(
        &self,
        program: &SelectorProgram,
        relation: SelectorNodeID,
        inner: SelectorNodeID,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        let (workspace, side) = self.match_workspace.unwrap();
        let cache = workspace.relations(side);
        let program_id = self.transitive_relation_program.get().unwrap();
        let Some(parent) = self.parent_of(node) else {
            return Ok(false);
        };
        let first = self.children_of(parent).next();
        let (parent_id, cached_prefix) = cache.preceding_sibling_prefix(program_id, relation, parent);
        let mut prefix = cached_prefix.unwrap_or(PrecedingSiblingPrefix {
            next: first,
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
                cache.insert_preceding_sibling_prefix(program_id, relation, parent_id, prefix);
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
                    next: first,
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

    /// The distinct hosts a `::part()` rule can address this element from, nearest first.
    ///
    /// One per level of `exportparts` forwarding. An element that carries a part name and is
    /// forwarded nowhere has no recorded pairing at all, so the host of the tree it stands in is the
    /// only level it has - which is every part in a document using no `exportparts`.
    fn part_exposure_hosts(&self, node: StyleNodeID) -> Vec<StyleNodeID> {
        let pairs = self.tree.part_hosts_of(node);
        if pairs.is_empty() {
            return self.tree.shadow_host_of(node).into_iter().collect();
        }
        let mut hosts: Vec<StyleNodeID> = Vec::with_capacity(pairs.len());
        for &(_, host) in pairs {
            if !hosts.contains(&host) {
                hosts.push(host);
            }
        }
        hosts
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
            true => self.facts.parts_of(row).contains(&name),
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

    fn matches_node(
        &self,
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
                let tag = self.facts.language_tag_of(row);
                // An element with no resolved language matches no range at all, not even `*`.
                Ok(!tag.is_empty()
                    && program
                        .language_ranges(first, count)
                        .any(|range| crate::css::selector::language_range_matches_tag(range, tag)))
            }
            SelectorOp::State(fact) => {
                counters.bump(Counter::StateTests);
                let row = self.row_of(node)?;
                Ok(self.facts.states_of(row).contains(fact))
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
                    return self.matches_transitive_relation(program, id, inner, node, false, counters);
                }
                let mut ancestor = self.parent_of(node);
                while let Some(current) = ancestor {
                    counters.bump(Counter::CombinatorSteps);
                    if self.matches_relation_target(program, inner, current, counters)? {
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
                    return self.matches_transitive_relation(program, id, inner, node, true, counters);
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
                if position.of_selector.is_none()
                    && let Some((workspace, side)) = self.match_workspace
                    && let Some(answer) = workspace.positional_answer(position, node, side)
                {
                    return Ok(answer);
                }
                counters.bump(Counter::StructuralTests);
                let result = self.matches_nth(program, position, node, counters);
                if position.of_selector.is_none()
                    && let Some((workspace, side)) = self.match_workspace
                    && let Ok(answer) = result
                {
                    workspace.insert_positional_answer(position, node, side, answer);
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
                Ok(self.facts.parts_of(row).contains(&part))
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
                Ok(self.tree.first_element_child(node).is_none() && !self.facts.has_text_content_of(row))
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
                    ValueStateTestKind::Directionality => self.facts.directionality_of(row) == value,
                    ValueStateTestKind::CustomState => self.facts.custom_states_of(row).contains(&value),
                })
            }
            SelectorOp::Heading(levels) => {
                counters.bump(Counter::StructuralTests);
                let row = self.row_of(node)?;
                let level = self.facts.heading_level_of(row);
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
                if let Some(witnesses) = self.witnesses
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
                            witnesses.borrow_mut().retain(key, witness, self.tree);
                        }
                        (Ok(false), _) => witnesses.borrow_mut().clear(key),
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
            FeatureTest::Namespace(NamespaceTest::None) => self.facts.namespace_of(row) == StyleAtomID::NONE,
            FeatureTest::Namespace(NamespaceTest::Named(namespace)) => self.facts.namespace_of(row) == namespace,
            FeatureTest::TagName(tag) => tag.matches(self.facts.tag_of(row), self.facts.namespace_of(row)),
            FeatureTest::Id(id) => self.facts.id_of(row) == id,
            FeatureTest::Class(class) => self.facts.classes_of(row).contains(&class),
            FeatureTest::Attribute(test) => {
                let insensitive = match test.case {
                    AttributeCase::Sensitive => false,
                    AttributeCase::Insensitive => true,
                    AttributeCase::InsensitiveForNamespace(namespace) => self.facts.namespace_of(row) == namespace,
                };
                // `[*|x]` names one attribute per namespace the element carries `x` in, and the
                // test holds when any of them satisfies it.
                self.attributes_named_by(row, test)
                    .any(|attribute| self.matches_attribute_value(program, test, attribute, insensitive))
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
    fn attributes_named_by(&self, row: u32, test: AttributeTest) -> impl Iterator<Item = super::index::AttributeFact> {
        // Whether this subject folds attribute names at all, which is one namespace comparison for
        // the whole test rather than one per attribute.
        let folds = !test.fold_in_namespace.is_none() && self.facts.namespace_of(row) == test.fold_in_namespace;
        self.facts.attributes_of(row).iter().copied().filter(move |attribute| {
            let (written, folded) = match test.any_namespace {
                true => (attribute.local, attribute.folded_local),
                false => (attribute.name, attribute.folded_name),
            };
            written == test.name || (folds && folded == test.folded)
        })
    }

    fn matches_attribute_value(
        &self,
        program: &SelectorProgram,
        test: AttributeTest,
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
        let Some(value) = self.facts.text_of(attribute) else {
            return false;
        };
        match test.operator {
            AttributeOperator::Presence => true,
            AttributeOperator::Exact => equals(value, literal, insensitive),
            AttributeOperator::Includes => {
                !literal.is_empty()
                    && value
                        .split(|unit| matches!(unit, 0x20 | 0x09 | 0x0A | 0x0C | 0x0D))
                        .any(|token| equals(token, literal, insensitive))
            }
            AttributeOperator::DashMatch => {
                equals(value, literal, insensitive)
                    || (value.len() > literal.len()
                        && starts_with(value, literal, insensitive)
                        && value[literal.len()] == u16::from(b'-'))
            }
            AttributeOperator::Prefix => !literal.is_empty() && starts_with(value, literal, insensitive),
            AttributeOperator::Suffix => {
                !literal.is_empty()
                    && value.len() >= literal.len()
                    && equals(&value[value.len() - literal.len()..], literal, insensitive)
            }
            AttributeOperator::Substring => {
                !literal.is_empty()
                    && value.len() >= literal.len()
                    && (0..=value.len() - literal.len())
                        .any(|start| equals(&value[start..start + literal.len()], literal, insensitive))
            }
        }
    }

    pub(super) fn matches_nth(
        &self,
        program: &SelectorProgram,
        position: NthPosition,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        if position.of_selector.is_none()
            && let Some(index) = self.indexed_sibling_position(position, node, counters)?
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
                Some((self.facts.tag_of(row), self.facts.namespace_of(row)))
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

    /// Return a sibling position from the shared index, building that sequence on its first ask.
    ///
    /// A broad matching or exact-planning batch asks many positional selectors about the same
    /// children. Counting from an end for every `(selector, child)` pair is quadratic in the
    /// sequence length and repeated across rules. Child positions depend only on tree relations;
    /// of-type positions additionally depend on the fact side, so old and new evaluations use
    /// separate caches.
    fn indexed_sibling_position(
        &self,
        position: NthPosition,
        node: StyleNodeID,
        _counters: &mut Counters,
    ) -> Result<Option<i64>, Incomplete> {
        let Some((workspace, side)) = self.match_workspace else {
            return Ok(None);
        };
        if !position.of_type
            && let Some(cached) = self
                .transaction_fact_view
                .and_then(|(view, side)| view.sibling_positions(side, node))
        {
            return Ok(Some(i64::from(match position.from_end {
                true => cached.from_end,
                false => cached.from_start,
            })));
        }
        if let Some(cached) = workspace.sibling_position(node, side, position.of_type) {
            return Ok(Some(i64::from(match position.from_end {
                true => cached.from_end,
                false => cached.from_start,
            })));
        }

        let Some(parent) = self.parent_of(node) else {
            if position.of_type {
                self.row_of(node)?;
            }
            return Ok(Some(1));
        };
        let siblings = match self
            .transaction_fact_view
            .and_then(|(view, side)| view.sibling_sequence(side, node))
        {
            Some(siblings) => siblings,
            None => match workspace.sibling_sequence(node, side) {
                Some(siblings) => siblings,
                None => {
                    let siblings: Vec<StyleNodeID> = self.children_of(parent).collect();
                    let (siblings, _, _) = workspace.publish_sibling_sequence(siblings, side);
                    siblings
                }
            },
        };
        if siblings.is_empty() {
            return Ok(None);
        }

        if position.of_type {
            let mut type_ids = HashMap::default();
            let mut sibling_types = Vec::with_capacity(siblings.len());
            let mut totals = Vec::<u32>::new();
            for &sibling in siblings.iter() {
                let row = match self.row_of(sibling) {
                    Ok(row) => row,
                    Err(Incomplete::MissingFacts(missing)) => {
                        return Err(Incomplete::MissingSiblingFacts {
                            first: missing,
                            last_exclusive: None,
                        });
                    }
                    Err(incomplete) => return Err(incomplete),
                };
                let sibling_type = (self.facts.tag_of(row), self.facts.namespace_of(row));
                let next_type_id = u32::try_from(totals.len()).expect("sibling type space exhausted");
                let type_id = *type_ids.entry(sibling_type).or_insert_with(|| {
                    totals.push(0);
                    next_type_id
                });
                sibling_types.push(type_id);
                totals[type_id as usize] += 1;
            }
            let mut seen = vec![0_u32; totals.len()];
            for (&sibling, type_id) in siblings.iter().zip(sibling_types) {
                let type_index = type_id as usize;
                let from_start = &mut seen[type_index];
                *from_start += 1;
                workspace.insert_type_position(
                    sibling,
                    side,
                    SiblingPositions {
                        from_start: *from_start,
                        from_end: totals[type_index] - *from_start + 1,
                    },
                );
            }
        }

        let Some(cached) = workspace.sibling_position(node, side, position.of_type) else {
            return Ok(None);
        };
        Ok(Some(i64::from(match position.from_end {
            true => cached.from_end,
            false => cached.from_start,
        })))
    }

    /// Whether one sibling is counted by this positional test's sequence.
    fn counts_in_sequence(
        &self,
        program: &SelectorProgram,
        position: NthPosition,
        subject_type: Option<(StyleAtomID, StyleAtomID)>,
        sibling: StyleNodeID,
        counters: &mut Counters,
    ) -> Result<bool, Incomplete> {
        match (subject_type, position.of_selector) {
            (Some((tag, namespace)), _) => {
                let row = self.row_of(sibling)?;
                Ok(self.facts.tag_of(row) == tag && self.facts.namespace_of(row) == namespace)
            }
            (None, Some(selector)) => self.matches_node(program, selector, sibling, counters),
            (None, None) => Ok(true),
        }
    }

    fn row_of(&self, node: StyleNodeID) -> Result<u32, Incomplete> {
        self.facts.row_of(node).ok_or(Incomplete::MissingFacts(node))
    }
}

/// `index` is a 1-based position; the test is whether it equals `step * n + offset` for some
/// non-negative integer `n`.
fn matches_an_plus_b(step: i32, offset: i32, index: i64) -> bool {
    let step = i64::from(step);
    let offset = i64::from(offset);
    if step == 0 {
        return index == offset;
    }
    let difference = index - offset;
    difference % step == 0 && difference / step >= 0
}

fn fold(unit: u16, insensitive: bool) -> u16 {
    if insensitive && (u16::from(b'A')..=u16::from(b'Z')).contains(&unit) {
        return unit + u16::from(b'a' - b'A');
    }
    unit
}

fn equals(value: &[u16], literal: &[u16], insensitive: bool) -> bool {
    value.len() == literal.len()
        && value
            .iter()
            .zip(literal)
            .all(|(&left, &right)| fold(left, insensitive) == fold(right, insensitive))
}

fn starts_with(value: &[u16], literal: &[u16], insensitive: bool) -> bool {
    value.len() >= literal.len() && equals(&value[..literal.len()], literal, insensitive)
}

#[cfg(test)]
mod tests {
    use super::super::index::AttributeFact;
    use super::super::index::FeatureKey;
    use super::super::index::StateSet;
    use super::super::memory::DeviceClass;
    use super::super::memory::MemoryController;
    use super::super::relative_selector::RelativeAxis;
    use super::*;

    #[test]
    fn relation_answers_share_packed_pages_per_compiled_relation() {
        let cache = MatchRelationCache::default();
        let program = SelectorProgramID(7);
        let relation = SelectorNodeID(11);
        let first = StyleNodeID::element(1);
        let same_page = StyleNodeID::element(511);
        let next_page = StyleNodeID::element(512);
        assert_eq!(
            cache.lookup(program, relation, first),
            Lookup::Missing(MatchRelationAnswerGap {
                program,
                relation,
                node: first,
            })
        );
        cache.insert(program, relation, first, false);
        cache.insert(program, relation, same_page, true);
        cache.insert(program, relation, next_page, true);

        assert_eq!(cache.lookup(program, relation, first), Lookup::KnownAbsent);
        assert_eq!(cache.lookup(program, relation, same_page), Lookup::Known(()));
        assert_eq!(cache.lookup(program, relation, next_page), Lookup::Known(()));
        let missing_program = SelectorProgramID(8);
        assert_eq!(
            cache.lookup(missing_program, relation, first),
            Lookup::Missing(MatchRelationAnswerGap {
                program: missing_program,
                relation,
                node: first,
            })
        );
        assert!(cache.answers.borrow().capacity_bytes() > 0);
    }

    #[test]
    fn preceding_sibling_prefixes_pack_parents_by_compiled_relation() {
        let mut prefixes = PrecedingSiblingPrefixes::default();
        let program = SelectorProgramID(7);
        let relation = SelectorNodeID(11);
        let first_parent = PrecedingSiblingParentID(0);
        let second_parent = PrecedingSiblingParentID(17);
        let first = PrecedingSiblingPrefix {
            next: Some(StyleNodeID::element(3)),
            answer: false,
        };
        let replacement = PrecedingSiblingPrefix {
            next: None,
            answer: true,
        };

        assert!(prefixes.get(program, relation, first_parent).is_none());
        prefixes.insert(program, relation, second_parent, replacement);
        prefixes.insert(program, relation, first_parent, first);
        assert_eq!(prefixes.get(program, relation, first_parent).unwrap().next, first.next);
        assert!(!prefixes.get(program, relation, first_parent).unwrap().answer);
        assert!(prefixes.get(program, SelectorNodeID(12), first_parent).is_none());

        prefixes.insert(program, relation, first_parent, replacement);
        assert!(prefixes.get(program, relation, first_parent).unwrap().answer);
        assert_eq!(prefixes.get(program, relation, first_parent).unwrap().next, None);
        assert!(prefixes.capacity_bytes() > 0);
    }

    #[test]
    fn sibling_positions_share_tree_geometry_but_not_fact_sensitive_ranks() {
        let mut cache = MatchEvaluationWorkspace::default();
        let node = StyleNodeID::element(3);
        let next_page = StyleNodeID::element(64);
        let position = SiblingPositions {
            from_start: 1,
            from_end: 2,
        };

        let (sequence, pages, published) =
            cache.publish_sibling_sequence(vec![node, next_page], MatchEvaluationSide::Current);
        assert_eq!(pages, 2);
        assert!(published);
        let (reused, pages, published) =
            cache.publish_sibling_sequence(vec![node, next_page], MatchEvaluationSide::OldFacts);
        assert_eq!(pages, 0);
        assert!(!published);
        assert!(Rc::ptr_eq(&sequence, &reused));
        assert_eq!(
            cache.sibling_position(node, MatchEvaluationSide::OldFacts, false),
            Some(position)
        );
        assert_eq!(cache.sibling_position(node, MatchEvaluationSide::OldTree, false), None);
        assert_eq!(
            cache.sibling_geometry_by_tree_side[MatchEvaluationSide::Current.tree_side()]
                .borrow()
                .memberships
                .page_count(),
            2
        );

        cache.insert_type_position(node, MatchEvaluationSide::Current, position);
        assert_eq!(cache.sibling_position(node, MatchEvaluationSide::OldFacts, true), None);
        cache.insert_type_position(node, MatchEvaluationSide::OldFacts, position);
        cache.retain_current_for_matching();
        assert_eq!(
            cache.sibling_position(node, MatchEvaluationSide::Current, true),
            Some(position)
        );
        assert_eq!(cache.sibling_position(node, MatchEvaluationSide::OldFacts, true), None);
    }

    #[test]
    fn positional_answers_are_shared_across_programs() {
        let mut fixture = Fixture::new();
        let first = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 2,
                offset: 0,
                from_end: false,
                of_selector: None,
                of_type: false,
            }))
        });
        let second = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 2,
                offset: 0,
                from_end: false,
                of_selector: None,
                of_type: false,
            }))
        });
        let workspace = MatchEvaluationWorkspace::default();
        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts)
            .with_match_workspace(&workspace, MatchEvaluationSide::Current);

        assert!(
            !evaluator
                .matches_entry(&first, &first.entries()[0], fixture.nodes[1], &mut fixture.counters)
                .unwrap()
        );
        assert!(
            !evaluator
                .matches_entry(&second, &second.entries()[0], fixture.nodes[1], &mut fixture.counters)
                .unwrap()
        );
        assert_eq!(fixture.counters.get(Counter::StructuralTests), 1);
    }

    #[test]
    fn feature_answers_are_recomputed_across_programs() {
        let mut fixture = Fixture::new();
        let first = single_entry(|builder| builder.push_feature(FeatureTest::Class(CLASS_ITEM)));
        let second = single_entry(|builder| builder.push_feature(FeatureTest::Class(CLASS_ITEM)));
        let workspace = MatchEvaluationWorkspace::default();
        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts)
            .with_match_workspace(&workspace, MatchEvaluationSide::Current);

        assert!(
            evaluator
                .matches_entry_for_program(
                    SelectorProgramID(7),
                    &first,
                    &first.entries()[0],
                    fixture.nodes[1],
                    &mut fixture.counters,
                )
                .unwrap()
        );
        assert!(
            evaluator
                .matches_entry_for_program(
                    SelectorProgramID(8),
                    &second,
                    &second.entries()[0],
                    fixture.nodes[1],
                    &mut fixture.counters,
                )
                .unwrap()
        );
        assert_eq!(fixture.counters.get(Counter::LocalFeatureTests), 2);
    }

    /// A four-element document: `root > [first.item, second.item#target, third]`.
    struct Fixture {
        memory: MemoryController,
        tree: StyleNodeTree,
        facts: StyleNodeFacts,
        nodes: Vec<StyleNodeID>,
        counters: Counters,
    }

    const TAG_DIV: StyleAtomID = StyleAtomID(1);
    const TAG_SPAN: StyleAtomID = StyleAtomID(2);
    const CLASS_ITEM: StyleAtomID = StyleAtomID(10);
    const CLASS_THEME: StyleAtomID = StyleAtomID(11);
    const ID_TARGET: StyleAtomID = StyleAtomID(20);
    const ATTR_HREF: StyleAtomID = StyleAtomID(30);
    const ATTR_TYPE: StyleAtomID = StyleAtomID(31);
    const VALUE_TEXT: StyleAtomID = StyleAtomID(40);

    impl Fixture {
        fn new() -> Self {
            let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
            let mut tree = StyleNodeTree::new(&mut memory);
            let nodes: Vec<StyleNodeID> = (0..4).map(|_| tree.allocate_element(&mut memory)).collect();
            tree.set_first_element_child(nodes[0], Some(nodes[1]));
            for index in 1..4 {
                tree.set_parent(nodes[index], Some(nodes[0]));
                tree.set_next_element_sibling(nodes[index], nodes.get(index + 1).copied());
                tree.set_previous_element_sibling(nodes[index], (index > 1).then(|| nodes[index - 1]));
            }

            let mut facts = StyleNodeFacts::new();
            let href: Vec<u16> = "https://example.com/a".encode_utf16().collect();
            let (offset, length) = facts.push_text(&href);
            facts.push_row(
                nodes[0],
                TAG_DIV,
                StyleAtomID::NONE,
                StateSet::default(),
                &[CLASS_THEME],
                &[],
            );
            facts.push_row(
                nodes[1],
                TAG_DIV,
                StyleAtomID::NONE,
                StateSet::default(),
                &[CLASS_ITEM],
                &[AttributeFact {
                    folded_local: StyleAtomID::NONE,
                    folded_name: ATTR_HREF,
                    local: StyleAtomID::NONE,
                    name: ATTR_HREF,
                    value: StyleAtomID::NONE,
                    text_offset: offset,
                    text_length: length,
                }],
            );
            let mut hovered = StateSet::default();
            hovered.insert(StateFact::Hover);
            facts.push_row(
                nodes[2],
                TAG_SPAN,
                ID_TARGET,
                hovered,
                &[CLASS_ITEM],
                &[AttributeFact {
                    folded_local: StyleAtomID::NONE,
                    folded_name: ATTR_TYPE,
                    local: StyleAtomID::NONE,
                    name: ATTR_TYPE,
                    value: VALUE_TEXT,
                    text_offset: u32::MAX,
                    text_length: 0,
                }],
            );
            let row = facts.row_of(nodes[2]).unwrap();
            facts.set_row_parameters(row, StyleAtomID::NONE, StyleAtomID::NONE, 0, &[], &[StyleAtomID(77)]);
            facts.push_row(nodes[3], TAG_DIV, StyleAtomID::NONE, StateSet::default(), &[], &[]);

            Self {
                memory,
                tree,
                facts,
                nodes,
                counters: Counters::new(),
            }
        }

        fn matches(&mut self, program: &SelectorProgram, node: usize) -> bool {
            let evaluator = MatchEvaluator::new(&self.tree, &self.facts);
            evaluator
                .matches_entry(program, &program.entries()[0], self.nodes[node], &mut self.counters)
                .unwrap()
        }

        /// The same, for rules attached to a shadow tree rather than to the document.
        fn matches_in_shadow_tree(&mut self, program: &SelectorProgram, node: usize, shadow_root: usize) -> bool {
            let shadow_root = self.nodes[shadow_root];
            let evaluator = MatchEvaluator::new(&self.tree, &self.facts).in_shadow_tree(shadow_root);
            evaluator
                .matches_entry(program, &program.entries()[0], self.nodes[node], &mut self.counters)
                .unwrap()
        }
    }

    fn single_entry(build: impl FnOnce(&mut SelectorProgramBuilder) -> SelectorNodeID) -> SelectorProgram {
        let mut builder = SelectorProgramBuilder::new();
        let root = build(&mut builder);
        builder.push_entry(root);
        builder.finish()
    }

    #[test]
    fn a_compound_matches_only_when_every_operand_does() {
        // span.item#target
        let program = single_entry(|builder| {
            let tag = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let id = builder.push_feature(FeatureTest::Id(ID_TARGET));
            builder.push_compound(&[tag, class, id])
        });

        let mut fixture = Fixture::new();
        assert!(fixture.matches(&program, 2));
        assert!(!fixture.matches(&program, 1));
        assert!(!fixture.matches(&program, 3));
    }

    #[test]
    fn transpose_routing_retains_each_necessary_compound_feature() {
        // span.item#target
        let program = single_entry(|builder| {
            let tag = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let id = builder.push_feature(FeatureTest::Id(ID_TARGET));
            builder.push_compound(&[tag, class, id])
        });

        assert_eq!(
            program.subject_required_keys(0),
            vec![DispatchKey::Class(CLASS_ITEM), DispatchKey::TagName(TAG_SPAN),]
        );
    }

    #[test]
    fn transpose_routing_does_not_turn_alternatives_into_requirements() {
        // span:is(.item, .theme)
        let program = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let either_class = builder.push_any_of(&[item, theme]);
            let tag = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            builder.push_compound(&[tag, either_class])
        });

        assert_eq!(
            program.subject_required_keys(0),
            &[],
            "neither alternative is independently necessary"
        );
    }

    #[test]
    fn compound_operands_run_cheapest_first() {
        // Declared attribute-first, but the ID test is what should reject.
        let program = single_entry(|builder| {
            let (offset, length) = builder.push_literal(&"x".encode_utf16().collect::<Vec<_>>());
            let attribute = builder.push_feature(FeatureTest::Attribute(AttributeTest {
                fold_in_namespace: StyleAtomID::NONE,
                folded: ATTR_HREF,
                any_namespace: false,
                name: ATTR_HREF,
                operator: AttributeOperator::Substring,
                value_atom: StyleAtomID::NONE,
                value_offset: offset,
                value_length: length,
                case: AttributeCase::Sensitive,
            }));
            let id = builder.push_feature(FeatureTest::Id(ID_TARGET));
            builder.push_compound(&[attribute, id])
        });

        let SelectorOp::And { first, count } = program.node(program.entries()[0].root) else {
            panic!("expected a compound");
        };
        let ordered = program.operands(first, count);
        assert!(
            matches!(program.node(ordered[0]), SelectorOp::Feature(FeatureTest::Id(_))),
            "the ID test must be tried before the substring test"
        );

        let mut fixture = Fixture::new();
        // The candidate has no matching ID, so the substring test never runs.
        assert!(!fixture.matches(&program, 1));
        assert_eq!(fixture.counters.get(Counter::LocalFeatureTests), 1);
    }

    #[test]
    fn descendant_and_child_combinators_evaluate_leftwards() {
        // .theme .item
        let descendant = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, ancestor])
        });
        // .theme > .item
        let child = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let parent = builder.push(SelectorOp::Parent(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, parent])
        });

        let mut fixture = Fixture::new();
        assert!(fixture.matches(&descendant, 1));
        assert!(fixture.matches(&child, 1));
        assert!(!fixture.matches(&descendant, 0));
        assert!(!fixture.matches(&child, 3));
    }

    #[test]
    fn sibling_combinators_look_backwards_from_the_subject() {
        // .item + span
        let adjacent = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let previous = builder.push(SelectorOp::PreviousSibling(item));
            let span = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            builder.push_compound(&[span, previous])
        });
        // .item ~ div
        let general = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let preceding = builder.push(SelectorOp::PrecedingSibling(item));
            let div = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_DIV)));
            builder.push_compound(&[div, preceding])
        });

        let mut fixture = Fixture::new();
        assert!(fixture.matches(&adjacent, 2));
        assert!(!fixture.matches(&adjacent, 1));
        assert!(fixture.matches(&general, 3));
        assert!(!fixture.matches(&general, 1));
    }

    #[test]
    fn the_before_transaction_view_evaluates_multiple_adjacent_steps() {
        // .item + .item + div
        let program = single_entry(|builder| {
            let first = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let previous = builder.push(SelectorOp::PreviousSibling(first));
            let middle = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let middle = builder.push_compound(&[middle, previous]);
            let previous = builder.push(SelectorOp::PreviousSibling(middle));
            let subject = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_DIV)));
            builder.push_compound(&[subject, previous])
        });

        let mut fixture = Fixture::new();
        fixture
            .tree
            .set_next_element_sibling(fixture.nodes[1], Some(fixture.nodes[3]));
        fixture
            .tree
            .set_previous_element_sibling(fixture.nodes[3], Some(fixture.nodes[1]));
        fixture.tree.set_parent(fixture.nodes[2], None);
        fixture.tree.set_next_element_sibling(fixture.nodes[2], None);
        fixture.tree.set_previous_element_sibling(fixture.nodes[2], None);

        let mut view = TransactionFactView {
            root: fixture.nodes[0],
            moved_features: Default::default(),
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
        };
        view.insert_before_sibling_sequence(
            fixture.nodes[0],
            vec![fixture.nodes[1], fixture.nodes[2], fixture.nodes[3]],
        );
        view.finish_before_sibling_relations();
        assert_eq!(
            view.sibling_positions(TransactionFactSide::Before, fixture.nodes[2]),
            Some(SiblingPositions {
                from_start: 2,
                from_end: 2,
            })
        );
        assert_eq!(view.before_sibling_geometry.memberships.page_count(), 1);

        assert!(
            !MatchEvaluator::new(&fixture.tree, &fixture.facts)
                .matches_entry(&program, &program.entries()[0], fixture.nodes[3], &mut fixture.counters,)
                .unwrap()
        );
        assert!(
            MatchEvaluator::new(&fixture.tree, &fixture.facts)
                .with_transaction_fact_view(&view, TransactionFactSide::Before)
                .matches_entry(&program, &program.entries()[0], fixture.nodes[3], &mut fixture.counters,)
                .unwrap()
        );
    }

    #[test]
    fn a_general_sibling_miss_requests_the_remaining_sequence_prefix() {
        let program = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let preceding = builder.push(SelectorOp::PrecedingSibling(item));
            let div = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_DIV)));
            builder.push_compound(&[div, preceding])
        });
        let mut fixture = Fixture::new();
        let mut sparse = StyleNodeFacts::new();
        sparse.push_row(
            fixture.nodes[3],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[],
        );
        let evaluator = MatchEvaluator::new(&fixture.tree, &sparse);

        assert_eq!(
            evaluator.matches_entry(&program, &program.entries()[0], fixture.nodes[3], &mut fixture.counters),
            Err(Incomplete::MissingSiblingFacts {
                first: fixture.nodes[1],
                last_exclusive: Some(fixture.nodes[3]),
            })
        );
    }

    #[test]
    fn an_adjacent_sibling_miss_requests_only_the_previous_node() {
        let program = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let previous = builder.push(SelectorOp::PreviousSibling(item));
            let div = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_DIV)));
            builder.push_compound(&[div, previous])
        });
        let mut fixture = Fixture::new();
        let mut sparse = StyleNodeFacts::new();
        sparse.push_row(
            fixture.nodes[3],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[],
        );
        let evaluator = MatchEvaluator::new(&fixture.tree, &sparse);

        assert_eq!(
            evaluator.matches_entry(&program, &program.entries()[0], fixture.nodes[3], &mut fixture.counters),
            Err(Incomplete::MissingFacts(fixture.nodes[2]))
        );
    }

    #[test]
    fn a_missing_fact_below_a_sibling_names_its_descendant_range() {
        let mut fixture = Fixture::new();
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let child = fixture.tree.allocate_element(&mut memory);
        fixture.tree.set_parent(child, Some(fixture.nodes[1]));
        fixture.tree.set_first_element_child(fixture.nodes[1], Some(child));
        let program = single_entry(|builder| {
            let descendant = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let has_descendant = builder.push_relative_exists(RelativeQuery {
                axis: RelativeAxis::Descendant,
                compound: descendant,
                driving_feature: Some(FeatureKey::Class(CLASS_THEME)),
                simple: true,
                witness_is_below_the_axis: false,
                match_in_shadow_tree: false,
            });
            let preceding = builder.push(SelectorOp::PrecedingSibling(has_descendant));
            let div = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_DIV)));
            builder.push_compound(&[div, preceding])
        });
        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts);

        assert_eq!(
            evaluator.matches_entry(&program, &program.entries()[0], fixture.nodes[3], &mut fixture.counters),
            Err(Incomplete::MissingDescendantFacts {
                root: fixture.nodes[1],
                first: child,
            })
        );
    }

    #[test]
    fn a_following_sibling_query_requests_the_remaining_sequence_suffix() {
        let program = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_relative_exists(RelativeQuery {
                axis: RelativeAxis::FollowingSibling,
                compound: item,
                driving_feature: Some(FeatureKey::Class(CLASS_ITEM)),
                simple: true,
                witness_is_below_the_axis: false,
                match_in_shadow_tree: false,
            })
        });
        let mut fixture = Fixture::new();
        let mut sparse = StyleNodeFacts::new();
        sparse.push_row(
            fixture.nodes[1],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[CLASS_ITEM],
            &[],
        );
        let evaluator = MatchEvaluator::new(&fixture.tree, &sparse);

        assert_eq!(
            evaluator.matches_entry(&program, &program.entries()[0], fixture.nodes[1], &mut fixture.counters),
            Err(Incomplete::MissingSiblingFacts {
                first: fixture.nodes[2],
                last_exclusive: None,
            })
        );
    }

    #[test]
    fn attribute_operators_read_text_only_when_an_atom_cannot_answer() {
        let mut fixture = Fixture::new();

        let exact = single_entry(|builder| {
            builder.push_feature(FeatureTest::Attribute(AttributeTest {
                fold_in_namespace: StyleAtomID::NONE,
                folded: ATTR_TYPE,
                any_namespace: false,
                name: ATTR_TYPE,
                operator: AttributeOperator::Exact,
                value_atom: VALUE_TEXT,
                value_offset: 0,
                value_length: 0,
                case: AttributeCase::Sensitive,
            }))
        });
        assert!(fixture.matches(&exact, 2), "interned values compare as integers");

        let prefix = single_entry(|builder| {
            let (offset, length) = builder.push_literal(&"https://".encode_utf16().collect::<Vec<_>>());
            builder.push_feature(FeatureTest::Attribute(AttributeTest {
                fold_in_namespace: StyleAtomID::NONE,
                folded: ATTR_HREF,
                any_namespace: false,
                name: ATTR_HREF,
                operator: AttributeOperator::Prefix,
                value_atom: StyleAtomID::NONE,
                value_offset: offset,
                value_length: length,
                case: AttributeCase::Sensitive,
            }))
        });
        assert!(fixture.matches(&prefix, 1));
        assert!(!fixture.matches(&prefix, 2));

        let suffix = single_entry(|builder| {
            let (offset, length) = builder.push_literal(&"/A".encode_utf16().collect::<Vec<_>>());
            builder.push_feature(FeatureTest::Attribute(AttributeTest {
                fold_in_namespace: StyleAtomID::NONE,
                folded: ATTR_HREF,
                any_namespace: false,
                name: ATTR_HREF,
                operator: AttributeOperator::Suffix,
                value_atom: StyleAtomID::NONE,
                value_offset: offset,
                value_length: length,
                case: AttributeCase::Insensitive,
            }))
        });
        assert!(fixture.matches(&suffix, 1), "the i flag folds ASCII case");

        let presence = single_entry(|builder| {
            builder.push_feature(FeatureTest::Attribute(AttributeTest {
                fold_in_namespace: StyleAtomID::NONE,
                folded: ATTR_HREF,
                any_namespace: false,
                name: ATTR_HREF,
                operator: AttributeOperator::Presence,
                value_atom: StyleAtomID::NONE,
                value_offset: 0,
                value_length: 0,
                case: AttributeCase::Sensitive,
            }))
        });
        assert!(fixture.matches(&presence, 1));
        assert!(!fixture.matches(&presence, 3));
    }

    #[test]
    fn state_is_a_fact_like_any_other() {
        let program = single_entry(|builder| {
            let hovered = builder.push(SelectorOp::State(StateFact::Hover));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, hovered])
        });
        let mut fixture = Fixture::new();
        assert!(fixture.matches(&program, 2));
        assert!(!fixture.matches(&program, 1));
    }

    #[test]
    fn query_scope_binds_scope_to_element_root() {
        let program = single_entry(|builder| {
            let scope = builder.push(SelectorOp::ScopeRootInstance);
            let scope = builder.push(SelectorOp::Parent(scope));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[scope, item])
        });

        let mut fixture = Fixture::new();
        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts).with_scope_root(fixture.nodes[0]);
        assert!(
            evaluator
                .matches_entry(&program, &program.entries()[0], fixture.nodes[1], &mut fixture.counters)
                .unwrap()
        );

        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts).with_scope_root(fixture.nodes[1]);
        assert!(
            !evaluator
                .matches_entry(&program, &program.entries()[0], fixture.nodes[1], &mut fixture.counters)
                .unwrap()
        );
    }

    #[test]
    fn nth_child_counts_the_child_sequence() {
        let second = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 2,
                from_end: false,
                of_selector: None,
                of_type: false,
            }))
        });
        let odd = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 2,
                offset: 1,
                from_end: false,
                of_selector: None,
                of_type: false,
            }))
        });
        let last = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 1,
                from_end: true,
                of_selector: None,
                of_type: false,
            }))
        });

        let mut fixture = Fixture::new();
        assert!(!fixture.matches(&second, 1));
        assert!(fixture.matches(&second, 2));
        assert!(fixture.matches(&odd, 1));
        assert!(!fixture.matches(&odd, 2));
        assert!(fixture.matches(&odd, 3));
        assert!(fixture.matches(&last, 3));
        assert!(!fixture.matches(&last, 2));
        // The root has no child sequence.
        assert!(!fixture.matches(&second, 0));
    }

    #[test]
    fn nth_of_type_counts_only_matching_siblings() {
        let second_div = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 2,
                from_end: false,
                of_selector: None,
                of_type: true,
            }))
        });
        let mut fixture = Fixture::new();
        // Children are div, span, div: the second div is the third child.
        assert!(fixture.matches(&second_div, 3));
        assert!(!fixture.matches(&second_div, 2));
    }

    #[test]
    fn positional_misses_request_only_the_direction_the_test_counts() {
        let from_start = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 2,
                from_end: false,
                of_selector: None,
                of_type: true,
            }))
        });
        let from_end = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 2,
                from_end: true,
                of_selector: None,
                of_type: true,
            }))
        });
        let mut fixture = Fixture::new();
        let mut first_only = StyleNodeFacts::new();
        first_only.push_row(
            fixture.nodes[1],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[],
        );
        let evaluator = MatchEvaluator::new(&fixture.tree, &first_only);
        assert_eq!(
            evaluator.matches_entry(
                &from_end,
                &from_end.entries()[0],
                fixture.nodes[1],
                &mut fixture.counters
            ),
            Err(Incomplete::MissingSiblingFacts {
                first: fixture.nodes[2],
                last_exclusive: None,
            })
        );

        let mut last_only = StyleNodeFacts::new();
        last_only.push_row(
            fixture.nodes[3],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[],
        );
        let evaluator = MatchEvaluator::new(&fixture.tree, &last_only);
        assert_eq!(
            evaluator.matches_entry(
                &from_start,
                &from_start.entries()[0],
                fixture.nodes[3],
                &mut fixture.counters
            ),
            Err(Incomplete::MissingSiblingFacts {
                first: fixture.nodes[1],
                last_exclusive: Some(fixture.nodes[3]),
            })
        );
    }

    #[test]
    fn matching_reports_the_greatest_specificity_among_matching_entries() {
        let mut builder = SelectorProgramBuilder::new();
        let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
        let class_entry = builder.push_entry(class);
        builder.set_entry_specificity(
            class_entry,
            Specificity {
                classes: 1,
                ..Specificity::default()
            },
        );
        let id = builder.push_feature(FeatureTest::Id(ID_TARGET));
        let id_entry = builder.push_entry(id);
        builder.set_entry_specificity(
            id_entry,
            Specificity {
                ids: 1,
                ..Specificity::default()
            },
        );
        let program = builder.finish();

        let mut fixture = Fixture::new();
        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts);
        let matched = evaluator
            .match_entries(&program, fixture.nodes[2], &mut fixture.counters)
            .unwrap()
            .unwrap();
        assert_eq!(
            matched.specificity,
            Specificity {
                ids: 1,
                ..Specificity::default()
            },
            "an earlier lower-specificity match must not hide a later higher one"
        );
    }

    #[test]
    fn a_missing_fact_row_is_reported_rather_than_answered() {
        let program = single_entry(|builder| builder.push_feature(FeatureTest::Class(CLASS_ITEM)));
        let mut fixture = Fixture::new();
        let missing = StyleNodeID::element(99);
        let evaluator = MatchEvaluator::new(&fixture.tree, &fixture.facts);
        assert_eq!(
            evaluator.matches_entry(&program, &program.entries()[0], missing, &mut fixture.counters),
            Err(Incomplete::MissingFacts(missing))
        );
    }

    fn transpose_of(program: &SelectorProgram) -> Vec<(RoutingKey, Vec<InverseStep>)> {
        let mut collected = Vec::new();
        program.collect_transpose_entry_points(0, |site| collected.push((site.key, site.path.to_vec())));
        collected
    }

    #[test]
    fn a_descendant_selector_transposes_each_operand_to_its_own_reach() {
        // .theme .item
        let program = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, ancestor])
        });

        let collected = transpose_of(&program);
        // The subject operand reaches only the changed node itself.
        assert!(collected.contains(&(RoutingKey::Class(CLASS_ITEM), vec![])));
        // The ancestor operand reaches descendants of whatever changed.
        assert!(collected.contains(&(RoutingKey::Class(CLASS_THEME), vec![InverseStep::Descendants])));
        assert_eq!(collected.len(), 2);
    }

    #[test]
    fn each_combinator_transposes_to_its_inverse_relation() {
        let cases = [
            (
                SelectorOp::Parent as fn(SelectorNodeID) -> SelectorOp,
                InverseStep::Children,
            ),
            (SelectorOp::Ancestor, InverseStep::Descendants),
            (SelectorOp::PreviousSibling, InverseStep::NextSibling),
            (SelectorOp::PrecedingSibling, InverseStep::FollowingSiblings),
        ];
        for (combinator, expected) in cases {
            let program = single_entry(|builder| {
                let left = builder.push_feature(FeatureTest::Class(CLASS_THEME));
                let step = builder.push(combinator(left));
                let right = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
                builder.push_compound(&[right, step])
            });
            let collected = transpose_of(&program);
            assert!(
                collected.contains(&(RoutingKey::Class(CLASS_THEME), vec![expected])),
                "expected {expected:?} for the left operand"
            );
        }
    }

    #[test]
    fn a_nested_path_is_reported_in_application_order() {
        // .a > .b .c: a change to .a reaches its children, and their descendants.
        let program = single_entry(|builder| {
            let a = builder.push_feature(FeatureTest::Class(StyleAtomID(1)));
            let parent = builder.push(SelectorOp::Parent(a));
            let b = builder.push_feature(FeatureTest::Class(StyleAtomID(2)));
            let inner = builder.push_compound(&[b, parent]);
            let ancestor = builder.push(SelectorOp::Ancestor(inner));
            let c = builder.push_feature(FeatureTest::Class(StyleAtomID(3)));
            builder.push_compound(&[c, ancestor])
        });

        let collected = transpose_of(&program);
        assert!(collected.contains(&(
            RoutingKey::Class(StyleAtomID(1)),
            vec![InverseStep::Children, InverseStep::Descendants]
        )));
        assert!(collected.contains(&(RoutingKey::Class(StyleAtomID(2)), vec![InverseStep::Descendants])));
        assert!(collected.contains(&(RoutingKey::Class(StyleAtomID(3)), vec![])));
    }

    #[test]
    fn a_negated_or_hidden_operand_is_still_routed() {
        // :not(.theme):where(.item)
        let program = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let negation = builder.push(SelectorOp::Not(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let hidden = builder.push(SelectorOp::Where(item));
            builder.push_compound(&[negation, hidden])
        });

        let collected = transpose_of(&program);
        assert!(
            collected.contains(&(RoutingKey::Class(CLASS_THEME), vec![])),
            "a change inside a negation flips the subject and has to be routed"
        );
        assert!(collected.contains(&(RoutingKey::Class(CLASS_ITEM), vec![])));
    }

    #[test]
    fn a_universal_selector_routes_from_no_local_input() {
        let program = single_entry(|builder| builder.push_feature(FeatureTest::AnyElement));
        assert!(
            transpose_of(&program).is_empty(),
            "* changes only when the element itself does, which the tree delta already names"
        );
    }

    #[test]
    fn a_positional_selector_routes_structurally() {
        let program = single_entry(|builder| {
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 2,
                offset: 1,
                from_end: false,
                of_selector: None,
                of_type: false,
            }))
        });
        assert_eq!(transpose_of(&program), vec![(RoutingKey::Structural, vec![])]);
    }

    #[test]
    fn a_positional_argument_routes_through_the_child_sequence() {
        let program = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 1,
                from_end: false,
                of_selector: Some(item),
                of_type: false,
            }))
        });
        let collected = transpose_of(&program);
        assert!(collected.contains(&(RoutingKey::Structural, vec![])));
        assert!(
            collected.contains(&(RoutingKey::Class(CLASS_ITEM), vec![InverseStep::SiblingSequence])),
            "changing what counts moves indices across the whole child sequence"
        );
    }

    #[test]
    fn the_registry_returns_only_the_routes_that_mention_an_input() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut registry = RoutingRegistry::new();

        let hovered = single_entry(|builder| {
            let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let state = builder.push(SelectorOp::State(StateFact::Hover));
            builder.push_compound(&[class, state])
        });
        let unrelated = single_entry(|builder| builder.push_feature(FeatureTest::Id(ID_TARGET)));
        let sibling = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let preceding = builder.push(SelectorOp::PrecedingSibling(item));
            let target = builder.push_feature(FeatureTest::Id(StyleAtomID(4242)));
            builder.push_compound(&[target, preceding])
        });
        let relational = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let has = builder.push_relative_exists(RelativeQuery {
                axis: RelativeAxis::Descendant,
                compound: item,
                driving_feature: None,
                simple: true,
                witness_is_below_the_axis: false,
                match_in_shadow_tree: false,
            });
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            builder.push_compound(&[theme, has])
        });

        registry.add_rule(RuleID(1), SelectorProgramID(0), &hovered);
        registry.add_rule(RuleID(2), SelectorProgramID(1), &unrelated);
        registry.add_rule(RuleID(3), SelectorProgramID(2), &sibling);
        registry.add_rule(RuleID(4), SelectorProgramID(3), &relational);
        registry.settle_memory(&mut memory);

        // A hover change reaches only the rule that mentions it.
        let routed = registry.routes_for(RoutingKey::State(StateFact::Hover));
        assert_eq!(routed.len(), 1);
        assert_eq!(registry.rule_of(routed[0]), RuleID(1));
        assert_eq!(registry.path_of(routed[0]), &[]);

        // And an unmentioned input reaches nothing at all, without scanning any selector header.
        assert!(registry.routes_for(RoutingKey::Class(StyleAtomID(999))).is_empty());
        assert_eq!(registry.routes_for(RoutingKey::Id(ID_TARGET)).len(), 1);
        let arrivals = registry.arrival_routes_for(RoutingKey::Class(CLASS_ITEM));
        assert_eq!(arrivals.len(), 2);
        assert_eq!(registry.rule_of(arrivals[0]), RuleID(3));
        assert_eq!(registry.rule_of(arrivals[1]), RuleID(4));
        assert!(memory.bytes_in_category(MemoryCategory::RoutingRegistry) > 0);
    }

    #[test]
    fn the_registry_canonicalizes_equivalent_sibling_routes() {
        assert_eq!(size_of::<Option<RouteID>>(), size_of::<u32>());

        // .item.theme + #target
        let sibling = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let origin = builder.push_compound(&[item, theme]);
            let preceding = builder.push(SelectorOp::PrecedingSibling(origin));
            let target = builder.push_feature(FeatureTest::Id(ID_TARGET));
            builder.push_compound(&[target, preceding])
        });
        let mut registry = RoutingRegistry::new();
        registry.add_rule(RuleID(1), SelectorProgramID(0), &sibling);

        let item_routes = registry.routes_for(RoutingKey::Class(CLASS_ITEM));
        let theme_routes = registry.routes_for(RoutingKey::Class(CLASS_THEME));
        assert_eq!(registry.len(), 2, "one subject route and one canonical sibling route");
        assert_eq!(registry.sibling_first_routes().len(), 1);
        assert_eq!(item_routes.len(), 1);
        assert_eq!(theme_routes, item_routes);
        assert_eq!(registry.arrival_routes_for(RoutingKey::Class(CLASS_ITEM)), item_routes);
        assert_eq!(registry.arrival_routes_for(RoutingKey::Class(CLASS_THEME)), item_routes);
    }

    #[test]
    fn distinct_structural_operators_have_distinct_routes() {
        let structural = single_entry(|builder| {
            let empty = builder.push(SelectorOp::Empty);
            let first = builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 1,
                from_end: false,
                of_selector: None,
                of_type: false,
            }));
            builder.push_compound(&[empty, first])
        });
        let mut registry = RoutingRegistry::new();
        registry.add_rule(RuleID(1), SelectorProgramID(0), &structural);

        let routes = registry.routes_for(RoutingKey::Structural);
        assert_eq!(routes.len(), 2);
        assert_ne!(
            registry.route(routes[0]).structural_node,
            registry.route(routes[1]).structural_node
        );
    }

    #[test]
    fn the_running_program_total_tracks_what_the_programs_reserve() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut programs = SelectorPrograms::new();
        // Enough programs, of differing sizes, to reallocate the list several times over.
        for index in 0..64_u32 {
            let program = single_entry(|builder| {
                let mut operands = vec![builder.push_feature(FeatureTest::Class(StyleAtomID(index + 1)))];
                for extra in 0..index % 5 {
                    operands.push(builder.push_feature(FeatureTest::Class(StyleAtomID(extra + 100))));
                }
                builder.push_compound(&operands)
            });
            programs.add(program);
        }
        programs.settle_memory(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::RuleProgram),
            programs.capacity_bytes()
        );
    }

    #[test]
    fn structurally_equal_selector_programs_share_dense_ir() {
        let mut programs = SelectorPrograms::new();
        let make_program = || single_entry(|builder| builder.push_feature(FeatureTest::Class(StyleAtomID(7))));

        let first = programs.add(make_program());
        let second = programs.add(make_program());
        assert_eq!(first, second);
        assert_eq!(programs.len(), 1);

        let different = programs.add(single_entry(|builder| {
            builder.push_feature(FeatureTest::Class(StyleAtomID(8)))
        }));
        assert_ne!(first, different);
        assert_eq!(programs.len(), 2);
    }

    #[test]
    fn selector_program_slots_are_reused_after_reclamation() {
        let mut programs = SelectorPrograms::new();
        let first = programs.add(single_entry(|builder| {
            builder.push_feature(FeatureTest::Class(StyleAtomID(7)))
        }));
        let retained = programs.add(single_entry(|builder| {
            builder.push_feature(FeatureTest::Class(StyleAtomID(8)))
        }));

        let mut referenced = vec![false; programs.len()];
        referenced[retained.0 as usize] = true;
        programs.sweep_unreferenced(&referenced);
        assert_eq!(programs.get(retained).entries().len(), 1);

        let reused = programs.add(single_entry(|builder| {
            builder.push_feature(FeatureTest::Class(StyleAtomID(9)))
        }));
        assert_eq!(reused, first);
        assert_eq!(programs.len(), 2);
    }

    #[test]
    fn the_registry_stays_proportional_to_selector_input_incidence() {
        let mut registry = RoutingRegistry::new();
        for index in 0..200_u32 {
            let program = single_entry(|builder| {
                let class = builder.push_feature(FeatureTest::Class(StyleAtomID(1000 + index)));
                let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
                let ancestor = builder.push(SelectorOp::Ancestor(theme));
                builder.push_compound(&[class, ancestor])
            });
            registry.add_rule(RuleID(index), SelectorProgramID(index), &program);
        }

        // Two inputs per rule, and the shared ancestor class collects all two hundred entries.
        assert_eq!(registry.len(), 400);
        assert_eq!(registry.routes_for(RoutingKey::Class(CLASS_THEME)).len(), 200);
        assert_eq!(registry.routes_for(RoutingKey::Class(StyleAtomID(1007))).len(), 1);
        assert_eq!(registry.routed_input_count(), 201);
    }

    #[test]
    fn a_relational_selector_answers_existentially_and_stops_at_the_first_witness() {
        // .theme:has(> .item)
        let program = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let has = builder.push_relative_exists(RelativeQuery {
                axis: RelativeAxis::Child,
                compound: item,
                driving_feature: None,
                simple: true,
                witness_is_below_the_axis: false,
                match_in_shadow_tree: false,
            });
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            builder.push_compound(&[theme, has])
        });

        let mut fixture = Fixture::new();
        // The root carries .theme and has two .item children; the first one answers it.
        assert!(fixture.matches(&program, 0));
        assert!(!fixture.matches(&program, 1), "an .item is not itself a .theme");
    }

    #[test]
    fn a_relational_selector_with_no_witness_is_false() {
        // :has(> #nothing)
        let program = single_entry(|builder| {
            let id = builder.push_feature(FeatureTest::Id(StyleAtomID(4242)));
            builder.push_relative_exists(RelativeQuery {
                axis: RelativeAxis::Child,
                compound: id,
                driving_feature: None,
                simple: true,
                witness_is_below_the_axis: false,
                match_in_shadow_tree: false,
            })
        });
        let mut fixture = Fixture::new();
        assert!(!fixture.matches(&program, 0));
    }

    #[test]
    fn a_relational_selector_transposes_witnesses_to_their_anchors() {
        // .card:has(+ .selected)
        let program = single_entry(|builder| {
            let selected = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let has = builder.push_relative_exists(RelativeQuery {
                axis: RelativeAxis::NextSibling,
                compound: selected,
                driving_feature: Some(super::super::index::FeatureKey::Class(CLASS_ITEM)),
                simple: true,
                witness_is_below_the_axis: false,
                match_in_shadow_tree: false,
            });
            let card = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            builder.push_compound(&[card, has])
        });

        let mut collected = Vec::new();
        program.collect_transpose_entry_points(0, |site| {
            collected.push((site.key, site.path.to_vec(), site.anchor));
        });

        // The witness records its anchor step separately rather than folding it into the path, so
        // routing can resolve the anchors before continuing.
        let witness = collected
            .iter()
            .find(|(key, _, _)| *key == RoutingKey::Class(CLASS_ITEM))
            .expect("the witness compound is routed");
        assert_eq!(
            witness.1,
            Vec::<InverseStep>::new(),
            "the anchor step is not in the path"
        );
        let anchor = witness.2.expect("a witness carries its anchor");
        assert_eq!(anchor.axis, RelativeAxis::NextSibling);
        assert_eq!(anchor.anchor_dispatch, DispatchKey::Class(CLASS_THEME));

        assert!(
            collected
                .iter()
                .any(|(key, path, anchor)| *key == RoutingKey::Class(CLASS_THEME)
                    && path.is_empty()
                    && anchor.is_none())
        );
    }

    #[test]
    fn shadow_operators_consume_the_relation_they_name() {
        let mut fixture = Fixture::new();
        let host = fixture.nodes[0];
        let slotted = fixture.nodes[1];
        let shadow_root = fixture.nodes[3];
        fixture.tree.set_shadow_root(host, shadow_root, &mut fixture.memory);
        fixture
            .tree
            .set_assigned_slot(slotted, Some(fixture.nodes[2]), &mut fixture.memory);
        fixture
            .tree
            .set_part_hosts(fixture.nodes[2], &[(StyleAtomID(77), host)], &mut fixture.memory);

        let host_rule = single_entry(|builder| {
            let any = builder.push_feature(FeatureTest::AnyElement);
            builder.push(SelectorOp::Host(any))
        });
        assert!(
            fixture.matches_in_shadow_tree(&host_rule, 0, 3),
            "the host of the tree the rule is in is the host"
        );
        assert!(
            !fixture.matches_in_shadow_tree(&host_rule, 1, 3),
            "a DOM child of the host is not"
        );
        assert!(
            !fixture.matches(&host_rule, 0),
            "and a rule in the document scope names no host at all"
        );

        let slotted_rule = single_entry(|builder| {
            let any = builder.push_feature(FeatureTest::AnyElement);
            builder.push(SelectorOp::Slotted(any))
        });
        assert!(fixture.matches(&slotted_rule, 1));
        assert!(!fixture.matches(&slotted_rule, 3), "an unassigned node is not slotted");

        let part_rule = single_entry(|builder| builder.push(SelectorOp::Part(StyleAtomID(77))));
        assert!(fixture.matches(&part_rule, 2));
        assert!(!fixture.matches(&part_rule, 1));
    }

    #[test]
    fn a_generic_descendant_walk_does_not_pierce_a_shadow_root() {
        let mut fixture = Fixture::new();
        fixture
            .tree
            .set_shadow_root(fixture.nodes[0], fixture.nodes[3], &mut fixture.memory);

        // .theme .item still means DOM ancestry; hosting changes nothing about it.
        let program = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, ancestor])
        });
        assert!(fixture.matches(&program, 1));
    }

    #[test]
    fn a_shadow_guard_on_the_subject_transposes_to_the_subject() {
        // `:host(.theme)` and `::slotted(.item)` style the element they test. The match program
        // evaluates them on the subject itself, so a change to what they test reaches that element
        // and nothing else: taking the relation as a step would name the hosted tree or the slot,
        // neither of which is the subject.
        let host = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            builder.push(SelectorOp::Host(theme))
        });
        assert_eq!(transpose_of(&host), vec![(RoutingKey::Class(CLASS_THEME), vec![])]);

        let slotted = single_entry(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push(SelectorOp::Slotted(item))
        });
        assert_eq!(transpose_of(&slotted), vec![(RoutingKey::Class(CLASS_ITEM), vec![])]);

        let part = single_entry(|builder| builder.push(SelectorOp::Part(StyleAtomID(77))));
        assert_eq!(transpose_of(&part), vec![(RoutingKey::Part(StyleAtomID(77)), vec![])]);
    }

    #[test]
    fn a_shadow_guard_behind_a_combinator_transposes_through_its_own_relation() {
        // `:host(.theme) .item` puts the subject inside the hosted tree, which no DOM-ancestry step
        // reaches. The step to the hosted tree is what carries the change across the boundary.
        let host = single_entry(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let guard = builder.push(SelectorOp::Host(theme));
            let ancestor = builder.push_ancestor(guard);
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, ancestor])
        });
        assert!(
            transpose_of(&host).contains(&(
                RoutingKey::Class(CLASS_THEME),
                vec![InverseStep::HostedTree, InverseStep::Descendants]
            )),
            "a host change reaches the tree it hosts, not its DOM descendants"
        );
    }

    #[test]
    fn a_nested_shadow_guard_carries_its_compound_through_the_host() {
        let host = single_entry(|builder| {
            let any = builder.push_feature(FeatureTest::AnyElement);
            let host = builder.push(SelectorOp::Host(any));
            let alternate = builder.push_feature(FeatureTest::Class(StyleAtomID(202)));
            let host_or_alternate = builder.push_any_of(&[host, alternate]);
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let guard = builder.push_compound(&[host_or_alternate, theme]);
            let ancestor = builder.push_ancestor(guard);
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_compound(&[item, ancestor])
        });
        assert!(
            transpose_of(&host).contains(&(
                RoutingKey::Class(CLASS_THEME),
                vec![InverseStep::HostedTree, InverseStep::Descendants]
            )),
            "a host nested in :is() carries its compound across the boundary"
        );
    }

    #[test]
    fn an_plus_b_handles_the_boundary_forms() {
        assert!(matches_an_plus_b(0, 1, 1));
        assert!(!matches_an_plus_b(0, 1, 2));
        assert!(matches_an_plus_b(2, 0, 2));
        assert!(!matches_an_plus_b(2, 0, 3));
        assert!(matches_an_plus_b(2, 1, 1));
        assert!(matches_an_plus_b(-1, 3, 3));
        assert!(matches_an_plus_b(-1, 3, 1));
        assert!(!matches_an_plus_b(-1, 3, 4));
        assert!(matches_an_plus_b(1, 0, 5));
    }

    #[test]
    fn an_plus_b_range_changes_are_decided_without_walking_the_range() {
        let position = |step, offset| NthPosition {
            step,
            offset,
            from_end: false,
            of_selector: None,
            of_type: false,
        };

        assert!(!position(3, 0).could_change_from_range(1, 1, 2));
        assert!(position(3, 0).could_change_from_range(2, 2, 3));
        assert!(position(3, 0).could_change_from_range(3, 2, 3));
        assert!(!position(1, 3).could_change_from_range(5, 3, 8));
        assert!(position(1, 3).could_change_from_range(3, 1, 3));
        assert!(!position(-1, 3).could_change_from_range(2, 1, 3));
        assert!(position(-1, 3).could_change_from_range(3, 3, 4));
        assert!(!position(1_000_000_000, 500_000_000).could_change_from_range(1, 1, 499_999_999));
        assert!(position(1_000_000_000, 500_000_000).could_change_from_range(1, 1, 500_000_000));
    }
}
