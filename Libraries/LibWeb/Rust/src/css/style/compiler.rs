/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiling parsed selectors into StyleEngine programs.
//!
//! The parser already produces a compiled selector for the matching engine, so this is a Rust-to-
//! Rust translation into the logical IR: the same selector, expressed as constraints on its
//! subject rather than as a list of compounds to walk.
//!
//! Compilation markers are bookkeeping only: they feed counters and do not change matching or
//! routing. The emitted IR carries the behavior, whether that is an exact never-match, an operator
//! handled elsewhere in the IR, or a conservative shape for something not implemented here. The
//! marker names which of those happened so counter data does not confuse an exact result with a
//! widened one.
//!
//! Names cross as atoms. A parsed selector already carries the one-word interned identity of every
//! name it mentions, and that identity is what the atom table is keyed by, so compiling a selector
//! copies no strings. Only attribute *values* tested by a substring or token operator become
//! literals, because those are the tests an integer comparison cannot answer.

use super::tree::StyleNodeID;
use crate::css::selector::AttributeCaseType;
use crate::css::selector::AttributeMatchType;
use crate::css::selector::AttributeSelector;
use crate::css::selector::Combinator;
use crate::css::selector::CompiledSelector;
use crate::css::selector::CompoundSelector;
use crate::css::selector::Direction;
use crate::css::selector::NamespaceType;
use crate::css::selector::PseudoClassSelector;
use crate::css::selector::PseudoClassType;
use crate::css::selector::PseudoElementSelector;
use crate::css::selector::PseudoElementType;
use crate::css::selector::PseudoElementValue;
use crate::css::selector::SimpleSelector;

use super::atoms::synthetic_text_atom_key;
use super::fnv::fnv1a64;
use super::index::StyleAtomID;
use super::relative_selector::RelativeAxis;
use super::relative_selector::RelativeQuery;
use super::selector::AttributeCase;
use super::selector::AttributeOperator;
use super::selector::AttributeTest;
use super::selector::FeatureTest;
use super::selector::NamespaceTest;
use super::selector::NthPosition;
use super::selector::SelectorNodeID;
use super::selector::SelectorOp;
use super::selector::SelectorProgram;
use super::selector::SelectorProgramBuilder;
use super::selector::TagTest;
use super::selector::ValueStateTestKind;
use super::transaction::StateFact;
use super::tree::PseudoElementKind;
use super::tree::PseudoElementTarget;

/// The counter category associated with a compilation marker.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompilationMarkerReason {
    /// A pseudo-class whose parsed value cannot match.
    PseudoClass,
    /// A name with no interned identity, so it has no atom to compare against.
    UninternedName,
    /// A nesting or invalid selector, which has no meaning outside its parse context.
    Malformed,
    /// A combinator with known never-match behavior.
    Combinator,
    /// A pseudo-element value that cannot match.
    PseudoElement,
    /// A relative selector with a selector list or more than one axis. This is the specification's
    /// *complex* relative selector: exact evaluation, no retained witness, and routing that widens
    /// rather than naming one inverse axis. It is a handled shape, not an unhandled one.
    ComplexRelative,
}

impl CompilationMarkerReason {
    /// The counter this reason reports through.
    #[must_use]
    pub fn counter(self) -> Option<super::instrumentation::Counter> {
        use super::instrumentation::Counter;
        Some(match self {
            Self::PseudoClass => Counter::UnsupportedPseudoClass,
            Self::UninternedName => Counter::UnsupportedUninternedName,
            Self::Malformed => Counter::UnsupportedMalformed,
            Self::Combinator => Counter::UnsupportedCombinator,
            Self::PseudoElement => Counter::UnsupportedPseudoElement,
            Self::ComplexRelative => return None,
        })
    }
}

/// How the emitted IR handles a marked selector construct.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompilationMarker {
    /// The construct is known to match nothing and the IR says so exactly.
    KnownNeverMatches(CompilationMarkerReason),
    /// Another IR operator evaluates the construct exactly.
    HandledByOtherIR(CompilationMarkerReason),
}

impl CompilationMarker {
    /// The reason-specific counter this marker reports through.
    #[must_use]
    pub fn counter(self) -> Option<super::instrumentation::Counter> {
        match self {
            Self::KnownNeverMatches(reason) | Self::HandledByOtherIR(reason) => reason.counter(),
        }
    }
}

/// The outcome of compiling one selector.
pub struct CompiledEntry {
    pub entry: usize,
    /// Bookkeeping for a construct whose treatment should remain visible in counters.
    pub marker: Option<CompilationMarker>,
}

/// The `@scope` rules a selector is compiled inside, outermost first.
///
/// Scopes nest, and an element is in scope only when it is in every one of them, so `levels` says
/// how many of `roots` and `limits` each enclosing scope contributed: without the boundaries the
/// roots of two levels read as alternatives rather than as constraints of their own.
/// Where an `@scope` with no `<scope-start>` roots.
///
/// The specification's answer is the parent element of the sheet's owner node, which is an element
/// rather than a selector, so the caller resolves it. When the sheet has no owner node - a
/// constructed sheet - the answer is the shadow host or the root of the containing node tree, and
/// which of those holds is a property of the scope the sheet is asked in rather than of the sheet,
/// so it is left to matching.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImplicitScopeRoot {
    Node(StyleNodeID),
    ContainingTree,
}

#[derive(Default)]
pub struct ScopeChain<'a> {
    pub roots: &'a [&'a CompiledSelector],
    pub limits: &'a [&'a CompiledSelector],
    pub levels: &'a [(u32, u32)],
    /// Where each level roots when it wrote no `<scope-start>`, one per level and `None` where the
    /// level wrote one.
    pub implicit_roots: &'a [Option<ImplicitScopeRoot>],
}

/// The `@namespace` declarations in scope for the selectors of one sheet.
///
/// A prefix means whatever its sheet says it means, and an unprefixed type or universal selector
/// means the sheet's default namespace where one is declared. Both are CSSOM state, so they are
/// resolved to atoms by the caller and looked up here.
#[derive(Default)]
pub struct NamespaceScope {
    /// The sheet's default namespace, or `None` when it declared none - in which case an
    /// unprefixed name constrains no namespace at all.
    pub default: Option<StyleAtomID>,
    /// Each declared prefix and the namespace it names.
    pub by_prefix: Vec<(StyleAtomID, StyleAtomID)>,
}

impl NamespaceScope {
    /// A declared namespace name, as a constraint. The empty string is the namespace an element in
    /// no namespace has, so declaring it names exactly those.
    #[must_use]
    fn test_for_uri(uri: StyleAtomID) -> NamespaceTest {
        match uri == StyleAtomID::NONE {
            true => NamespaceTest::None,
            false => NamespaceTest::Named(uri),
        }
    }

    /// The constraint a qualified name places on its subject's namespace.
    #[must_use]
    fn test_for(&self, namespace_type: NamespaceType, prefix: Option<StyleAtomID>) -> Option<NamespaceTest> {
        match namespace_type {
            // `*|x` names every namespace, so it constrains nothing.
            NamespaceType::Any => None,
            // `|x` names the absence of one.
            NamespaceType::None => Some(NamespaceTest::None),
            NamespaceType::Default => self.default.map(Self::test_for_uri),
            // A prefix the sheet never declared makes the selector invalid, which the parser has
            // already rejected; nothing reaches here with one.
            NamespaceType::Named => prefix
                .and_then(|prefix| self.by_prefix.iter().find(|(declared, _)| *declared == prefix))
                .map(|&(_, uri)| Self::test_for_uri(uri)),
        }
    }
}

/// Compiles parsed selectors into one program, interning names through the document's atom table.
pub struct SelectorCompiler<'a> {
    builder: SelectorProgramBuilder,
    /// The document-local atom for a name, optionally qualified by a namespace. A qualified name is
    /// a name of its own: `[ns|x]` names an attribute that `[x]` does not, and one element can
    /// carry both.
    intern: &'a mut dyn FnMut(usize, Option<StyleAtomID>) -> StyleAtomID,
    /// Whether the document matches id and class selectors ASCII case-insensitively, which a
    /// quirks-mode one does. Both a selector's name and an element's are keyed by their lowercase
    /// folding there, so the two meet however either was written.
    fold_id_and_class_name_case: bool,
    /// The HTML namespace, when this is an HTML document, and none otherwise. A handful of attribute
    /// names compare their values ASCII case-insensitively on an HTML element in an HTML document
    /// and case-sensitively anywhere else, so the answer is a fact about the subject.
    html_element_namespace: StyleAtomID,
    /// The `@namespace` declarations of the sheet these selectors belong to.
    namespaces: NamespaceScope,
    /// Whether the selector being compiled reaches across a shadow boundary, where the sheet's
    /// default namespace does not follow it.
    selector_escapes_its_namespace: bool,
    /// Whether a scoping root is bound where the selector being compiled sits. `:scope` names that
    /// root rather than the root of the tree, and only the evaluation knows which element it is.
    scope_root_is_bound: bool,
    /// Whether the selector being compiled names `:scope`. A scoped selector that does not is
    /// relative to the root and so cannot match it; one that does says where it means to match.
    selector_names_the_scope: bool,
    /// Whether the compound being compiled names the shadow host. A `:has()` in such a compound is
    /// anchored on the host and reads the tree the host opens; see
    /// `RelativeQuery::match_in_shadow_tree`. It is a property of the whole compound rather than of
    /// the order its simple selectors were written in, so `:has(.x):host` says the same as
    /// `:host:has(.x)`.
    compound_names_the_host: bool,
    /// Whether the current compound is the subject of a selector nested in a logical pseudo-class.
    /// A sheet's default namespace does not constrain that subject unless it writes a type or
    /// universal selector of its own.
    logical_pseudo_subject: Option<*const CompoundSelector>,
}

impl<'a> SelectorCompiler<'a> {
    #[must_use]
    pub fn new(
        intern: &'a mut dyn FnMut(usize, Option<StyleAtomID>) -> StyleAtomID,
        fold_id_and_class_name_case: bool,
        html_element_namespace: StyleAtomID,
        namespaces: NamespaceScope,
    ) -> Self {
        Self {
            builder: SelectorProgramBuilder::new(),
            intern,
            fold_id_and_class_name_case,
            html_element_namespace,
            namespaces,
            selector_escapes_its_namespace: false,
            scope_root_is_bound: false,
            selector_names_the_scope: false,
            compound_names_the_host: false,
            logical_pseudo_subject: None,
        }
    }

    #[must_use]
    pub fn finish(self) -> SelectorProgram {
        self.builder.finish()
    }

    /// Compile one complex selector as a top-level entry of the program.
    /// Compile one complex selector, conjoined with the scope its rule sits in.
    ///
    /// A scoped rule matches only inside its scope, so the scope's start selector is a constraint on
    /// the subject like any other: the subject is that element or a descendant of it. Expressing it
    /// that way is what makes a class change that moves an element in or out of a scope route -
    /// otherwise the scope's selectors appear in no compiled entry and reach nothing.
    ///
    /// The scope's `to` bound is not expressed. It can only narrow which elements match, so leaving
    /// it out keeps the entry a superset, which is the safe direction for invalidation and is what
    /// the exact evaluator settles afterwards.
    pub fn compile_in_scope(&mut self, selector: &CompiledSelector, scope: &ScopeChain<'_>) -> CompiledEntry {
        let (scope_roots, scope_limits, scope_levels) = (scope.roots, scope.limits, scope.levels);
        let implicit_root_of = |level: usize| scope.implicit_roots.get(level).copied().flatten();
        if scope_levels.is_empty()
            && scope_roots.is_empty()
            && scope_limits.is_empty()
            && scope.implicit_roots.is_empty()
        {
            return self.compile(selector);
        }
        let mut marker = None;

        // Scopes nest, and an element is in scope only when it is in every one of them, so each
        // enclosing scope is a constraint of its own. Within one scope the roots are alternatives.
        // An empty level list means one scope holding everything, which is what a caller with a
        // single `@scope` passes.
        let levels: Vec<(usize, usize, usize, usize)> = if scope_levels.is_empty() {
            vec![(0, scope_roots.len(), 0, scope_limits.len())]
        } else {
            let mut spans = Vec::with_capacity(scope_levels.len());
            let mut root_start = 0usize;
            let mut limit_start = 0usize;
            for &(root_count, limit_count) in scope_levels {
                let root_end = (root_start + root_count as usize).min(scope_roots.len());
                let limit_end = (limit_start + limit_count as usize).min(scope_limits.len());
                spans.push((root_start, root_end, limit_start, limit_end));
                root_start = root_end;
                limit_start = limit_end;
            }
            spans
        };

        // https://drafts.csswg.org/css-cascade-6/#scoped-rules
        // The rule is relative to *one* scoping root, which the evaluation binds: `:scope` names
        // that element, a limit excludes relative to it, and proximity counts to it. So the scopes
        // wrap what the rule wrote rather than conjoining constraints onto it, innermost first, and
        // `:scope` anywhere inside compiles to the binding.
        let outer_bound = std::mem::replace(&mut self.scope_root_is_bound, true);
        let outer_names_the_scope = std::mem::replace(&mut self.selector_names_the_scope, false);
        let mut entry = self.compile(selector);
        let selector_names_the_scope = self.selector_names_the_scope;
        self.scope_root_is_bound = outer_bound;
        self.selector_names_the_scope = outer_names_the_scope;

        let mut inner = self.builder.entry_root(entry.entry);
        let mut innermost_root = None;
        for (level, &(root_start, root_end, limit_start, limit_end)) in levels.iter().enumerate().rev() {
            let innermost = level + 1 == levels.len();
            let mut roots = Vec::with_capacity(root_end - root_start + 1);
            if let Some(implicit) = implicit_root_of(level) {
                roots.push(self.push_implicit_scope_root(implicit));
            }
            for root_selector in &scope_roots[root_start..root_end] {
                let compounds = &root_selector.compound_selectors;
                if compounds.is_empty() {
                    marker.get_or_insert(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed));
                    continue;
                }
                // A `<scope-start>` is written outside the scope it opens, so `:scope` in one names
                // the scope enclosing it - which is bound while this level's candidates are tested.
                // The outermost level has none unless this compile is itself inside a scope.
                let bound = std::mem::replace(&mut self.scope_root_is_bound, level > 0 || outer_bound);
                roots.push(self.compile_chain(compounds, compounds.len() - 1, &mut marker));
                self.scope_root_is_bound = bound;
            }
            // An explicit `<scope-start>` whose selectors all failed parent substitution matches
            // no roots. An omitted start is represented by an implicit root above.
            if roots.is_empty() {
                marker.get_or_insert(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed));
                inner = self.builder.push_never();
                break;
            }
            let root = self.builder.push_any_of(&roots);

            // `:scope` inside a `<scope-end>` names the root of the scope that end belongs to,
            // which is this level's own.
            let mut limits = Vec::with_capacity(limit_end - limit_start);
            let bound = std::mem::replace(&mut self.scope_root_is_bound, true);
            for limit_selector in &scope_limits[limit_start..limit_end] {
                let compounds = &limit_selector.compound_selectors;
                if compounds.is_empty() {
                    marker.get_or_insert(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed));
                    continue;
                }
                limits.push(self.compile_chain(compounds, compounds.len() - 1, &mut marker));
            }
            self.scope_root_is_bound = bound;
            let limit = (!limits.is_empty()).then(|| self.builder.push_any_of(&limits));

            // A scoped selector carries an implied `:scope ` prefix unless it names `:scope`, so
            // the subject is a strict descendant of the innermost root. An enclosing scope is
            // satisfied by the subject being anywhere inside it, its root included.
            inner = self.builder.push(SelectorOp::InScope {
                root,
                limit,
                inner,
                names_the_scope: !innermost || selector_names_the_scope,
            });
            if innermost_root.is_none() {
                innermost_root = Some(root);
            }
        }
        self.builder.set_entry_root(entry.entry, inner);
        if let Some(scope_root) = innermost_root {
            // Proximity is measured to the nearest scoping root, which is the innermost.
            self.builder.set_entry_scope_root(entry.entry, scope_root);
        }

        if marker.is_some() && entry.marker.is_none() {
            entry.marker = marker;
        }
        entry
    }

    /// The node an `@scope` with no `<scope-start>` roots at.
    ///
    /// A named element is one identity comparison. A sheet with no owner node roots at the shadow
    /// host of the tree it is asked in, or at that tree's root when it is not a shadow tree, which
    /// is `:host` and `:root` answering for whichever holds.
    fn push_implicit_scope_root(&mut self, implicit: ImplicitScopeRoot) -> SelectorNodeID {
        match implicit {
            ImplicitScopeRoot::Node(node) => self.builder.push_is_node(node),
            ImplicitScopeRoot::ContainingTree => {
                let anything = self.builder.push_feature(FeatureTest::AnyElement);
                let host = self.builder.push(SelectorOp::Host(anything));
                let root = self.builder.push(SelectorOp::Root);
                self.builder.push_any_of(&[host, root])
            }
        }
    }

    pub fn compile(&mut self, selector: &CompiledSelector) -> CompiledEntry {
        // `::slotted()` and `::part()` name an element in another tree, which the sheet's default
        // namespace says nothing about.
        self.selector_escapes_its_namespace = selector.compound_selectors.iter().any(|compound| {
            compound.simple_selectors.iter().any(|simple| {
                matches!(simple, SimpleSelector::PseudoElement(pseudo_element)
                if matches!(
                    pseudo_element.pseudo_element,
                    PseudoElementType::Slotted | PseudoElementType::Part
                ))
            })
        });
        let compounds = &selector.compound_selectors;
        if compounds.is_empty() {
            let never = self.builder.push_never();
            let entry = self.builder.push_entry(never);
            return CompiledEntry {
                entry,
                marker: Some(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed)),
            };
        }

        let mut marker = None;
        let root = self.compile_chain(compounds, compounds.len() - 1, &mut marker);
        let pseudo_element = selector
            .target_pseudo_element
            .map(|kind| PseudoElementTarget::new(PseudoElementKind(kind as u16)));
        let entry = self.builder.push_entry_for_pseudo(root, pseudo_element);
        self.builder.set_entry_specificity(entry, selector.specificity());

        CompiledEntry { entry, marker }
    }

    pub fn compile_for_query(&mut self, selector: &CompiledSelector) -> CompiledEntry {
        let outer_bound = std::mem::replace(&mut self.scope_root_is_bound, true);
        let entry = self.compile(selector);
        self.scope_root_is_bound = outer_bound;
        // https://dom.spec.whatwg.org/#scope-match-a-selectors-string
        // NB: A selector query matches elements, never pseudo-elements. `::slotted()` is represented
        //     as an operator on its assigned element for style matching, so it needs this explicit
        //     query rejection rather than the entry's ordinary pseudo-element target check.
        if selector.compound_selectors.iter().any(|compound| {
            compound
                .simple_selectors
                .iter()
                .any(|simple| matches!(simple, SimpleSelector::PseudoElement(_)))
        }) {
            let never = self.builder.push_never();
            self.builder.set_entry_root(entry.entry, never);
        }
        entry
    }

    /// Compile compounds `0..=index` with the subject at `index`.
    ///
    /// A compound's combinator relates it to the compound on its left, so walking leftwards from
    /// the subject turns each one into a constraint on that subject.
    /// Whether this compound is the `::part()` half of a selector.
    fn names_a_part(compound: &CompoundSelector) -> bool {
        compound.simple_selectors.iter().any(|simple| {
            matches!(simple, SimpleSelector::PseudoElement(pseudo_element)
                if pseudo_element.pseudo_element == PseudoElementType::Part)
        })
    }

    fn names_slotted(compound: &CompoundSelector) -> bool {
        compound.simple_selectors.iter().any(|simple| {
            matches!(simple, SimpleSelector::PseudoElement(pseudo_element)
                if pseudo_element.pseudo_element == PseudoElementType::Slotted)
        })
    }

    fn compile_chain(
        &mut self,
        compounds: &[CompoundSelector],
        index: usize,
        marker: &mut Option<CompilationMarker>,
    ) -> SelectorNodeID {
        self.compile_chain_anchored(compounds, index, marker, None)
    }

    fn compile_chain_with_subject(
        &mut self,
        compounds: &[CompoundSelector],
        index: usize,
        marker: &mut Option<CompilationMarker>,
        subject: *const CompoundSelector,
    ) -> SelectorNodeID {
        let outer_subject = self.logical_pseudo_subject.replace(subject);
        let result = self.compile_chain(compounds, index, marker);
        self.logical_pseudo_subject = outer_subject;
        result
    }

    /// The same, with `leftmost` conjoined onto the compound the chain starts at.
    ///
    /// A relative selector uses this to say where its chain started, which the constraints on the
    /// witness cannot say on their own.
    fn compile_chain_anchored(
        &mut self,
        compounds: &[CompoundSelector],
        index: usize,
        marker: &mut Option<CompilationMarker>,
        leftmost: Option<SelectorNodeID>,
    ) -> SelectorNodeID {
        // `X::part(p)` reaches the engine as two compounds joined by the pseudo-element combinator,
        // and unlike every other pseudo-element the two sides describe different elements: `X` is the
        // shadow host and `::part(p)` is an element inside the tree it hosts. Conjoining them would
        // ask the part element to carry the host's own features, so the host is compiled first and
        // becomes the guard it belongs inside.
        if index > 0
            && compounds[index].combinator == Combinator::PseudoElement
            && Self::names_a_part(&compounds[index])
        {
            let host = self.compile_chain_anchored(compounds, index - 1, marker, leftmost);
            return self.compile_compound_hosted_by(&compounds[index], Some(host), marker);
        }

        // `X::slotted(y)` is the same shape the other way round: `X` describes the slot and
        // `::slotted(y)` the element assigned to it, which is the subject. Conjoining them would ask
        // the assigned element to be a slot.
        if index > 0
            && compounds[index].combinator == Combinator::PseudoElement
            && Self::names_slotted(&compounds[index])
        {
            let slot = self.compile_chain_anchored(compounds, index - 1, marker, leftmost);
            let compound = self.compile_compound(&compounds[index], marker);
            let step = self.builder.push(SelectorOp::AssignedSlot(slot));
            return self.builder.push_compound(&[compound, step]);
        }

        let compound = self.compile_compound(&compounds[index], marker);
        if index == 0 {
            return match leftmost {
                Some(guard) => self.builder.push_compound(&[compound, guard]),
                None => compound,
            };
        }

        let left = self.compile_chain_anchored(compounds, index - 1, marker, leftmost);
        let constraint = match compounds[index].combinator {
            Combinator::Descendant => SelectorOp::Ancestor(left),
            Combinator::ImmediateChild => SelectorOp::Parent(left),
            Combinator::NextSibling => SelectorOp::PreviousSibling(left),
            Combinator::SubsequentSibling => SelectorOp::PrecedingSibling(left),
            // A pseudo-element combinator is not a tree step: it moves from an element to a
            // pseudo-element that element originates. The entry already carries the target, and
            // both sides constrain the same originating element, so the two compounds simply
            // conjoin.
            Combinator::PseudoElement => return self.builder.push_compound(&[compound, left]),
            // No shipping engine implements column combinator matching, so it matches nothing
            // until the table-column relation exists here too.
            Combinator::Column => {
                marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                    CompilationMarkerReason::Combinator,
                ));
                return self.builder.push_never();
            }
            // A leftward combinator of None on a non-leftmost compound is a malformed shape the
            // parser should not produce.
            Combinator::None => {
                marker.get_or_insert(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed));
                return self.builder.push_never();
            }
        };
        let step = self.builder.push(constraint);
        self.builder.push_compound(&[compound, step])
    }

    fn compile_compound(
        &mut self,
        compound: &CompoundSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> SelectorNodeID {
        self.compile_compound_hosted_by(compound, None, marker)
    }

    /// Compile one compound. `host` is the node describing the shadow host of a `::part()` compound
    /// whose host was written as a compound of its own; without one the host is whatever else the
    /// compound says, which is the shape `X::part(p)` takes when it arrives unsplit.
    fn compile_compound_hosted_by(
        &mut self,
        compound: &CompoundSelector,
        host: Option<SelectorNodeID>,
        marker: &mut Option<CompilationMarker>,
    ) -> SelectorNodeID {
        // `X::part(p)` is one compound describing two elements: `X` is the shadow host and `::part(p)`
        // is an element inside the tree it hosts. The subject is the part element, so the rest of
        // the compound becomes a step to the host rather than a conjunct on the same node.
        let part = compound.simple_selectors.iter().find_map(|simple| match simple {
            SimpleSelector::PseudoElement(pseudo_element)
                if pseudo_element.pseudo_element == PseudoElementType::Part =>
            {
                Some(pseudo_element)
            }
            _ => None,
        });

        // A pseudo-class in a `::part()` compound constrains the part element, not the host: in
        // `X::part(p):focus-within` it is the part that has focus within it. Only pseudo-classes may
        // follow `::part()`, so which element a simple selector belongs to is decided by kind - the
        // parsed compound does not keep the pseudo-element in source order. Compiling them onto the
        // host leaves the part element reachable only through the host, so a state change on the part
        // itself routes to a subject that cannot carry it.
        let mut operands: Vec<SelectorNodeID> = Vec::with_capacity(compound.simple_selectors.len());
        let mut on_the_part: Vec<SelectorNodeID> = Vec::new();
        // Whether this compound names the host has to be known before any `:has()` in it compiles,
        // and it is a property of the compound as a whole, so it is answered by looking rather than
        // by relying on the order the simple selectors happen to be compiled in. An inner compound
        // does not inherit it: `:host :is(.a:has(.x))` anchors that query on the `.a`.
        let compound_names_the_host = compound.simple_selectors.iter().any(|simple| {
            matches!(
                simple,
                // FIXME: `:host-context()` names the host too, and a `:has()` beside one reads
                //        the hosted tree for the same reason. The parser has no pseudo-class for
                //        it yet - it arrives as an invalid simple selector - so there is nothing
                //        to test here until it does.
                SimpleSelector::PseudoClass(pseudo_class)
                    if pseudo_class.pseudo_class == PseudoClassType::Host
            )
        });
        let outer_names_the_host = std::mem::replace(&mut self.compound_names_the_host, compound_names_the_host);
        for simple in &compound.simple_selectors {
            if let Some(operand) = self.compile_simple(simple, marker) {
                match part.is_some() && matches!(simple, SimpleSelector::PseudoClass(_)) {
                    true => on_the_part.push(operand),
                    false => operands.push(operand),
                }
            }
        }
        self.compound_names_the_host = outer_names_the_host;

        // A compound with no type or universal selector written has one implied, and a default
        // namespace constrains that one as well: in a sheet declaring one, `:link` names a link in
        // that namespace rather than a link anywhere.
        if let Some(default) = self.implied_default_namespace(compound, compound_names_the_host) {
            operands.push(self.builder.push_feature(FeatureTest::Namespace(default)));
        }

        if let Some(part) = part {
            if let Some(written_host) = host {
                operands.push(written_host);
            }
            let host = match operands.is_empty() {
                true => self.builder.push_feature(FeatureTest::AnyElement),
                false => self.builder.push_compound(&operands),
            };
            // The names go under the host op rather than beside it, so that one level of
            // `exportparts` forwarding has to answer for both halves at once.
            let names: Vec<SelectorNodeID> = part
                .identifier_identities
                .iter()
                .map(|identity| {
                    let atom = (self.intern)(identity.raw(), None);
                    self.builder.push(SelectorOp::Part(atom))
                })
                .collect();
            if names.is_empty() {
                marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                    CompilationMarkerReason::PseudoElement,
                ));
            }
            let parts = self.builder.push_compound(&names);
            let mut exposed = vec![self.builder.push(SelectorOp::ExposedToHost { host, parts })];
            exposed.extend(on_the_part);
            return self.builder.push_compound(&exposed);
        }

        if operands.is_empty() {
            return self.builder.push_feature(FeatureTest::AnyElement);
        }
        self.builder.push_compound(&operands)
    }

    /// The identity an id or class selector is keyed by, which is its lowercase folding where the
    /// document matches those selectors case-insensitively.
    fn name_identity(&self, name: &crate::css::selector::NameSelector) -> Option<usize> {
        if self.fold_id_and_class_name_case {
            name.interned_lowercase_name_identity()
        } else {
            name.interned_name_identity()
        }
    }

    fn compile_simple(
        &mut self,
        simple: &SimpleSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Option<SelectorNodeID> {
        match simple {
            // A qualified name constrains two things: the namespace and the name. `*` constrains
            // only the first, and only when its sheet gave it one to constrain.
            SimpleSelector::Universal(name) => Some(match self.namespace_test(name) {
                Some(test) => self.builder.push_feature(FeatureTest::Namespace(test)),
                None if self.compound_names_the_host => self.builder.push_never(),
                None => self.builder.push_feature(FeatureTest::AnyElement),
            }),
            SimpleSelector::TagName(name) => {
                // Tag names are compared against the element's own local name, which preserves
                // case. An HTML element in an HTML document has a lowercase local name, so a
                // selector written `DIV` has to reach it through its lowercase form as well - but
                // `foreignObject` must not be reachable by a lowercase-written selector, because
                // SVG is case-sensitive. One test carrying both forms covers that.
                // A name the document never interned is one no element carries, so the test is
                // unsatisfiable rather than absent. Leaving it out would let `div` match every
                // element in a document holding no `div`.
                let Some(written) = name.interned_name_identity() else {
                    marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                        CompilationMarkerReason::UninternedName,
                    ));
                    return Some(self.builder.push_never());
                };
                let written_atom = (self.intern)(written, None);
                let folded_atom = match name.interned_lowercase_name_identity() {
                    Some(folded) if folded != written => (self.intern)(folded, None),
                    _ => written_atom,
                };
                let tag = self.builder.push_feature(FeatureTest::TagName(TagTest {
                    written: written_atom,
                    folded: folded_atom,
                    fold_in_namespace: self.html_element_namespace,
                }));
                Some(match self.namespace_test(name) {
                    Some(test) => {
                        let namespace = self.builder.push_feature(FeatureTest::Namespace(test));
                        self.builder.push_compound(&[tag, namespace])
                    }
                    None => tag,
                })
            }
            SimpleSelector::Id(name) => match self.name_identity(name) {
                Some(raw) => {
                    let atom = (self.intern)(raw, None);
                    Some(self.builder.push_feature(FeatureTest::Id(atom)))
                }
                None => {
                    marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                        CompilationMarkerReason::UninternedName,
                    ));
                    Some(self.builder.push_never())
                }
            },
            SimpleSelector::Class(name) => match self.name_identity(name) {
                Some(raw) => {
                    let atom = (self.intern)(raw, None);
                    Some(self.builder.push_feature(FeatureTest::Class(atom)))
                }
                None => {
                    marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                        CompilationMarkerReason::UninternedName,
                    ));
                    Some(self.builder.push_never())
                }
            },
            SimpleSelector::Attribute(attribute) => Some(self.compile_attribute(attribute, marker)),
            SimpleSelector::PseudoClass(pseudo_class) => self.compile_pseudo_class(pseudo_class, marker),
            SimpleSelector::PseudoElement(pseudo_element) => self.compile_pseudo_element(pseudo_element, marker),
            // https://drafts.csswg.org/css-nesting/#nest-selector
            // A `&` that reaches matching has not been substituted for its parent rule's selector,
            // and then it behaves as `:scope`: the scoping root where there is one, and the root of
            // the tree where there is not. Compiling it to nothing at all left the compound with no
            // constraint, which matches every element in the document.
            SimpleSelector::Nesting => Some(match self.scope_root_is_bound {
                true => self.builder.push(SelectorOp::ScopeRootInstance),
                false => self.builder.push(SelectorOp::Scope),
            }),
            // A simple selector nothing can satisfy makes its whole compound unsatisfiable. Compiling
            // it to nothing left the compound with no constraint at all, which matches every element
            // in the document rather than none of them.
            SimpleSelector::Invalid(_) => {
                marker.get_or_insert(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed));
                Some(self.builder.push_never())
            }
        }
    }

    /// Compile a pseudo-element simple selector.
    ///
    /// Most of them constrain nothing here: the entry's target is taken from the selector itself,
    /// and the compound describes the originating element. `::slotted()` is different. It is not a
    /// target at all - it styles the assigned element, which is the subject - and its argument is a
    /// real constraint on that element. Leaving it out made `::slotted(input:disabled)` compile to
    /// a compound with nothing in it, so a change to what it tests reached no element and the rule
    /// was never re-evaluated.
    fn compile_pseudo_element(
        &mut self,
        pseudo_element: &PseudoElementSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Option<SelectorNodeID> {
        if pseudo_element.pseudo_element != PseudoElementType::Slotted {
            return None;
        }
        // `::slotted()` is the subject, so an argument that cannot be read leaves the compound with
        // nothing in it, which is every element rather than none.
        let PseudoElementValue::CompoundSelector(argument) = &pseudo_element.value else {
            marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                CompilationMarkerReason::PseudoElement,
            ));
            return Some(self.builder.push_never());
        };
        let compounds = &argument.compound_selectors;
        if compounds.is_empty() {
            marker.get_or_insert(CompilationMarker::KnownNeverMatches(CompilationMarkerReason::Malformed));
            return Some(self.builder.push_never());
        }
        let inner = self.compile_chain(compounds, compounds.len() - 1, marker);
        Some(self.builder.push(SelectorOp::Slotted(inner)))
    }

    /// The constraint the sheet's default namespace places on a compound that writes no name.
    ///
    /// Returns nothing when the compound names a type or universal selector of its own, since that
    /// name carries the constraint already, and when the sheet declared no default.
    fn implied_default_namespace(
        &self,
        compound: &CompoundSelector,
        compound_names_the_host: bool,
    ) -> Option<NamespaceTest> {
        if self.selector_escapes_its_namespace
            || compound_names_the_host
            || self.logical_pseudo_subject == Some(compound as *const CompoundSelector)
        {
            return None;
        }
        let names_an_element = compound
            .simple_selectors
            .iter()
            .any(|simple| matches!(simple, SimpleSelector::Universal(_) | SimpleSelector::TagName(_)));
        match names_an_element {
            true => None,
            false => self.namespaces.default.map(NamespaceScope::test_for_uri),
        }
    }

    /// The namespace constraint a qualified name places, resolved through its sheet.
    fn namespace_test(&mut self, name: &crate::css::selector::QualifiedName) -> Option<NamespaceTest> {
        let prefix = match name.namespace_type {
            NamespaceType::Named => Some(match name.interned_namespace_identity() {
                Some(raw) => (self.intern)(raw, None),
                None => self.intern_text(name.namespace.as_ref()),
            }),
            _ => None,
        };
        self.namespaces.test_for(name.namespace_type, prefix)
    }

    /// Every attribute selector compiles to a test, including one nothing can satisfy: a compound
    /// that drops a simple selector constrains less than the selector says, which for a compound
    /// means matching more.
    fn compile_attribute(
        &mut self,
        attribute: &AttributeSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> SelectorNodeID {
        let Some(raw) = attribute.qualified_name.interned_name_identity() else {
            marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                CompilationMarkerReason::UninternedName,
            ));
            return self.builder.push_never();
        };
        // Each of the three forms an attribute publishes answers one of the three questions a
        // selector can ask. `[ns|x]` tests the qualified name and reaches only that namespace;
        // `[*|x]` tests the any-namespace name, which every attribute publishes; `[x]` and `|x`
        // test the bare local name, which only an attribute in no namespace publishes.
        let namespace = match attribute.qualified_name.namespace_type {
            NamespaceType::Any => Some(StyleAtomID::NONE),
            // https://drafts.csswg.org/css-namespaces/#css-qnames
            // A default namespace applies to type and universal selectors and to nothing else, so
            // an attribute selector written without a prefix names an attribute in no namespace
            // however the sheet declared its default.
            NamespaceType::Default | NamespaceType::None => None,
            NamespaceType::Named => match self.namespace_test(&attribute.qualified_name) {
                Some(NamespaceTest::Named(namespace)) => Some(namespace),
                Some(NamespaceTest::None) | None => None,
            },
        };
        let name = (self.intern)(raw, namespace);

        let operator = match attribute.match_type {
            AttributeMatchType::HasAttribute => AttributeOperator::Presence,
            AttributeMatchType::ExactValue => AttributeOperator::Exact,
            AttributeMatchType::ContainsWord => AttributeOperator::Includes,
            AttributeMatchType::ContainsString => AttributeOperator::Substring,
            AttributeMatchType::StartsWithSegment => AttributeOperator::DashMatch,
            AttributeMatchType::StartsWithString => AttributeOperator::Prefix,
            AttributeMatchType::EndsWithString => AttributeOperator::Suffix,
        };
        let case = match attribute.case_type {
            AttributeCaseType::Insensitive => AttributeCase::Insensitive,
            AttributeCaseType::Sensitive => AttributeCase::Sensitive,
            // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
            // Some attribute names compare their values ASCII case-insensitively, but only for an
            // HTML element in an HTML document, so the subject decides which rule applies.
            AttributeCaseType::Default => {
                let names_a_legacy_attribute = attribute.qualified_name.namespace_type == NamespaceType::Default
                    && crate::css::selector::is_ascii_case_insensitive_html_attribute(&attribute.qualified_name.name);
                match names_a_legacy_attribute && !self.html_element_namespace.is_none() {
                    true => AttributeCase::InsensitiveForNamespace(self.html_element_namespace),
                    false => AttributeCase::Sensitive,
                }
            }
        };
        let (value_offset, value_length) = self.builder.push_literal(&attribute.value);

        // An exact case-sensitive test is an identity question, and the DOM publishes each attribute
        // value under the same text key, so both sides can answer it by comparing two integers -
        // and the dispatch can reject a rule whose value the element does not hold without
        // evaluating anything. Any other operator or case reads the literal.
        let value_atom = match (operator, case) {
            (AttributeOperator::Exact, AttributeCase::Sensitive) => match &attribute.value_identity {
                Some(identity) => (self.intern)(identity.raw(), None),
                None => self.intern_text(&attribute.value),
            },
            _ => StyleAtomID::NONE,
        };

        // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
        // An attribute name is matched ASCII case-insensitively against an HTML element in an HTML
        // document and case-sensitively everywhere else, so `[aB]` reaches an HTML element carrying
        // `ab` while `[viewbox]` must not reach an SVG element carrying `viewBox`. Both forms are
        // carried in one test, and the dispatch keys on the folded one - the form a subject offers
        // whichever way it wrote its own name.
        let folded = match attribute.qualified_name.interned_lowercase_name_identity() {
            Some(lowercase) if lowercase != raw => (self.intern)(lowercase, namespace),
            _ => name,
        };
        let fold_in_namespace = match folded == name {
            true => StyleAtomID::NONE,
            false => self.html_element_namespace,
        };
        self.builder.push_feature(FeatureTest::Attribute(AttributeTest {
            name,
            any_namespace: attribute.qualified_name.namespace_type == NamespaceType::Any,
            folded,
            fold_in_namespace,
            operator,
            value_atom,
            value_offset,
            value_length,
            case,
        }))
    }

    /// Compile one pseudo-class.
    ///
    /// The match is exhaustive over every pseudo-class the parser can produce. That is deliberate:
    /// a pseudo-class with no mapping compiles to something the engine cannot route, and a selector
    /// that cannot be routed cannot be invalidated. Adding a pseudo-class to the parser therefore
    /// fails to compile here until it has been given a meaning.
    fn compile_pseudo_class(
        &mut self,
        pseudo_class: &PseudoClassSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Option<SelectorNodeID> {
        use PseudoClassType as Pc;

        // Boolean facts, which is most of them.
        if let Some(fact) = state_fact_for(pseudo_class.pseudo_class) {
            return Some(self.builder.push(SelectorOp::State(fact)));
        }

        match pseudo_class.pseudo_class {
            // Logical combinators.
            Pc::Is | Pc::Where => {
                // https://drafts.csswg.org/selectors/#matches
                // Both are forgiving, so an argument list whose every selector is invalid parses to
                // an empty one - and an empty list matches nothing. Compiling it to no constraint
                // left the compound matching every element instead.
                let branches = self.compile_argument_list(pseudo_class, marker);
                if branches.is_empty() {
                    return Some(self.builder.push_never());
                }
                let any_of = self.builder.push_any_of(&branches);
                if pseudo_class.pseudo_class == Pc::Where {
                    return Some(self.builder.push(SelectorOp::Where(any_of)));
                }
                Some(any_of)
            }
            Pc::Not => {
                let branches = self.compile_argument_list(pseudo_class, marker);
                if branches.is_empty() {
                    return None;
                }
                let any_of = self.builder.push_any_of(&branches);
                Some(self.builder.push(SelectorOp::Not(any_of)))
            }
            Pc::Has => self.compile_has(pseudo_class, marker),

            // Structural position.
            Pc::FirstChild => Some(self.push_nth(0, 1, false, None, false)),
            Pc::LastChild => Some(self.push_nth(0, 1, true, None, false)),
            Pc::FirstOfType => Some(self.push_nth(0, 1, false, None, true)),
            Pc::LastOfType => Some(self.push_nth(0, 1, true, None, true)),
            // Being the only child is being both the first and the last, but it is one
            // pseudo-class and contributes the specificity of one. The second half is wrapped so
            // that conjoining the two does not count it twice.
            Pc::OnlyChild => {
                let first = self.push_nth(0, 1, false, None, false);
                let last = self.push_nth(0, 1, true, None, false);
                let last = self.builder.push(SelectorOp::Where(last));
                Some(self.builder.push_compound(&[first, last]))
            }
            Pc::OnlyOfType => {
                let first = self.push_nth(0, 1, false, None, true);
                let last = self.push_nth(0, 1, true, None, true);
                let last = self.builder.push(SelectorOp::Where(last));
                Some(self.builder.push_compound(&[first, last]))
            }
            Pc::NthChild | Pc::NthLastChild => {
                let of_selector = self.compile_nth_of(pseudo_class, marker);
                let from_end = pseudo_class.pseudo_class == Pc::NthLastChild;
                Some(self.push_nth(
                    pseudo_class.an_plus_b_pattern.step_size,
                    pseudo_class.an_plus_b_pattern.offset,
                    from_end,
                    of_selector,
                    false,
                ))
            }
            Pc::NthOfType | Pc::NthLastOfType => {
                let from_end = pseudo_class.pseudo_class == Pc::NthLastOfType;
                Some(self.push_nth(
                    pseudo_class.an_plus_b_pattern.step_size,
                    pseudo_class.an_plus_b_pattern.offset,
                    from_end,
                    None,
                    true,
                ))
            }

            // Structural facts about the subject's place in its tree.
            Pc::Root => Some(self.builder.push(SelectorOp::Root)),
            Pc::Empty => Some(self.builder.push(SelectorOp::Empty)),
            Pc::Scope => {
                self.selector_names_the_scope = true;
                // Inside an `@scope`, `:scope` names that scope's root, which is one element the
                // evaluation binds rather than anything the selector can say. Outside one it names
                // the root of the tree.
                Some(match self.scope_root_is_bound {
                    true => self.builder.push(SelectorOp::ScopeRootInstance),
                    false => self.builder.push(SelectorOp::Scope),
                })
            }

            // Shadow encapsulation. `:host` with an argument constrains the host itself.
            Pc::Host => {
                let branches = self.compile_argument_list(pseudo_class, marker);
                let inner = if branches.is_empty() {
                    self.builder.push_feature(FeatureTest::AnyElement)
                } else {
                    self.builder.push_any_of(&branches)
                };
                Some(self.builder.push(SelectorOp::Host(inner)))
            }

            // States whose test carries a value rather than a boolean.
            Pc::Dir => {
                // The keyword is interned as its own text, so the DOM side names the same atom
                // without either side agreeing on a numbering.
                let Some(direction) = pseudo_class.direction else {
                    marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                        CompilationMarkerReason::PseudoClass,
                    ));
                    return Some(self.builder.push_never());
                };
                let keyword: &[u16] = match direction {
                    Direction::LeftToRight => &[b'l' as u16, b't' as u16, b'r' as u16],
                    Direction::RightToLeft => &[b'r' as u16, b't' as u16, b'l' as u16],
                    Direction::Other => &[b'a' as u16, b'u' as u16, b't' as u16, b'o' as u16],
                };
                let value = match &pseudo_class.identifier_lowercase_identity {
                    Some(identity) => (self.intern)(identity.raw(), None),
                    None => self.intern_ascii_lowercase_text(keyword),
                };
                Some(self.builder.push(SelectorOp::ValueState {
                    kind: ValueStateTestKind::Directionality,
                    value,
                }))
            }
            Pc::Lang => {
                // A language list is a disjunction of one test per range.
                //
                // A range is not an atom: `:lang(en)` matches `en-GB`, and a range may hold
                // wildcards, so no single interned value says which elements a rule names. Every
                // `:lang()` compiles to one key, so a resolved language moving reaches every rule
                // that asks about one - bounded to the elements whose tag actually moved.
                match pseudo_class.languages.is_empty() {
                    true => Some(self.builder.push_never()),
                    false => {
                        let ranges: Vec<&[u16]> = pseudo_class
                            .languages
                            .iter()
                            .map(|range| range.value.as_ref())
                            .collect();
                        let (first, count) = self.builder.push_language_ranges(&ranges);
                        Some(self.builder.push(SelectorOp::Language { first, count }))
                    }
                }
            }
            Pc::State => {
                let value = match &pseudo_class.identifier_identity {
                    Some(identity) => (self.intern)(identity.raw(), None),
                    None => match pseudo_class.identifier.as_ref() {
                        Some(identifier) => self.intern_text(identifier),
                        None => StyleAtomID::NONE,
                    },
                };
                Some(self.builder.push(SelectorOp::ValueState {
                    kind: ValueStateTestKind::CustomState,
                    value,
                }))
            }
            Pc::Heading => {
                // A written heading is one of six, but its computed level counts the heading offset
                // its ancestors declare and is clamped at nine. Bare `:heading` is any level an
                // element can have, which is all nine rather than the six it can be written as.
                let mut mask: u16 = 0;
                for level in &pseudo_class.levels {
                    if (1..=9).contains(level) {
                        mask |= 1 << (level - 1);
                    }
                }
                if pseudo_class.levels.is_empty() {
                    mask = 0b1_1111_1111;
                }
                Some(self.builder.push(SelectorOp::Heading(mask)))
            }

            // Everything remaining is a boolean fact, which `state_fact_for` already returned. The
            // arm exists so that adding a pseudo-class to the parser fails to compile here.
            Pc::Active
            | Pc::AnyLink
            | Pc::Autofill
            | Pc::Buffering
            | Pc::Checked
            | Pc::Default
            | Pc::Defined
            | Pc::Disabled
            | Pc::Enabled
            | Pc::EvenLessGoodValue
            | Pc::Focus
            | Pc::FocusVisible
            | Pc::FocusWithin
            | Pc::Fullscreen
            | Pc::HighValue
            | Pc::Hover
            | Pc::Indeterminate
            | Pc::Invalid
            | Pc::Link
            | Pc::LocalLink
            | Pc::LowValue
            | Pc::Modal
            | Pc::Muted
            | Pc::Open
            | Pc::OptimalValue
            | Pc::Optional
            | Pc::Paused
            | Pc::PlaceholderShown
            | Pc::Playing
            | Pc::PopoverOpen
            | Pc::ReadOnly
            | Pc::ReadWrite
            | Pc::Required
            | Pc::Seeking
            | Pc::Stalled
            | Pc::SuboptimalValue
            | Pc::Target
            | Pc::Unchecked
            | Pc::UserInvalid
            | Pc::UserValid
            | Pc::Valid
            | Pc::Visited
            | Pc::VolumeLocked => unreachable!("boolean pseudo-classes are handled as state facts"),
        }
    }

    /// Intern a name the parser did not give an interned identity for, by its text.
    ///
    /// Language tags and custom state names arrive as text rather than as fly strings, so they are
    /// keyed by content. The atom space is shared, so a text-keyed name and an identity-keyed one
    /// never collide: text keys are offset out of the identity range.
    /// The atom key of a case-insensitive name, so a selector and the DOM agree on it.
    ///
    /// Language subtags and `:dir()` keywords are both ASCII case-insensitive and neither arrives
    /// with an interned identity, so both sides derive the same key from the text itself.
    #[must_use]
    pub fn ascii_lowercase_text_key(text: &[u16]) -> usize {
        let hash = fnv1a64(text.iter().map(|unit| match *unit {
            0x41..=0x5a => *unit + 0x20,
            other => other,
        }));
        synthetic_text_atom_key(hash)
    }

    fn intern_ascii_lowercase_text(&mut self, text: &[u16]) -> StyleAtomID {
        (self.intern)(Self::ascii_lowercase_text_key(text), None)
    }

    fn intern_text(&mut self, text: &[u16]) -> StyleAtomID {
        let hash = fnv1a64(text.iter().copied());
        // Text keys live above the pointer range so they cannot alias an interned identity.
        (self.intern)(synthetic_text_atom_key(hash), None)
    }

    fn compile_argument_list(
        &mut self,
        pseudo_class: &PseudoClassSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Vec<SelectorNodeID> {
        let mut branches = Vec::with_capacity(pseudo_class.argument_selector_list.len());
        for argument in &pseudo_class.argument_selector_list {
            let compounds = &argument.compound_selectors;
            if compounds.is_empty() {
                continue;
            }
            // A branch that targets a pseudo-element cannot match the originating element, so it
            // contributes nothing to the disjunction. Compiling it anyway would leave a compound
            // with no feature left in it, and a disjunction with one such branch distinguishes
            // nothing at all: `:is(button, ::file-selector-button):hover` would then be reachable
            // from a hover on any element whatsoever.
            if argument.target_pseudo_element.is_some() {
                continue;
            }
            let subject_index = compounds.len() - 1;
            let subject = &raw const compounds[subject_index];
            let branch = self.compile_chain_with_subject(compounds, subject_index, marker, subject);
            branches.push(branch);
        }
        branches
    }

    fn compile_nth_of(
        &mut self,
        pseudo_class: &PseudoClassSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Option<SelectorNodeID> {
        if pseudo_class.argument_selector_list.is_empty() {
            return None;
        }
        // An `of` list whose every selector names nothing counts an empty sequence, which is not
        // the same as counting every sibling - which is what leaving the filter out would do.
        let branches = self.compile_argument_list(pseudo_class, marker);
        if branches.is_empty() {
            return Some(self.builder.push_never());
        }
        Some(self.builder.push_any_of(&branches))
    }

    /// `:has()` compiles to a relative query per argument. A selector list holds when any one of its
    /// arguments does, so the queries are combined into a disjunction and each keeps its own axis
    /// and driving feature: a list is routed as precisely as each of its alternatives is.
    fn compile_has(
        &mut self,
        pseudo_class: &PseudoClassSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Option<SelectorNodeID> {
        // A `:has()` asking about nothing holds for nothing.
        if pseudo_class.argument_selector_list.is_empty() {
            return Some(self.builder.push_never());
        }

        let mut alternatives = Vec::with_capacity(pseudo_class.argument_selector_list.len());
        for argument in &pseudo_class.argument_selector_list {
            // An alternative that cannot be compiled cannot be dropped: the query holds when any one
            // of them does, so leaving one out would answer a narrower question than the rule asks.
            alternatives.push(self.compile_has_argument(argument, marker)?);
        }
        if alternatives.len() == 1 {
            return Some(alternatives[0]);
        }
        Some(self.builder.push_any_of(&alternatives))
    }

    /// One relative selector: an axis leading from the anchor to the elements that can witness it,
    /// and a compound each of them is tested against.
    fn compile_has_argument(
        &mut self,
        argument: &CompiledSelector,
        marker: &mut Option<CompilationMarker>,
    ) -> Option<SelectorNodeID> {
        let compounds = &argument.compound_selectors;
        if compounds.is_empty() {
            return None;
        }
        let leading = match compounds[0].combinator {
            Combinator::Descendant | Combinator::None => RelativeAxis::Descendant,
            Combinator::ImmediateChild => RelativeAxis::Child,
            Combinator::NextSibling => RelativeAxis::NextSibling,
            Combinator::SubsequentSibling => RelativeAxis::FollowingSibling,
            // Neither relates one element to another: a column combinator relates table cells
            // through a relation the IR has no operator for, and a pseudo-element combinator does
            // not lead out of the anchor at all. Nothing witnesses such a query.
            Combinator::Column | Combinator::PseudoElement => {
                marker.get_or_insert(CompilationMarker::KnownNeverMatches(
                    CompilationMarkerReason::Combinator,
                ));
                return Some(self.builder.push_never());
            }
        };

        // A single compound is the exact shape: one axis leads from the anchor to the elements that
        // can witness the query, and each of them is tested on the spot.
        let simple = compounds.len() == 1;

        // `:has(+ div .test)` names a `.test` under the anchor's next sibling, not a next sibling
        // that is itself a `.test`. That holds whenever every step after the leading one goes
        // downwards; a step that goes sideways again reaches a witness beside the axis element.
        let witness_is_below_the_axis = !simple
            && matches!(leading, RelativeAxis::NextSibling | RelativeAxis::FollowingSibling)
            && compounds[1..].iter().all(|compound| {
                matches!(
                    compound.combinator,
                    Combinator::Descendant | Combinator::None | Combinator::ImmediateChild
                )
            });
        let axis = match simple {
            true => leading,
            // AD-HOC: With more compounds the witness is no longer on the leading axis. Every step
            //         after a downward one stays inside the anchor's subtree, whichever direction
            //         it goes, so the descendant axis covers the witnesses. A leading sibling
            //         combinator leaves the subtree and has no axis that covers it.
            false => match leading {
                RelativeAxis::Descendant | RelativeAxis::Child => RelativeAxis::Descendant,
                // A leading sibling combinator leaves the anchor's subtree, and the steps after it
                // go down from there, so the witness is under one of the siblings rather than
                // being one. Unless one of them goes sideways again: `:has(+ .a + .b)` reaches a
                // witness that is a following sibling of the anchor and under no sibling at all, so
                // the axis has to cover them.
                RelativeAxis::NextSibling | RelativeAxis::FollowingSibling if !witness_is_below_the_axis => {
                    RelativeAxis::FollowingSiblingSubtree
                }
                RelativeAxis::NextSibling => RelativeAxis::NextSiblingSubtree,
                RelativeAxis::FollowingSibling => RelativeAxis::FollowingSiblingSubtree,
                RelativeAxis::NextSiblingSubtree | RelativeAxis::FollowingSiblingSubtree => leading,
            },
        };

        // An in-shadow-tree query walks its axis from the shadow root, and every sibling axis leaves
        // that tree: the host's siblings are in the light DOM, which the sheet this query was
        // written in cannot see. `:host:has(+ .x)` therefore names nothing at all, which is worth
        // saying at compile time so no such query is ever evaluated or routed.
        if self.compound_names_the_host && !matches!(axis, RelativeAxis::Descendant | RelativeAxis::Child) {
            return Some(self.builder.push_never());
        }

        // The chain is compiled as constraints on the witness, which cannot say where the chain
        // started: `:has(.a .b)` would accept a `.b` whose `.a` is above the anchor, and
        // `:has(+ div > .test)` a `.test` under any `div` at all. Ending the chain at the anchor
        // itself is what ties it back down. A single compound needs none of this: the axis reaches
        // exactly the elements the leading combinator names.
        let leftmost = match simple {
            true => None,
            false => {
                let anchor = self.builder.push(SelectorOp::RelativeAnchorInstance);
                Some(self.builder.push(match compounds[0].combinator {
                    Combinator::Descendant | Combinator::None => SelectorOp::Ancestor(anchor),
                    Combinator::ImmediateChild => SelectorOp::Parent(anchor),
                    Combinator::NextSibling => SelectorOp::PreviousSibling(anchor),
                    Combinator::SubsequentSibling => SelectorOp::PrecedingSibling(anchor),
                    Combinator::Column | Combinator::PseudoElement => unreachable!("rejected above"),
                }))
            }
        };

        let compound = self.compile_chain_anchored(compounds, compounds.len() - 1, marker, leftmost);
        if !simple {
            // A query with more than one compound retains no witness, so it is answered by the
            // direct evaluator rather than by a replacement search.
            marker.get_or_insert(CompilationMarker::HandledByOtherIR(
                CompilationMarkerReason::ComplexRelative,
            ));
        }

        let driving_feature = self.driving_feature_of(compound);
        Some(self.builder.push_relative_exists(RelativeQuery {
            axis,
            compound,
            driving_feature,
            simple,
            witness_is_below_the_axis,
            match_in_shadow_tree: self.compound_names_the_host,
        }))
    }

    /// The positive indexable feature a witness search can drive from, if the compound has one.
    fn driving_feature_of(&self, compound: SelectorNodeID) -> Option<super::index::LocalFeatureKey> {
        let program = self.builder.program();
        let feature_of = |test: FeatureTest| match test {
            FeatureTest::Class(class) => Some(super::index::LocalFeatureKey::Class(class)),
            FeatureTest::Id(_) => Some(super::index::LocalFeatureKey::Id),
            FeatureTest::Attribute(attribute) => Some(super::index::LocalFeatureKey::Attribute(attribute.folded)),
            FeatureTest::TagName(_) => Some(super::index::LocalFeatureKey::TagName),
            // Neither enumerates a candidate set: every element is in some namespace, so driving a
            // witness search from one would be driving it from the whole document.
            FeatureTest::AnyElement | FeatureTest::Namespace(_) => None,
        };
        match program.node(compound) {
            SelectorOp::Feature(test) => feature_of(test),
            SelectorOp::And { first, count } => {
                program
                    .operands(first, count)
                    .iter()
                    .find_map(|&operand| match program.node(operand) {
                        SelectorOp::Feature(test) => feature_of(test),
                        _ => None,
                    })
            }
            _ => None,
        }
    }

    fn push_nth(
        &mut self,
        step: i32,
        offset: i32,
        from_end: bool,
        of_selector: Option<SelectorNodeID>,
        of_type: bool,
    ) -> SelectorNodeID {
        self.builder.push(SelectorOp::NthPosition(NthPosition {
            step,
            offset,
            from_end,
            of_selector,
            of_type,
        }))
    }
}

// The pseudo-classes StyleEngine models as boolean element state facts.
//
// Returning `None` means the pseudo-class is an operator instead, not that it lacks a state fact.
include!(concat!(env!("OUT_DIR"), "/style_state_fact_compiler_generated.rs"));
