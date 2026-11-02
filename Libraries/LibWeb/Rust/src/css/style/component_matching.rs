/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Matching selectors by walking their components: the evaluator the rule index, invalidation, the exact matcher and
//! the DOM queries match by.
//!
//! A selector matches compound by compound from its subject leftwards, reading each component where it is in its
//! sheet's arena. What the document decides (quirks folding, the HTML namespace) comes from the match context, and
//! the `@scope` levels of the rule from its bound scope chain.

use super::StyleAtomID;
use super::index::{DispatchKey, dispatch_bloom_bit};
use super::selector::attribute_value_matches;
use super::selector_evaluation::{ElementFeatures, SelectorEvaluator, SelectorSubject, SelectorTree, StructuralRead};
use crate::css::selector::{
    AttributeCaseType, AttributeMatchType, AttributeSelector, Combinator, CompoundRef, NamespaceType,
    PseudoClassSelector, PseudoClassType, Selector, SelectorListView, SimpleSelector,
};
use crate::css::selector_arena::{IdOrClass, Nesting};
use smallvec::SmallVec;

type Answer<S> = Result<bool, <S as SelectorSubject>::Incomplete>;

/// What matching a selector needs to know of the document it matches in.
#[derive(Clone, Copy)]
pub(crate) struct MatchContext<'a> {
    /// The HTML namespace in an HTML document, where tag and attribute names fold, and none elsewhere.
    pub(crate) html_namespace: StyleAtomID,
    /// Whether ids and classes compare ASCII case-insensitively, as in a quirks-mode document, whose elements
    /// publish their folded forms.
    pub(crate) fold_ids_and_classes: bool,
    /// The `@namespace` declarations of the sheet the selectors are in.
    pub(crate) namespaces: crate::css::selector_arena::SheetNamespaces<'a>,
    /// Whether the selector names an element in another tree, with `::slotted()` or `::part()`, where the sheet's
    /// default namespace does not follow it.
    pub(crate) escapes_its_namespace: bool,
    /// Whether the selector being matched is an argument of a logical pseudo-class, whose subject the default
    /// namespace does not constrain unless it writes a name of its own.
    pub(crate) logical_subject: bool,
}

/// How a walk along a selector's compounds ended: matched, or else which elements a walk further right has to try
/// again from.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Walk {
    Matched,
    /// No element of the walk left of a later sibling of the element tried can match either.
    RestartFromLaterSibling,
    /// Only an element further down, below a descendant combinator, can match.
    RestartFromDescendant,
    /// No element the walk can still reach matches.
    NotMatchedGlobally,
}

/// A positional pseudo-class, as the components say it.
#[derive(Clone, Copy)]
struct Position<'a> {
    step: i32,
    offset: i32,
    from_end: bool,
    of_type: bool,
    of: Option<SelectorListView<'a>>,
}

impl<S: SelectorSubject> SelectorEvaluator<S> {
    /// Whether `node` matches `selector`, walking its compounds from the subject leftwards.
    /// A selector targeting a pseudo-element matches the element it originates from: the compounds before it.
    pub(crate) fn matches_selector(
        &mut self,
        selector: Selector<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        // NB: The compounds are read from the subject leftwards where they lie in the arena: the compound a selector
        //     ends in, as `::part()`, `::slotted()` or a pseudo-element, is the first one read, which the arena notes
        //     most selectors end in none of.
        if selector.describes_its_subject() {
            return self.matches_from(selector, None, node, context);
        }
        let target = selector.first_compound();
        if targets_a_pseudo_element(&target) {
            let Some(left) = left_of(selector) else {
                return Ok(false);
            };
            // The pseudo-classes written after an element-backed pseudo-element describe the element backing it,
            // which stands in the originating element's shadow tree.
            match backed_pseudo_element_kind(&target) {
                Some(kind) => {
                    let Some(backing) = self.subject.tree().backing_element_of(node, kind) else {
                        return Ok(false);
                    };
                    for simple in target.simple_selectors.after_first().iter() {
                        if !self.matches_simple_component(&simple, backing, context)? {
                            return Ok(false);
                        }
                    }
                }
                // FIXME: The pseudo-classes written after a pseudo-element no element backs, which describe the
                //        pseudo-element itself, but for the search text filters, which pick the entry.
                None if target
                    .simple_selectors
                    .after_first()
                    .iter()
                    .any(|simple| crate::css::selector_arena::search_text_match_filter(&simple).is_none()) =>
                {
                    return Ok(false);
                }
                None => {}
            }
            return self.matches_chain_from(left, node, context);
        }
        self.matches_chain_from(selector, node, context)
    }

    /// As [`Self::matches_selector`], for the selector from `at` leftwards, once the compound of a pseudo-element it
    /// targets was matched.
    fn matches_chain_from(&mut self, at: Selector<'_>, node: S::Node, context: MatchContext<'_>) -> Answer<S> {
        let target = at.first_compound();
        let rest = left_of(at);
        if let Some(names) = part_names(&target) {
            return self.matches_part(rest, &target, names, node, context);
        }
        if let Some(argument) = slotted_argument(&target) {
            return self.matches_slotted(rest, argument, node, context);
        }
        self.matches_from(at, None, node, context)
    }

    /// Whether `node` is a part, named `names`, which a host the selector `rest` matches exposes, and matches the
    /// pseudo-classes written after `::part()`.
    ///
    /// https://drafts.csswg.org/css-shadow-parts-1/#part
    fn matches_part(
        &mut self,
        rest: Option<Selector<'_>>,
        part: &CompoundRef<'_>,
        names: &[StyleAtomID],
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        for simple in part.simple_selectors.after_first().iter() {
            if !self.matches_simple_component(&simple, node, context)? {
                return Ok(false);
            }
        }
        // `::part()` reaches one level down: into a tree hosted by an element of the tree the rule itself is in. A
        // compound naming `:host` reaches the rule's own tree as well, which is where a part forwarded out of it
        // stands.
        let tree = self.subject.tree();
        let scope_host = self.bindings.scope_shadow_root.and_then(|root| tree.host_of(root));
        let mentions_the_host = rest.is_some_and(|rest| {
            rest.first_compound().simple_selectors.iter().any(|simple| {
                matches!(simple, SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Host)
            })
        });
        for level_host in self.subject.part_exposure_hosts(node) {
            let reaches_a_hosted_tree = tree.shadow_host_of(level_host) == scope_host;
            let reaches_its_own_tree = Some(level_host) == scope_host && mentions_the_host;
            if !reaches_a_hosted_tree && !reaches_its_own_tree {
                continue;
            }
            let mut exposed = true;
            for &name in names {
                if !self.subject.exposes_part_to(node, name, level_host)? {
                    exposed = false;
                    break;
                }
            }
            if !exposed {
                continue;
            }
            let host_matches = match rest {
                Some(rest) => self.matches_from(rest, None, level_host, context)?,
                None => true,
            };
            if host_matches {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// Whether `node`, slotted, matches `argument`, and a slot it is assigned to through the tree whose selectors are
    /// matched matches the selector `rest`.
    ///
    /// https://drafts.csswg.org/css-scoping/#slotted-pseudo
    fn matches_slotted(
        &mut self,
        rest: Option<Selector<'_>>,
        argument: Selector<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        let tree = self.subject.tree();
        if tree.assigned_slot_of(node).is_none() || !self.matches_selector(argument, node, context)? {
            return Ok(false);
        }
        // The chain can pass through several trees; the slot the compounds describe is the one in the tree whose
        // selectors are matched.
        const MAX_REASSIGNMENTS: usize = 32;
        let mut current = node;
        for _ in 0..MAX_REASSIGNMENTS {
            let Some(slot) = tree.assigned_slot_of(current) else {
                return Ok(false);
            };
            let in_this_tree = self.bindings.scope_shadow_root.is_none_or(|root| {
                std::iter::successors(Some(slot), |&node| tree.live_parent(node)).any(|node| node == root)
            });
            if in_this_tree {
                let slot_matches = match rest {
                    Some(rest) => self.matches_from(rest, None, slot, context)?,
                    None => true,
                };
                if slot_matches {
                    return Ok(true);
                }
            }
            current = slot;
        }
        Ok(false)
    }

    fn matches_scope_level(
        &mut self,
        scope: &super::bridge::BoundScopeChain,
        level: usize,
        selector: Selector<'_>,
        names_the_scope: bool,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        if level == scope.levels.len() {
            let matches = self.matches_selector(selector, node, context)?;
            if matches {
                self.bindings.matched_scope_root = self.bindings.scope_root_instance;
            }
            return Ok(matches);
        }
        let (roots, limits) = scope_level_ranges(scope, level);
        let innermost = level + 1 == scope.levels.len();
        // A scoped selector carries an implied `:scope ` prefix unless it names `:scope`, so the subject is a strict
        // descendant of the innermost root. An enclosing scope is satisfied by the subject being anywhere inside it.
        let inclusive = !innermost || names_the_scope;
        let enclosing_root = self.bindings.scope_root_instance;
        let scope_shadow_root = self.bindings.scope_shadow_root;
        let tree = self.subject.tree();
        let step = |candidate: Option<S::Node>| match candidate {
            Some(root) if Some(root) == scope_shadow_root => tree.host_of(root),
            candidate => candidate,
        };
        // A part stands inside a shadow tree, but an outer scope reaches it through the host exposing it, so scope
        // membership is measured from that host rather than from the part's parent chain.
        let part_hosts = match part_names(&selector.first_compound()) {
            Some(_) => self.subject.part_hosts_in_exposure_scope(node),
            None => None,
        };
        let starts: smallvec::SmallVec<[(Option<S::Node>, bool); 1]> = match &part_hosts {
            Some(hosts) => hosts
                .iter()
                .map(|&host| match inclusive {
                    true => (Some(host), false),
                    false => (tree.live_parent(host), false),
                })
                .collect(),
            None => smallvec::smallvec![(
                step(match inclusive {
                    true => Some(node),
                    false => tree.live_parent(node),
                }),
                true
            )],
        };
        for (start, steps_to_host) in starts {
            let mut candidate = start;
            while let Some(root) = candidate {
                if self.matches_scope_root(scope, level, roots.clone(), root, context)? {
                    let outer = self.bindings.scope_root_instance.replace(root);
                    let answer = (|| -> Answer<S> {
                        if self.is_beyond_scope_limit(scope, limits.clone(), node, root, context)? {
                            return Ok(false);
                        }
                        self.matches_scope_level(scope, level + 1, selector, names_the_scope, node, context)
                    })();
                    self.bindings.scope_root_instance = outer;
                    if answer? {
                        return Ok(true);
                    }
                }
                if Some(root) == enclosing_root {
                    break;
                }
                candidate = match steps_to_host {
                    true => step(tree.live_parent(root)),
                    false => tree.live_parent(root),
                };
            }
        }
        Ok(false)
    }

    /// Whether `candidate` roots a scope of `level`: its implicit root, or an element its `<scope-start>` matches,
    /// where `:scope` names the enclosing scope's root.
    fn matches_scope_root(
        &mut self,
        scope: &super::bridge::BoundScopeChain,
        level: usize,
        roots: std::ops::Range<usize>,
        candidate: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        if let Some(implicit) = scope.implicit_roots.get(level).copied().flatten() {
            let matches = match implicit {
                super::compiler::ImplicitScopeRoot::Node(named) => self.subject.is_node(candidate, named),
                super::compiler::ImplicitScopeRoot::ContainingTree => {
                    self.subject.is_root(candidate)
                        || self
                            .bindings
                            .scope_shadow_root
                            .is_some_and(|root| self.subject.tree().shadow_root_of(candidate) == Some(root))
                }
            };
            if matches {
                return Ok(true);
            }
        }
        let anchor = self.bindings.scope_root_instance;
        for root in &scope.roots[roots] {
            let root = root.view();
            let leading = root.leftmost_compound().combinator;
            // Directly inside another `@scope`, a start that begins with a combinator is relative to its root.
            let matches = match leading {
                Combinator::None => self.matches_selector(root, candidate, context)?,
                Combinator::Descendant => {
                    self.matches_with_leading(root, Combinator::None, None, candidate, context)?
                }
                _ => self.matches_with_leading(root, leading, anchor, candidate, context)?,
            };
            if matches {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// Whether `node`, or an element between it and the scoping root `root`, matches a `<scope-end>` of the level.
    fn is_beyond_scope_limit(
        &mut self,
        scope: &super::bridge::BoundScopeChain,
        limits: std::ops::Range<usize>,
        node: S::Node,
        root: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        if limits.is_empty() {
            return Ok(false);
        }
        let tree = self.subject.tree();
        let mut walked = Some(node);
        while let Some(current) = walked {
            for (limit, &names_the_root) in scope.limits[limits.clone()]
                .iter()
                .zip(&scope.limits_name_the_root[limits.clone()])
            {
                let limit = limit.view();
                let leading = limit.leftmost_compound().combinator;
                // https://drafts.csswg.org/css-cascade-6/#scope-syntax
                // A `<scope-end>` is relative to the scoping root: one that starts with a combinator, or names
                // neither `&` nor `:scope`, is read as though `:where(:scope)` came first.
                let matches = match (leading, names_the_root) {
                    (Combinator::None, false) => {
                        self.matches_with_leading(limit, Combinator::Descendant, Some(root), current, context)?
                    }
                    (Combinator::None | Combinator::Descendant, true) => {
                        self.matches_with_leading(limit, Combinator::None, None, current, context)?
                    }
                    (leading, _) => self.matches_with_leading(limit, leading, Some(root), current, context)?,
                };
                if matches {
                    return Ok(true);
                }
            }
            if current == root {
                break;
            }
            walked = tree.live_parent(current);
        }
        Ok(false)
    }

    /// Whether `node` matches `selector` with its leftmost compound related to `anchor` by `leading`, or standing
    /// alone for no combinator.
    fn matches_with_leading(
        &mut self,
        selector: Selector<'_>,
        leading: Combinator,
        anchor: Option<S::Node>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        let outer = std::mem::replace(&mut self.bindings.relative_anchor, anchor);
        let answer = self.matches_from(selector, Some(leading), node, context);
        self.bindings.relative_anchor = outer;
        answer
    }

    /// Whether `node` matches any selector of `list`.
    fn matches_any(&mut self, list: SelectorListView<'_>, node: S::Node, context: MatchContext<'_>) -> Answer<S> {
        let context = MatchContext {
            logical_subject: true,
            ..context
        };
        for selector in list {
            // A pseudo-element is not an element, which a logical pseudo-class's argument describes.
            if !targets_a_pseudo_element(&selector.first_compound())
                && self.matches_selector(selector, node, context)?
            {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// Whether `node` matches the selector from `at`, its subject compound, leftwards, with the leftmost compound
    /// related to the anchor by `leading` where one is given rather than by the combinator written.
    fn matches_from(
        &mut self,
        at: Selector<'_>,
        leading: Option<Combinator>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        Ok(self.walk_compound(at, true, leading, node, context)? == Walk::Matched)
    }

    /// As [`Self::walk_compound`] for a compound left of the subject's, which an element whose dispatch keys lack an id
    /// or class it tests for cannot match: the bits the arena noted for the compound tell before its components are read.
    fn walk_rejecting_on_bloom(
        &mut self,
        at: Selector<'_>,
        leading: Option<Combinator>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Result<Walk, S::Incomplete> {
        if S::FILTERS_BY_REQUIRED_DISPATCH_BLOOM {
            let required = required_dispatch_bloom(at, context);
            if required != 0 && !self.subject.may_match_required(required, node) {
                return Ok(Walk::RestartFromLaterSibling);
            }
        }
        self.walk_compound(at, false, leading, node, context)
    }

    /// Whether `node` matches the first compound of `at`, the subject compound where `is_subject`, and what is left
    /// of it, or else which elements no further one tried left of the combinator right of it can match from either.
    fn walk_compound(
        &mut self,
        at: Selector<'_>,
        is_subject: bool,
        leading: Option<Combinator>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Result<Walk, S::Incomplete> {
        let compound = at.first_compound();
        if !self.matches_compound_components(&compound, node, context)?
            || self.is_outside_the_default_namespace(&compound, is_subject, node, context)?
        {
            return Ok(Walk::RestartFromLaterSibling);
        }
        self.walk_left_of(at, leading, node, context)
    }

    /// Whether `node` is outside the default namespace that constrains `compound`, the subject compound where
    /// `is_subject`.
    fn is_outside_the_default_namespace(
        &mut self,
        compound: &CompoundRef<'_>,
        is_subject: bool,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        // A compound with no type or universal selector written has one implied, which a default namespace
        // constrains as well, except where the compound names the host, the selector reaches into another tree, or
        // the compound is the subject of a logical pseudo-class's argument.
        // The universal selector the parse implies before a compound that starts with a pseudo-element is not
        // written.
        let written = match compound.is_implicit_universal_anchor {
            true => compound.simple_selectors.after_first(),
            false => compound.simple_selectors,
        };
        Ok(match context.namespaces.default {
            Some(default)
                if !context.escapes_its_namespace
                    && !(context.logical_subject && is_subject)
                    && !written.iter().any(|simple| {
                        matches!(simple, SimpleSelector::Universal(_) | SimpleSelector::TagName(_))
                            || matches!(simple, SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Host)
                    }) =>
            {
                !self.is_in_namespace(default, node)?
            }
            _ => false,
        })
    }

    /// Whether `node` is in the namespace the qualified name `name` names in its sheet.
    ///
    /// https://drafts.csswg.org/css-namespaces/#css-qnames
    fn is_in_namespace_of(
        &mut self,
        name: &crate::css::selector::QualifiedName,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        let prefix = (name.namespace_type == NamespaceType::Named).then_some(name.namespace);
        match context.namespaces.test_for(name.namespace_type, prefix) {
            Ok(Some(namespace)) => self.is_in_namespace(namespace, node),
            Ok(None) => Ok(true),
            // A qualified name with a prefix that has not been declared is invalid, and matches nothing.
            Err(_) => Ok(false),
        }
    }

    /// Whether `node` is in `namespace`, none being the absence of one.
    fn is_in_namespace(&mut self, namespace: StyleAtomID, node: S::Node) -> Answer<S> {
        let row = self.subject.row(node)?;
        Ok(self.subject.features(row).namespace_is(namespace))
    }

    /// Whether what is left of the first compound of `at`, matched at `node`, matches: the compound left of it, or for
    /// the leftmost compound of a relative selector, the anchor of the `:has()` being answered.
    ///
    /// NB: A failure tells how far back a walk has to go, so that a walk up or back across elements does not try
    ///     again what an inner walk already tried.
    fn walk_left_of(
        &mut self,
        at: Selector<'_>,
        leading: Option<Combinator>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Result<Walk, S::Incomplete> {
        let (combinator, left_compounds) = match at.left() {
            Some((combinator, left)) if !left.is_relative_anchor() => (combinator, Some(left)),
            Some((combinator, _)) => (leading.unwrap_or(combinator), None),
            None => (leading.unwrap_or(Combinator::None), None),
        };
        if left_compounds.is_none() && combinator == Combinator::None {
            return Ok(Walk::Matched);
        }
        let tree = self.subject.tree();
        match combinator {
            Combinator::ImmediateChild => match self.parent_in_walk(node) {
                Some(parent) => Ok(match self.walk_left(left_compounds, leading, parent, false, context)? {
                    walk @ (Walk::Matched | Walk::NotMatchedGlobally) => walk,
                    _ => Walk::RestartFromDescendant,
                }),
                None => Ok(Walk::NotMatchedGlobally),
            },
            Combinator::Descendant => self.walk_up(left_compounds, leading, node, context),
            Combinator::NextSibling => {
                self.subject.note_structural_read(node, StructuralRead::PreviousSibling);
                match tree.previous_sibling(node) {
                    Some(sibling) => self.walk_left(left_compounds, leading, sibling, false, context),
                    None => Ok(Walk::RestartFromDescendant),
                }
            }
            Combinator::SubsequentSibling => {
                self.subject.note_structural_read(node, StructuralRead::EarlierSiblings);
                self.walk_back(left_compounds, leading, node, context)
            }
            // FIXME: Pseudo-element compounds and the column combinator.
            Combinator::PseudoElement | Combinator::Column | Combinator::None => Ok(Walk::NotMatchedGlobally),
        }
    }

    /// Test `candidate`, which a combinator reached, against the compounds left of it, `left_compounds`, or where
    /// there are none against what the selector is relative to.
    ///
    /// NB: A walk up has tested each ancestor against the bits the compound requires before trying it, which the
    ///     compound need not test again where `bloom_checked`.
    #[inline(always)]
    fn walk_left(
        &mut self,
        left_compounds: Option<Selector<'_>>,
        leading: Option<Combinator>,
        candidate: S::Node,
        bloom_checked: bool,
        context: MatchContext<'_>,
    ) -> Result<Walk, S::Incomplete> {
        match left_compounds {
            // A relative selector outside a `:has()` is relative to the scoping root, as one in `@scope` is.
            None => Ok(
                match match self.bindings.relative_anchor {
                    Some(anchor) => anchor == candidate,
                    None => self.is_scoping_root(candidate),
                } {
                    true => Walk::Matched,
                    false => Walk::RestartFromLaterSibling,
                },
            ),
            Some(left) if bloom_checked => self.walk_compound(left, false, leading, candidate, context),
            Some(left) => self.walk_rejecting_on_bloom(left, leading, candidate, context),
        }
    }

    /// The parent a combinator reaches from `node`.
    ///
    /// https://drafts.csswg.org/css-scoping/#host-element-in-tree
    /// In the context of its shadow tree, the shadow host is the parent of the tree's root elements. Nothing outside
    /// the tree whose selectors are matched is above its host.
    #[inline(always)]
    fn parent_in_walk(&self, node: S::Node) -> Option<S::Node> {
        let tree = self.subject.tree();
        let scope_shadow_root = self.bindings.scope_shadow_root;
        match tree.parent(node) {
            _ if scope_shadow_root.is_some() && tree.shadow_root_of(node) == scope_shadow_root => None,
            Some(parent) if Some(parent) == scope_shadow_root => tree.host_of(parent),
            parent => parent,
        }
    }

    /// Where what this walk from `left` finds for an element is remembered: it is what it finds for every element it
    /// passed on the way, for a subject that remembers it, while no `:has()` or `@scope` start binds the walk.
    #[inline(always)]
    fn remembered_walk(&mut self, left: Option<Selector<'_>>, leading: Option<Combinator>) -> Option<S::WalkAnswers> {
        let left = left?;
        if leading.is_some() || self.bindings.relative_anchor.is_some() || self.bindings.matching_host_argument {
            return None;
        }
        self.subject.walk_answers(left.key(), &self.bindings)
    }

    /// Test the ancestors of `node` against `left_compounds`, across a descendant combinator.
    fn walk_up(
        &mut self,
        left_compounds: Option<Selector<'_>>,
        leading: Option<Combinator>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Result<Walk, S::Incomplete> {
        // An ancestor whose dispatch keys lack an id or class the compound left of the combinator tests for
        // cannot match it.
        let required = match left_compounds {
            Some(left) if S::FILTERS_BY_REQUIRED_DISPATCH_BLOOM => required_dispatch_bloom(left, context),
            _ => 0,
        };
        let remembered = match S::REMEMBERS_ANCESTOR_ANSWERS {
            true => self.remembered_walk(left_compounds, leading),
            false => None,
        };
        let mut passed: SmallVec<[S::Node; 16]> = SmallVec::new();
        let mut found = Walk::NotMatchedGlobally;
        let mut ancestor = self.parent_in_walk(node);
        while let Some(current) = ancestor {
            if let Some(answers) = remembered {
                if let Some(answer) = self.subject.walk_answer(answers, current) {
                    found = match answer {
                        true => Walk::Matched,
                        false => Walk::NotMatchedGlobally,
                    };
                    break;
                }
                passed.push(current);
            }
            if (required == 0 || self.subject.may_match_required(required, current))
                && let walk @ (Walk::Matched | Walk::NotMatchedGlobally) =
                    self.walk_left(left_compounds, leading, current, required != 0, context)?
            {
                found = walk;
                break;
            }
            ancestor = self.parent_in_walk(current);
        }
        if let Some(answers) = remembered {
            self.subject
                .record_walk_answers(answers, &passed, found == Walk::Matched);
        }
        Ok(found)
    }

    /// Test the earlier siblings of `node` against `left_compounds`, across a subsequent-sibling combinator.
    fn walk_back(
        &mut self,
        left_compounds: Option<Selector<'_>>,
        leading: Option<Combinator>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Result<Walk, S::Incomplete> {
        let tree = self.subject.tree();
        let remembered = match S::REMEMBERS_SIBLING_ANSWERS {
            true => self.remembered_walk(left_compounds, leading),
            false => None,
        };
        let mut passed: SmallVec<[S::Node; 16]> = SmallVec::new();
        let mut sibling = tree.previous_sibling(node);
        let found = loop {
            let Some(current) = sibling else {
                break false;
            };
            if let Some(answers) = remembered {
                if let Some(answer) = self.subject.walk_answer(answers, current) {
                    break answer;
                }
                passed.push(current);
            }
            match self.walk_left(left_compounds, leading, current, false, context)? {
                Walk::Matched => break true,
                // A walk that gave up on the subject says nothing of the siblings left.
                walk @ (Walk::NotMatchedGlobally | Walk::RestartFromDescendant) => return Ok(walk),
                Walk::RestartFromLaterSibling => {}
            }
            sibling = tree.previous_sibling(current);
        };
        if let Some(answers) = remembered {
            self.subject.record_walk_answers(answers, &passed, found);
        }
        Ok(match found {
            true => Walk::Matched,
            false => Walk::RestartFromDescendant,
        })
    }

    /// Whether an element relative to `anchor` matches a selector of `list`, the argument of a `:has()`. The
    /// candidates are reached from `within`: the anchor, or the shadow root of the host a `:host:has()` names.
    ///
    /// https://drafts.csswg.org/selectors/#relational
    fn matches_relative(
        &mut self,
        list: SelectorListView<'_>,
        anchor: S::Node,
        within: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        let key = list.identity();
        if let Some(key) = key
            && let Some(answer) = self.subject.relative_answer_of(key, anchor, &self.bindings)
        {
            return Ok(answer);
        }
        let answer = (|| {
            for selector in list {
                if self.matches_relative_selector(selector, anchor, within, context)? {
                    return Ok(true);
                }
            }
            Ok(false)
        })();
        // A walk that ended incomplete proved nothing either way.
        if let Some(key) = key
            && let Ok(answer) = answer
        {
            self.subject
                .record_relative_answer_of(key, anchor, &self.bindings, answer);
        }
        answer
    }

    /// Whether an element relative to `anchor` matches `selector`, a relative selector of the argument of a `:has()`,
    /// reached from `within` as [`Self::matches_relative`] reaches it.
    fn matches_relative_selector(
        &mut self,
        selector: Selector<'_>,
        anchor: S::Node,
        within: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        if selector.is_relative_anchor() {
            return Ok(false);
        }
        let context = MatchContext {
            logical_subject: false,
            ..context
        };
        let outer = self.bindings.relative_anchor.replace(anchor);
        let outer_argument = std::mem::replace(&mut self.bindings.matching_relative_argument, true);
        let (axis, below_the_axis) = relative_axis(selector);
        let tree = self.subject.tree();
        let mut matched = Ok(false);
        // The anchor stands outside the tree a `:host:has()` reads, whose root it walks from.
        let axis = match (within == anchor, axis) {
            (false, _) => super::relative_selector::RelativeAxis::Descendant,
            (true, axis) => axis,
        };
        super::selector_evaluation::walk_relative_axis(tree, axis, below_the_axis, within, |candidate| {
            match self.matches_from(selector, None, candidate, context) {
                Ok(true) => {
                    matched = Ok(true);
                    false
                }
                Ok(false) => true,
                Err(incomplete) => {
                    matched = Err(incomplete);
                    false
                }
            }
        });
        self.bindings.relative_anchor = outer;
        self.bindings.matching_relative_argument = outer_argument;
        matched
    }

    fn matches_compound_components(
        &mut self,
        compound: &CompoundRef<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        // https://drafts.csswg.org/css-scoping/#host-element-in-tree
        // In the context of its shadow tree, the shadow host is featureless: only `:host` and the like match it.
        // FIXME: `:host-context()`, which makes a compound match it.
        if let Some(shadow_root) = self.bindings.scope_shadow_root
            && self.subject.tree().shadow_root_of(node) == Some(shadow_root)
            && !self.bindings.matching_host_argument
        {
            return self.matches_featureless_host_components(compound, node, context);
        }
        let answer = self.test_components(compound.simple_selectors, node, context, false, |_| DecodedTest::Run)?;
        Ok(answer == Some(true))
    }

    /// Test `simple_selectors` of `node`, the names and states of a compound first, and what decodes its component
    /// after them, which `decided` can answer for instead: they reject most elements for less. Answers false at the
    /// first test that fails. Where `goes_on_unknown`, a test that cannot tell does not end the walk, as a later one
    /// may fail, and the answer is none unless one did; a failure to tell is the answer otherwise.
    #[inline(always)]
    fn test_components(
        &mut self,
        simple_selectors: crate::css::selector_arena::SimpleSelectors<'_>,
        node: S::Node,
        context: MatchContext<'_>,
        goes_on_unknown: bool,
        decided: impl Fn(&SimpleSelector<'_>) -> DecodedTest,
    ) -> Result<Option<bool>, S::Incomplete> {
        let mut known = true;
        // NB: The element's facts are fetched once for the names of the compound.
        let mut row: Option<S::Row> = None;
        let mut row_of = |evaluator: &mut Self| -> Result<S::Row, S::Incomplete> {
            if let Some(row) = row {
                return Ok(row);
            }
            let fetched = evaluator.subject.row(node)?;
            row = Some(fetched);
            Ok(fetched)
        };
        let mut deferred: SmallVec<[crate::css::selector_arena::SimpleView<'_>; 2]> = SmallVec::new();
        for view in simple_selectors.views() {
            let answer = match view {
                crate::css::selector_arena::SimpleView::Id(name) => {
                    row_of(self).map(|row| self.has_id_in(row, name, context))
                }
                crate::css::selector_arena::SimpleView::Class(name) => {
                    row_of(self).map(|row| self.has_class_in(row, name, context))
                }
                crate::css::selector_arena::SimpleView::TagName(name) => {
                    match self.is_in_namespace_of(&name, node, context) {
                        Ok(true) => row_of(self).map(|row| self.has_local_name_in(row, &name, context)),
                        other => other,
                    }
                }
                crate::css::selector_arena::SimpleView::PseudoClass {
                    kind,
                    is_search_text_filter,
                    ..
                } if is_bare_pseudo_class(kind, is_search_text_filter) => {
                    self.matches_bare_pseudo_class(kind, node, context)
                }
                other => {
                    deferred.push(other);
                    continue;
                }
            };
            match answer {
                Ok(true) => {}
                Ok(false) => return Ok(Some(false)),
                Err(incomplete) if !goes_on_unknown => return Err(incomplete),
                Err(_) => known = false,
            }
        }
        for view in deferred {
            let simple = &view.decode();
            match decided(simple) {
                DecodedTest::Run => {}
                DecodedTest::Holds => continue,
                DecodedTest::Unknown => {
                    known = false;
                    continue;
                }
            }
            match self.matches_simple_component(simple, node, context) {
                Ok(true) => {}
                Ok(false) => return Ok(Some(false)),
                Err(incomplete) if !goes_on_unknown => return Err(incomplete),
                Err(_) => known = false,
            }
        }
        Ok(known.then_some(true))
    }

    #[inline]
    fn has_tag_name(
        &mut self,
        name: &crate::css::selector::QualifiedName,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        if !self.is_in_namespace_of(name, node, context)? {
            return Ok(false);
        }
        let row = self.subject.row(node)?;
        Ok(self.has_local_name_in(row, name, context))
    }

    /// Whether the element whose facts `row` holds has the local name of the type selector `name`.
    #[inline]
    fn has_local_name_in(
        &self,
        row: S::Row,
        name: &crate::css::selector::QualifiedName,
        context: MatchContext<'_>,
    ) -> bool {
        let features = self.subject.features(row);
        // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
        let folds = !name.name_is_ascii_lowercase
            && !context.html_namespace.is_none()
            && features.namespace_is(context.html_namespace);
        let local = match folds {
            true => crate::css::style::process_atoms().ascii_lowercase(name.name),
            false => name.name,
        };
        features.local_name_is(local)
    }

    /// https://dom.spec.whatwg.org/#concept-id
    /// A quirks-mode document matches ids and classes ASCII case-insensitively.
    #[inline]
    fn has_id_in(&self, row: S::Row, name: crate::css::selector::NameSelector, context: MatchContext<'_>) -> bool {
        let id = match context.fold_ids_and_classes && !name.is_ascii_lowercase {
            true => crate::css::style::process_atoms().ascii_lowercase(name.name),
            false => name.name,
        };
        self.subject.features(row).has_id(id)
    }

    #[inline]
    fn has_class_in(&self, row: S::Row, name: crate::css::selector::NameSelector, context: MatchContext<'_>) -> bool {
        let class = match context.fold_ids_and_classes && !name.is_ascii_lowercase {
            true => crate::css::style::process_atoms().ascii_lowercase(name.name),
            false => name.name,
        };
        self.subject.features(row).has_class(class)
    }

    fn matches_simple_component(
        &mut self,
        simple: &SimpleSelector<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        match simple {
            SimpleSelector::Universal(name) => self.is_in_namespace_of(name, node, context),
            SimpleSelector::TagName(name) => self.has_tag_name(name, node, context),
            SimpleSelector::Id(name) => {
                let row = self.subject.row(node)?;
                Ok(self.has_id_in(row, *name, context))
            }
            SimpleSelector::Class(name) => {
                let row = self.subject.row(node)?;
                Ok(self.has_class_in(row, *name, context))
            }
            SimpleSelector::Attribute(attribute) => self.matches_attribute_component(attribute, node, context),
            SimpleSelector::PseudoClass(pseudo_class) => {
                self.matches_pseudo_class_component(pseudo_class, node, context)
            }
            // https://drafts.csswg.org/css-nesting/#nest-selector
            SimpleSelector::Nesting(Nesting::Rule(parent)) => self.matches_any(*parent, node, context),
            SimpleSelector::Nesting(Nesting::Root | Nesting::Scope) => Ok(self.is_scoping_root(node)),
            SimpleSelector::Nesting(Nesting::Unbound) => {
                unreachable!("placing a rule binds its `&` before it is matched")
            }
            // FIXME: Pseudo-elements.
            SimpleSelector::PseudoElement(_) => Ok(false),
            SimpleSelector::Invalid(_) => Ok(false),
        }
    }

    /// Whether the shadow host `node`, featureless in the tree whose selectors are matched, matches `compound`:
    /// every simple selector in it is a `:host` its argument matches.
    ///
    /// https://drafts.csswg.org/css-scoping/#host-selector
    fn matches_featureless_host_components(
        &mut self,
        compound: &CompoundRef<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        let mut names_the_host = false;
        for simple in compound.simple_selectors.iter() {
            let simple = &simple;
            // A logical pseudo-class or a `&` reaching the host matches it where an argument naming the host does.
            let arguments = match simple {
                SimpleSelector::PseudoClass(pseudo_class)
                    if matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where) =>
                {
                    Some(pseudo_class.argument_selector_list)
                }
                SimpleSelector::Nesting(Nesting::Rule(parent)) => Some(*parent),
                // The host can be the scoping root, which `:scope` and a `&` naming it name.
                SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Scope => {
                    if !self.is_scoping_root(node) {
                        return Ok(false);
                    }
                    names_the_host = true;
                    continue;
                }
                SimpleSelector::Nesting(Nesting::Root | Nesting::Scope) => {
                    if !self.is_scoping_root(node) {
                        return Ok(false);
                    }
                    names_the_host = true;
                    continue;
                }
                _ => None,
            };
            if let Some(arguments) = arguments {
                if !self.matches_any(arguments, node, context)? {
                    return Ok(false);
                }
                names_the_host = true;
                continue;
            }
            let SimpleSelector::PseudoClass(pseudo_class) = simple else {
                return Ok(false);
            };
            // A `:has()` beside `:host` is anchored on the host and reads the tree the host opens.
            if pseudo_class.pseudo_class == PseudoClassType::Has {
                let Some(shadow_root) = self.subject.tree().shadow_root_of(node) else {
                    return Ok(false);
                };
                if !self.matches_relative(pseudo_class.argument_selector_list, node, shadow_root, context)? {
                    return Ok(false);
                }
                continue;
            }
            if pseudo_class.pseudo_class != PseudoClassType::Host {
                return Ok(false);
            }
            names_the_host = true;
            if pseudo_class.argument_selector_list.is_empty() {
                continue;
            }
            let outer = std::mem::replace(&mut self.bindings.matching_host_argument, true);
            let matches = self.matches_any(pseudo_class.argument_selector_list, node, context);
            self.bindings.matching_host_argument = outer;
            if !matches? {
                return Ok(false);
            }
        }
        Ok(names_the_host)
    }

    /// Whether `node` is what `:scope` names: the bound scoping root, or else the root of its tree.
    fn is_scoping_root(&self, node: S::Node) -> bool {
        match self.bindings.scope_root_instance {
            Some(root) => root == node,
            None => self.subject.tree().live_parent(node).is_none(),
        }
    }

    fn matches_attribute_component(
        &mut self,
        attribute: &AttributeSelector<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        let atoms = crate::css::style::process_atoms();
        let row = self.subject.row(node)?;
        self.subject.read_attributes(row);
        let features = self.subject.features(row);
        let html_element = !context.html_namespace.is_none() && features.namespace_is(context.html_namespace);
        let written = attribute.qualified_name.name;
        // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
        // An attribute name is matched ASCII case-insensitively against an HTML element in an HTML document.
        let name = match html_element && !attribute.qualified_name.name_is_ascii_lowercase {
            true => atoms.ascii_lowercase(written),
            false => written,
        };
        // `[ns|x]` tests the name qualified by the namespace the prefix names, which only an attribute in that
        // namespace publishes.
        let qualified;
        let name = match attribute.qualified_name.namespace_type {
            NamespaceType::Named => {
                match context
                    .namespaces
                    .test_for(NamespaceType::Named, Some(attribute.qualified_name.namespace))
                {
                    Ok(Some(namespace)) if !namespace.is_none() => {
                        qualified = crate::css::style::AtomLease::acquire_qualified(atoms, namespace, name);
                        qualified.atom()
                    }
                    Ok(_) => name,
                    Err(_) => return Ok(false),
                }
            }
            // `[*|x]` tests the any-namespace name every attribute publishes, qualified by no namespace.
            NamespaceType::Any => {
                qualified = crate::css::style::AtomLease::acquire_qualified(atoms, StyleAtomID::NONE, name);
                qualified.atom()
            }
            _ => name,
        };
        let any_namespace = attribute.qualified_name.namespace_type == NamespaceType::Any;
        let operator = attribute.match_type;
        let insensitive = match attribute.case_type {
            AttributeCaseType::Insensitive => true,
            AttributeCaseType::Sensitive => false,
            AttributeCaseType::Default => html_element && attribute.names_a_legacy_case_insensitive_attribute,
        };
        // NB: Presence reads no value, which the values' subject would be asked for.
        if operator == AttributeMatchType::HasAttribute {
            return Ok(features.attributes_named(name, any_namespace).next().is_some());
        }
        let found: smallvec::SmallVec<[S::Attribute; 2]> = features.attributes_named(name, any_namespace).collect();
        Ok(found.into_iter().any(|found| {
            if operator == AttributeMatchType::ExactValue && !insensitive && !attribute.value_identity.is_none() {
                let value = self.subject.attribute_value_atom(found);
                if !value.is_none() {
                    return value == attribute.value_identity;
                }
            }
            match self.subject.attribute_value_text(found) {
                Some(crate::css::css_tokenizer::TokenizerInput::Latin1(value)) => {
                    attribute_value_matches(operator, value, attribute.value, insensitive)
                }
                Some(crate::css::css_tokenizer::TokenizerInput::Utf16(value)) => {
                    attribute_value_matches(operator, value, attribute.value, insensitive)
                }
                None => false,
            }
        }))
    }

    fn matches_pseudo_class_component(
        &mut self,
        pseudo_class: &PseudoClassSelector<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        use PseudoClassType as Pc;
        let is_search_text_filter = pseudo_class.pseudo_class == Pc::Not
            && crate::css::selector::search_text_match_filter_of_pseudo_class(pseudo_class).is_some();
        if is_bare_pseudo_class(pseudo_class.pseudo_class, is_search_text_filter) {
            return self.matches_bare_pseudo_class(pseudo_class.pseudo_class, node, context);
        }
        let position = |step, offset, from_end, of_type, of| Position {
            step,
            offset,
            from_end,
            of_type,
            of,
        };
        let pattern = pseudo_class.an_plus_b_pattern;
        let arguments = pseudo_class.argument_selector_list;
        match pseudo_class.pseudo_class {
            // https://drafts.csswg.org/selectors/#matches
            // An argument list whose every selector is invalid parses to an empty one, which matches nothing.
            Pc::Is | Pc::Where => self.matches_any(arguments, node, context),
            Pc::Not => Ok(!self.matches_any(arguments, node, context)?),
            Pc::Scope => Ok(self.is_scoping_root(node)),
            Pc::NthChild | Pc::NthLastChild => {
                let of = (!arguments.is_empty()).then_some(arguments);
                let from_end = pseudo_class.pseudo_class == Pc::NthLastChild;
                self.matches_position(
                    position(pattern.step_size, pattern.offset, from_end, false, of),
                    node,
                    context,
                )
            }
            Pc::NthOfType | Pc::NthLastOfType => {
                let from_end = pseudo_class.pseudo_class == Pc::NthLastOfType;
                self.matches_position(
                    position(pattern.step_size, pattern.offset, from_end, true, None),
                    node,
                    context,
                )
            }
            Pc::Lang => {
                let row = self.subject.row(node)?;
                let mut ranges = pseudo_class.languages.iter().map(|range| range.value);
                // An element with no resolved language matches no range at all, not even `*`.
                Ok(match self.subject.language_tag(row) {
                    crate::css::css_tokenizer::TokenizerInput::Latin1(tag) => {
                        !tag.is_empty()
                            && ranges.any(|range| crate::css::selector::language_range_matches_tag(range, tag))
                    }
                    crate::css::css_tokenizer::TokenizerInput::Utf16(tag) => {
                        !tag.is_empty()
                            && ranges.any(|range| crate::css::selector::language_range_matches_tag(range, tag))
                    }
                })
            }
            Pc::Dir => {
                // NB: The arena holds the keyword in ASCII lowercase, as the directionality is compared.
                let keyword = pseudo_class.identifier_identity;
                if keyword.is_none() {
                    return Ok(false);
                }
                let row = self.subject.row(node)?;
                Ok(self.subject.directionality_is(row, keyword))
            }
            Pc::State => {
                let row = self.subject.row(node)?;
                Ok(!pseudo_class.identifier_identity.is_none()
                    && self.subject.has_custom_state(row, pseudo_class.identifier_identity))
            }
            Pc::Heading => {
                let row = self.subject.row(node)?;
                let level = self.subject.heading_level(row);
                Ok((1..=9).contains(&level)
                    && (pseudo_class.levels.is_empty() || pseudo_class.levels.contains(&i64::from(level))))
            }
            // A `:host` reaches no element but the shadow host, which the compound walk answers for.
            Pc::Host => Ok(false),
            // https://drafts.csswg.org/selectors/#relational
            // A `:has()` does not nest, not even through a `&` naming a rule whose selectors have one.
            Pc::Has if self.bindings.matching_relative_argument => Ok(false),
            Pc::Has => self.matches_relative(arguments, node, node, context),
            // A search text filter picks the kind of search text match a `::search-text` styles, which the entry
            // carries, and constrains no element.
            _ => Ok(false),
        }
    }

    /// Whether `node` matches a pseudo-class of the kind `kind` taking no argument, which
    /// [`is_bare_pseudo_class`] says it is, read from its kind alone.
    fn matches_bare_pseudo_class(
        &mut self,
        kind: PseudoClassType,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Answer<S> {
        use PseudoClassType as Pc;
        if let Some(fact) = super::compiler::state_fact_for(kind) {
            let row = self.subject.row(node)?;
            return Ok(self.subject.has_state(row, fact));
        }
        let first = |from_end, of_type| Position {
            step: 0,
            offset: 1,
            from_end,
            of_type,
            of: None,
        };
        match kind {
            // A search text filter picks the kind of search text match a `::search-text` styles, which the entry
            // carries, and constrains no element.
            Pc::Not | Pc::Current => Ok(true),
            Pc::Root => Ok(self.subject.is_root(node)),
            Pc::Empty => {
                self.subject.note_structural_read(node, StructuralRead::Emptiness);
                self.subject.is_empty(node)
            }
            Pc::FirstChild => self.matches_position(first(false, false), node, context),
            Pc::LastChild => self.matches_position(first(true, false), node, context),
            Pc::FirstOfType => self.matches_position(first(false, true), node, context),
            Pc::LastOfType => self.matches_position(first(true, true), node, context),
            Pc::OnlyChild => Ok(self.matches_position(first(false, false), node, context)?
                && self.matches_position(first(true, false), node, context)?),
            Pc::OnlyOfType => Ok(self.matches_position(first(false, true), node, context)?
                && self.matches_position(first(true, true), node, context)?),
            _ => unreachable!("a pseudo-class taking an argument is matched decoded"),
        }
    }

    /// Whether `node` sits at a position `position` names among its inclusive siblings.
    ///
    /// https://drafts.csswg.org/selectors/#child-index
    fn matches_position(&mut self, position: Position<'_>, node: S::Node, context: MatchContext<'_>) -> Answer<S> {
        self.subject.note_structural_read(
            node,
            StructuralRead::Position {
                from_end: position.from_end,
                of_type: position.of_type,
                beyond_edge: position.step != 0 || position.offset != 1 || position.of.is_some(),
            },
        );
        if position.of.is_none() {
            let remembered = super::selector::NthPosition {
                step: position.step,
                offset: position.offset,
                from_end: position.from_end,
                of_type: position.of_type,
            };
            if let Some(index) = self.subject.sibling_index(remembered, node)? {
                return Ok(super::selector::matches_an_plus_b(
                    position.step,
                    position.offset,
                    index,
                ));
            }
        }
        if let Some(of) = position.of
            && !self.matches_any(of, node, context)?
        {
            return Ok(false);
        }
        // An index among the siblings `of S` matches is remembered by the list, as each sibling is counted once.
        let of_key = match self.bindings.matching_host_argument {
            false => position.of.and_then(SelectorListView::identity),
            true => None,
        };
        if let Some(key) = of_key
            && self.subject.remembers_of_indices()
            && let Some(index) = self.of_index(key, position, node, context)?
        {
            return Ok(super::selector::matches_an_plus_b(
                position.step,
                position.offset,
                index,
            ));
        }
        let subject_row = match position.of_type {
            true => Some(self.subject.row(node)?),
            false => None,
        };
        let tree = self.subject.tree();
        let mut index: i64 = 1;
        let mut current = match position.from_end {
            true => tree.next_sibling(node),
            false => tree.previous_sibling(node),
        };
        while let Some(sibling) = current {
            let counts = match (subject_row, position.of) {
                (Some(row), _) => {
                    let sibling_row = self.subject.row(sibling)?;
                    self.subject.same_type(row, sibling_row)
                }
                (None, Some(of)) => self.matches_any(of, sibling, context)?,
                (None, None) => true,
            };
            // A matching sibling with its index remembered counts every matching sibling beyond it.
            if counts
                && let Some(key) = of_key
                && let Some(remembered) = self.subject.of_index_of(key, position.from_end, sibling)
            {
                index += remembered;
                break;
            }
            index += i64::from(counts);
            // A test with no step names one position, which no sibling further away can bring the count back to.
            if position.step == 0 && index > i64::from(position.offset) {
                if position.of_type {
                    self.subject.row(node)?;
                }
                return Ok(false);
            }
            current = match position.from_end {
                true => tree.next_sibling(sibling),
                false => tree.previous_sibling(sibling),
            };
        }
        // NB: A compound reads its element's names once, and the subject may hold the sibling it read last instead.
        if position.of_type {
            self.subject.row(node)?;
        }
        if let Some(key) = of_key {
            self.subject.record_of_index(key, position.from_end, node, index);
        }
        Ok(super::selector::matches_an_plus_b(
            position.step,
            position.offset,
            index,
        ))
    }

    /// The index of `node`, which matches the `of S` list `key` names, among its inclusive siblings that match it,
    /// counted from the end: one less than that of the nearest earlier sibling matching it, when that is remembered.
    /// None for a count from the start, which the walk back finds.
    fn of_index(
        &mut self,
        key: super::selector_evaluation::SelectorListKey,
        position: Position<'_>,
        node: S::Node,
        context: MatchContext<'_>,
    ) -> Result<Option<i64>, S::Incomplete> {
        let Some(of) = position.of.filter(|_| position.from_end) else {
            return Ok(None);
        };
        let tree = self.subject.tree();
        let mut sibling = tree.previous_sibling(node);
        while let Some(current) = sibling {
            if self.matches_any(of, current, context)? {
                let index = self.subject.of_index_of(key, true, current).map(|later| later - 1);
                if let Some(index) = index {
                    self.subject.record_of_index(key, true, node, index);
                }
                return Ok(index);
            }
            sibling = tree.previous_sibling(current);
        }
        Ok(None)
    }
}

/// Whether a pseudo-class of the kind `kind` is answered from its kind alone, with no argument to decode: a state, a
/// structural test of the first or last position, `:root`, `:empty`, or a `:not()` that only filters search text.
fn is_bare_pseudo_class(kind: PseudoClassType, is_search_text_filter: bool) -> bool {
    use PseudoClassType as Pc;
    super::compiler::state_fact_for(kind).is_some()
        || matches!(
            kind,
            Pc::Current
                | Pc::Root
                | Pc::Empty
                | Pc::FirstChild
                | Pc::LastChild
                | Pc::FirstOfType
                | Pc::LastOfType
                | Pc::OnlyChild
                | Pc::OnlyOfType
        )
        || (kind == Pc::Not && is_search_text_filter)
}

/// What a caller of a compound's tests says of one that decodes its component.
enum DecodedTest {
    /// It is tested.
    Run,
    /// It is taken to hold.
    Holds,
    /// It cannot be told, so neither can the compound's answer, unless another test fails.
    Unknown,
}

/// The dispatch bloom bits an element has to carry to match the first compound of `compound`: those of the ids and
/// classes it tests for.
fn required_dispatch_bloom(compound: Selector<'_>, context: MatchContext<'_>) -> u64 {
    // NB: The arena notes the bits of the names as written, which a document folding ids and classes folds first.
    if !context.fold_ids_and_classes {
        return compound.required_dispatch_bloom();
    }
    let folded = |name: crate::css::selector::NameSelector| match name.is_ascii_lowercase {
        true => name.name,
        false => crate::css::style::process_atoms().ascii_lowercase(name.name),
    };
    compound.ids_and_classes().fold(0, |bloom, name| match name {
        IdOrClass::Class(name) => bloom | dispatch_bloom_bit(DispatchKey::Class(folded(name))),
        IdOrClass::Id(name) => bloom | dispatch_bloom_bit(DispatchKey::Id(folded(name))),
    })
}

/// The axis from the anchor the subject of the relative selector `selector` is found on, and whether the subject
/// stands strictly below the elements the axis reaches.
pub(crate) fn relative_axis(selector: Selector<'_>) -> (super::relative_selector::RelativeAxis, bool) {
    use super::relative_selector::RelativeAxis;
    let descends = |combinator: Combinator| matches!(combinator, Combinator::Descendant | Combinator::ImmediateChild);
    // NB: Read from the subject leftwards, each compound's combinator relates it to the compound left of it: the
    //     leftmost one's is the leading combinator, and the one read right before it is the first written after it.
    let mut first_later = None;
    let mut later_descends = false;
    let mut current = selector;
    let leading = loop {
        let combinator = current.first_compound().combinator;
        match current.left() {
            Some((_, left)) if !left.is_relative_anchor() => {
                later_descends |= descends(combinator);
                first_later = Some(combinator);
                current = left;
            }
            _ => break combinator,
        }
    };
    match leading {
        Combinator::ImmediateChild if !later_descends => (RelativeAxis::Child, false),
        Combinator::NextSibling if first_later.is_none() => (RelativeAxis::NextSibling, false),
        Combinator::NextSibling if first_later.is_some_and(descends) => (RelativeAxis::NextSiblingSubtree, true),
        Combinator::NextSibling | Combinator::SubsequentSibling => match later_descends {
            false => (RelativeAxis::FollowingSibling, false),
            true => (RelativeAxis::FollowingSiblingSubtree, true),
        },
        _ => (RelativeAxis::Descendant, false),
    }
}

/// The ranges of `scope.roots` and `scope.limits` that level `level` wrote.
fn scope_level_ranges(
    scope: &super::bridge::BoundScopeChain,
    level: usize,
) -> (std::ops::Range<usize>, std::ops::Range<usize>) {
    let (mut root_start, mut limit_start) = (0usize, 0usize);
    for (index, &(root_count, limit_count)) in scope.levels.iter().enumerate() {
        let root_end = (root_start + root_count as usize).min(scope.roots.len());
        let limit_end = (limit_start + limit_count as usize).min(scope.limits.len());
        if index == level {
            return (root_start..root_end, limit_start..limit_end);
        }
        root_start = root_end;
        limit_start = limit_end;
    }
    (0..0, 0..0)
}

/// The selector left of the first compound of `selector`, or none where that compound is the leftmost one written.
fn left_of(selector: Selector<'_>) -> Option<Selector<'_>> {
    selector
        .left()
        .map(|(_, left)| left)
        .filter(|left| !left.is_relative_anchor())
}

/// Whether a selector whose first compound in matching order, the one it ends in, is `last` targets a
/// pseudo-element: that compound is one after a pseudo-element combinator, other than `::part()` or `::slotted()`.
fn targets_a_pseudo_element(last: &CompoundRef<'_>) -> bool {
    last.combinator == Combinator::PseudoElement
        && matches!(
            compound_target(last),
            CompoundTarget::Transparent | CompoundTarget::Backed { .. }
        )
}

/// What the pseudo-element a compound starts with, if any, makes the compound describe.
pub(crate) enum CompoundTarget<'a> {
    /// The compound starts with none, and describes the element.
    Element,
    /// A pseudo-element no element stands for, of the element the compound left of it describes.
    Transparent,
    /// An element-backed pseudo-element, with pseudo-classes written after it that describe its element.
    Backed { kind: u16 },
    /// `::part()`, whose names the element exposed.
    Part { names: &'a [StyleAtomID] },
    /// `::slotted()`, whose argument, if it has one, describes the element.
    Slotted { argument: Option<Selector<'a>> },
}

/// See [`CompoundTarget`].
pub(crate) fn compound_target<'a>(compound: &CompoundRef<'a>) -> CompoundTarget<'a> {
    use crate::css::selector::{PseudoElementType, PseudoElementValue};
    let Some(SimpleSelector::PseudoElement(pseudo_element)) = compound.simple_selectors.first() else {
        return CompoundTarget::Element;
    };
    match pseudo_element.pseudo_element {
        PseudoElementType::Part => CompoundTarget::Part {
            names: pseudo_element.identifier_identities,
        },
        PseudoElementType::Slotted => CompoundTarget::Slotted {
            argument: match pseudo_element.value {
                PseudoElementValue::CompoundSelector(argument) => Some(argument),
                _ => None,
            },
        },
        other => {
            let kind = other as u16;
            let backed_kinds = u16::from(super::bridge::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND)
                ..=u16::from(super::bridge::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND);
            match backed_kinds.contains(&kind) && !compound.simple_selectors.after_first().is_empty() {
                true => CompoundTarget::Backed { kind },
                false => CompoundTarget::Transparent,
            }
        }
    }
}

/// The kind of the element-backed pseudo-element a compound names, when it also writes pseudo-classes after it.
pub(crate) fn backed_pseudo_element_kind(compound: &CompoundRef<'_>) -> Option<u16> {
    match compound_target(compound) {
        CompoundTarget::Backed { kind } => Some(kind),
        _ => None,
    }
}

/// The names of a compound that starts with `::part()` and goes on with pseudo-classes only.
fn part_names<'a>(compound: &CompoundRef<'a>) -> Option<&'a [StyleAtomID]> {
    if compound.combinator != Combinator::PseudoElement {
        return None;
    }
    let CompoundTarget::Part { names } = compound_target(compound) else {
        return None;
    };
    compound
        .simple_selectors
        .after_first()
        .iter()
        .all(|simple| matches!(simple, SimpleSelector::PseudoClass(_)))
        .then_some(names)
}

/// The argument of a compound that is `::slotted()` alone, after the compounds naming the slot.
fn slotted_argument<'a>(compound: &CompoundRef<'a>) -> Option<Selector<'a>> {
    if compound.combinator != Combinator::PseudoElement || compound.simple_selectors.len() != 1 {
        return None;
    }
    match compound_target(compound) {
        CompoundTarget::Slotted { argument } => argument,
        _ => None,
    }
}

/// An indexed entry's selector, and what the rule it is of says about matching it.
#[derive(Clone, Copy)]
pub(crate) struct EntrySelector<'a> {
    pub(crate) selector: Selector<'a>,
    scope: Option<&'a super::bridge::BoundScopeChain>,
}

impl<'a> EntrySelector<'a> {
    /// The selector of an entry whose rule is inside the `@scope` levels `scope`, if any.
    pub(crate) fn new(selector: Selector<'a>, scope: Option<&'a super::bridge::BoundScopeChain>) -> Self {
        Self { selector, scope }
    }

    /// The selector of an entry whose rule is inside no `@scope`.
    pub(crate) fn unscoped(selector: Selector<'a>) -> Self {
        Self { selector, scope: None }
    }

    /// The `@scope` levels of the entry's rule, outermost first, or none outside any.
    pub(crate) fn scope(self) -> Option<&'a super::bridge::BoundScopeChain> {
        self.scope
    }

    /// What matching the selector in a document `document` describes needs to know.
    fn context(self, document: DocumentMatching) -> MatchContext<'a> {
        // NB: Only a default namespace is escaped, which few sheets declare.
        let escapes_its_namespace =
            self.selector.namespaces().default.is_some() && self.selector.escapes_its_namespace();
        MatchContext {
            html_namespace: document.html_namespace,
            fold_ids_and_classes: document.fold_ids_and_classes,
            namespaces: self.selector.namespaces(),
            escapes_its_namespace,
            logical_subject: false,
        }
    }
}

/// Whether the pseudo-class `kind` tests a state of the element.
pub(crate) fn names_a_state(kind: PseudoClassType) -> bool {
    super::compiler::state_fact_for(kind).is_some()
}

/// The state of the element the pseudo-class `kind` tests, where it tests one.
pub(crate) fn state_tested_by(kind: PseudoClassType) -> Option<super::input_types::StateFact> {
    super::compiler::state_fact_for(kind)
}

/// Whether `simple` holds a `:has()`, in itself or in a list it takes.
pub(crate) fn mentions_relative(simple: &SimpleSelector<'_>) -> bool {
    mentions(
        simple,
        &|simple| matches!(simple, SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Has),
    )
}

/// Whether `simple` reads the scoping root, which an element's own facts do not tell: `:scope`, or a `&` naming it.
pub(crate) fn reads_the_scope_root(simple: &SimpleSelector<'_>) -> bool {
    mentions(simple, &|simple| match simple {
        SimpleSelector::PseudoClass(pseudo_class) => pseudo_class.pseudo_class == PseudoClassType::Scope,
        SimpleSelector::Nesting(Nesting::Root | Nesting::Scope) => true,
        _ => false,
    })
}

/// Whether `simple`, or a simple selector of a list it takes, is one `wanted` holds for.
pub(crate) fn mentions(simple: &SimpleSelector<'_>, wanted: &impl Fn(&SimpleSelector<'_>) -> bool) -> bool {
    if wanted(simple) {
        return true;
    }
    let in_selector = |selector: Selector<'_>| {
        selector
            .compounds()
            .any(|compound| compound.simple_selectors.iter().any(|simple| mentions(&simple, wanted)))
    };
    simple.nested().any(in_selector)
}

/// What matching in a document needs to know of it.
#[derive(Clone, Copy)]
pub(crate) struct DocumentMatching {
    /// The HTML namespace in an HTML document, and none in any other.
    pub(crate) html_namespace: StyleAtomID,
    /// Whether ids and classes compare ASCII case-insensitively, as in a quirks-mode document.
    pub(crate) fold_ids_and_classes: bool,
}

impl<S: SelectorSubject> SelectorEvaluator<S> {
    /// Whether `node` matches the selector of an entry, inside the `@scope` levels of its rule.
    pub(crate) fn matches_entry_selector(
        &mut self,
        entry: EntrySelector<'_>,
        node: S::Node,
        document: DocumentMatching,
    ) -> Answer<S> {
        let context = entry.context(document);
        self.bindings.matched_scope_root = None;
        match entry.scope {
            Some(scope) => {
                let names_the_scope = entry.selector.names_the_scope();
                self.matches_scope_level(scope, 0, entry.selector, names_the_scope, node, context)
            }
            None => self.matches_selector(entry.selector, node, context),
        }
    }

    /// Whether `node`, known to match the subject compound of the unscoped selector of `entry`, matches what is left
    /// of it.
    pub(crate) fn matches_left_of_subject(
        &mut self,
        entry: EntrySelector<'_>,
        node: S::Node,
        document: DocumentMatching,
    ) -> Answer<S> {
        let context = entry.context(document);
        self.bindings.matched_scope_root = None;
        Ok(self.walk_left_of(entry.selector, None, node, context)? == Walk::Matched)
    }

    /// Whether `node` matches the first compound of `compound`, a compound of the selector of `entry` or of one
    /// nested in it, on its own: whatever the elements its combinators reach match. None where that cannot be told
    /// from the element alone: the compound reads the scoping root, or what the subject does not know.
    ///
    /// Where `around_relative`, the `:has()` of the compound are taken to match. The argument of a `::slotted()`
    /// describes the element, and the pseudo-classes after an element-backed pseudo-element the element backing it.
    pub(crate) fn matches_compound_alone(
        &mut self,
        entry: EntrySelector<'_>,
        compound: Selector<'_>,
        node: S::Node,
        document: DocumentMatching,
        around_relative: bool,
    ) -> Option<bool> {
        if compound.is_relative_anchor() {
            return None;
        }
        let context = entry.context(document);
        let first = compound.first_compound();
        // NB: Only a compound starting with a pseudo-element, which the first component's kind tells
        //     without decoding it, describes another element than its own.
        let may_lead_with_a_pseudo_element = matches!(
            first.simple_selectors.views().next(),
            Some(crate::css::selector_arena::SimpleView::Other(_))
        );
        let simple_selectors = match may_lead_with_a_pseudo_element.then(|| compound_target(&first)) {
            Some(CompoundTarget::Slotted {
                argument: Some(argument),
            }) => argument.first_compound().simple_selectors,
            // NB: Whether the host the rule names is exposed the part is left to the restyle.
            Some(CompoundTarget::Part { names }) => {
                let row = self.subject.row(node).ok()?;
                if !names.iter().all(|&part| self.subject.has_part(row, part)) {
                    return Some(false);
                }
                first.simple_selectors.after_first()
            }
            Some(CompoundTarget::Backed { .. }) => first.simple_selectors.after_first(),
            Some(CompoundTarget::Transparent | CompoundTarget::Slotted { argument: None }) => return Some(true),
            Some(CompoundTarget::Element) | None => first.simple_selectors,
        };
        if let Some(shadow_root) = self.bindings.scope_shadow_root
            && self.subject.tree().shadow_root_of(node) == Some(shadow_root)
            && !self.bindings.matching_host_argument
        {
            if simple_selectors.iter().any(|simple| reads_the_scope_root(&simple)) {
                return None;
            }
            return self.matches_featureless_host_components(&first, node, context).ok();
        }
        let known = match self.test_components(simple_selectors, node, context, true, |simple| match () {
            _ if around_relative && mentions_relative(simple) => DecodedTest::Holds,
            _ if reads_the_scope_root(simple) => DecodedTest::Unknown,
            _ => DecodedTest::Run,
        }) {
            Ok(Some(false)) => return Some(false),
            Ok(Some(true)) => true,
            Ok(None) | Err(_) => false,
        };
        let compound = CompoundRef {
            simple_selectors,
            ..first
        };
        match self.is_outside_the_default_namespace(&compound, false, node, context) {
            Ok(true) => Some(false),
            Ok(false) => known.then_some(true),
            Err(_) => None,
        }
    }

    /// Whether an element relative to `anchor` matches `argument`, a relative selector of a `:has()` in the selector
    /// of `entry`, walking the shadow tree `anchor` hosts where `in_shadow_tree`.
    pub(crate) fn matches_relative_query(
        &mut self,
        entry: EntrySelector<'_>,
        argument: Selector<'_>,
        in_shadow_tree: bool,
        anchor: S::Node,
        document: DocumentMatching,
    ) -> Answer<S> {
        let context = entry.context(document);
        let within = match in_shadow_tree {
            true => match self.subject.tree().shadow_root_of(anchor) {
                Some(shadow_root) => shadow_root,
                None => return Ok(false),
            },
            false => anchor,
        };
        self.matches_relative_selector(argument, anchor, within, context)
    }

    /// How far `node`, which the last entry asked for matched, is from the scoping root of the scope it matched in:
    /// the hops up to it, or infinitely far for an entry no `@scope` constrains.
    ///
    /// https://drafts.csswg.org/css-cascade-6/#cascade-proximity
    pub(crate) fn scope_proximity_of_match(&self, node: S::Node) -> u32 {
        let Some(root) = self.bindings.matched_scope_root else {
            return u32::MAX;
        };
        let tree = self.subject.tree();
        let mut proximity = 0;
        let mut candidate = Some(node);
        while let Some(current) = candidate {
            if current == root {
                return proximity;
            }
            proximity += 1;
            candidate = tree
                .live_parent(current)
                .map(|parent| tree.host_of(parent).unwrap_or(parent));
        }
        u32::MAX
    }
}

/// https://drafts.csswg.org/css-namespaces/#css-qnames
/// A qualified name with a prefix that has not been previously declared as a namespace prefix is invalid. The parser
/// rejects such a selector, so one that reaches matching anyway matches nothing rather than every namespace.
pub(crate) struct UndeclaredPrefix;

impl crate::css::selector_arena::SheetNamespaces<'_> {
    /// The namespace a qualified name constrains its subject to, none being the absence of one, where it constrains
    /// it at all, or an error for a prefix the sheet never declared. The empty string is the namespace an element in no
    /// namespace has, so declaring it names exactly those.
    pub(crate) fn test_for(
        self,
        namespace_type: NamespaceType,
        prefix: Option<StyleAtomID>,
    ) -> Result<Option<StyleAtomID>, UndeclaredPrefix> {
        Ok(match namespace_type {
            // `*|x` names every namespace, so it constrains nothing.
            NamespaceType::Any => None,
            // `|x` names the absence of one.
            NamespaceType::None => Some(StyleAtomID::NONE),
            NamespaceType::Default => self.default,
            NamespaceType::Named => prefix
                .and_then(|prefix| self.namespace_of(prefix))
                .ok_or(UndeclaredPrefix)?
                .into(),
        })
    }
}
