/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the rule index reads of an entry's selector, from its components: which families of
//! buckets it is in, whether it revalidates, the ids it tests for, and the key a compound reached
//! by no name is bucketed by.

use super::StyleAtomID;
use super::component_matching::{CompoundTarget, EntrySelector, compound_target, mentions};
use super::index::DispatchKey;
use crate::css::selector::{
    Combinator, CompoundRef, PseudoClassType, PseudoElementType, PseudoElementValue, Selector, SelectorListView,
    SimpleSelector,
};
use crate::css::selector_arena::{Nesting, SheetNamespaces};
use smallvec::SmallVec;

/// What the rule index reads of one entry.
#[derive(Clone, Copy)]
pub(super) struct EntryFacts {
    pub(super) never_matches: bool,
    pub(super) scoped: bool,
    pub(super) revalidates: bool,
    pub(super) host: bool,
    pub(super) slotted: bool,
    pub(super) part: bool,
}

impl EntryFacts {
    pub(super) fn of(entry: EntrySelector<'_>) -> Self {
        let scoped = entry.scope().is_some();
        let subject = subject_compound(entry.selector);
        let first = subject.first_compound();
        let pseudo_element = match first.simple_selectors.first() {
            Some(SimpleSelector::PseudoElement(pseudo_element)) => Some(pseudo_element.pseudo_element),
            _ => None,
        };
        Self {
            never_matches: never_matches(&first),
            scoped,
            revalidates: scoped || revalidates(entry.selector),
            // NB: Asking the host is the wider of the two answers where the subject may be the
            //     scoping root, which can be the host.
            host: scoped || names_the_host(&first),
            slotted: pseudo_element == Some(PseudoElementType::Slotted),
            part: pseudo_element == Some(PseudoElementType::Part),
        }
    }
}

/// The compound of `selector` an element matching it is the subject of: its first, or the one
/// left of a pseudo-element no element stands for.
pub(super) fn subject_compound(selector: Selector<'_>) -> Selector<'_> {
    let first = selector.first_compound();
    let transparent = matches!(compound_target(&first), CompoundTarget::Transparent);
    match (transparent, selector.left()) {
        (true, Some((Combinator::PseudoElement, left))) => left,
        _ => selector,
    }
}

/// Whether the compound tests something no element satisfies, as a `:has()`, `:is()` or
/// `:where()` all of whose arguments were invalid does.
pub(crate) fn never_matches(compound: &CompoundRef<'_>) -> bool {
    compound.simple_selectors.iter().any(|simple| match simple {
        SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
            PseudoClassType::Has | PseudoClassType::Is | PseudoClassType::Where => {
                pseudo_class.argument_selector_list.is_empty()
            }
            PseudoClassType::State => pseudo_class.identifier_identity.is_none(),
            _ => false,
        },
        SimpleSelector::Invalid(_) => true,
        _ => false,
    })
}

/// Whether a compound can match the featureless host of the tree its rule is in: one of its
/// simple selectors names the host or the scoping root, and every other one may match the host.
fn names_the_host(compound: &CompoundRef<'_>) -> bool {
    let names = |simple: &SimpleSelector<'_>| {
        mentions(simple, &|simple| match simple {
            SimpleSelector::PseudoClass(pseudo_class) => {
                matches!(
                    pseudo_class.pseudo_class,
                    PseudoClassType::Host | PseudoClassType::Scope
                )
            }
            SimpleSelector::Nesting(Nesting::Root | Nesting::Scope) => true,
            _ => false,
        })
    };
    compound.simple_selectors.iter().any(|simple| names(&simple))
}

/// Whether a selector can tell apart two elements that agree on the style sharing key: their tag,
/// namespace, classes and states, the absence of an id a selector tests for, and parents that are
/// the same element or themselves shared a style, all the way up. What it reads of the elements
/// and their ancestors beyond that key, such as attributes, siblings and tree position, it has to
/// be evaluated for.
fn revalidates(selector: Selector<'_>) -> bool {
    let mut compound = Some(subject_compound(selector));
    while let Some(current) = compound {
        if current.is_relative_anchor() {
            return true;
        }
        if current
            .first_compound()
            .simple_selectors
            .iter()
            .any(|simple| simple_revalidates(&simple))
        {
            return true;
        }
        compound = match current.left() {
            None => None,
            Some((Combinator::ImmediateChild | Combinator::Descendant, left)) => Some(left),
            Some(_) => return true,
        };
    }
    false
}

fn simple_revalidates(simple: &SimpleSelector<'_>) -> bool {
    let any = |list: SelectorListView<'_>| list.iter().any(revalidates);
    match simple {
        SimpleSelector::Universal(_)
        | SimpleSelector::TagName(_)
        | SimpleSelector::Id(_)
        | SimpleSelector::Class(_) => false,
        SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
            PseudoClassType::Root => false,
            PseudoClassType::Is | PseudoClassType::Where | PseudoClassType::Not => {
                any(pseudo_class.argument_selector_list)
            }
            kind => super::compiler::state_fact_for(kind).is_none(),
        },
        SimpleSelector::Nesting(Nesting::Rule(parent)) => any(*parent),
        _ => true,
    }
}

/// Collect the ids `selector`, which does not revalidate, tests for, in each form an element
/// carries them.
pub(super) fn collect_tested_ids(selector: Selector<'_>, ids: &mut Vec<StyleAtomID>) {
    let atoms = super::process_atoms();
    for current in subject_compound(selector).compounds() {
        for simple in current.simple_selectors.iter() {
            let simple = &simple;
            match simple {
                SimpleSelector::Id(name) => {
                    ids.push(name.name);
                    if !name.is_ascii_lowercase {
                        ids.push(atoms.ascii_lowercase(name.name));
                    }
                }
                SimpleSelector::PseudoClass(pseudo_class)
                    if matches!(
                        pseudo_class.pseudo_class,
                        PseudoClassType::Is | PseudoClassType::Where | PseudoClassType::Not
                    ) =>
                {
                    for selector in pseudo_class.argument_selector_list {
                        collect_tested_ids(selector, ids);
                    }
                }
                SimpleSelector::Nesting(Nesting::Rule(parent)) => {
                    for selector in *parent {
                        collect_tested_ids(selector, ids);
                    }
                }
                _ => {}
            }
        }
    }
}

/// A key only elements the subject compound of `selector` can match carry, for a compound reached
/// by no name: a state, being a heading, or being the document element.
pub(super) fn state_dispatch_key(selector: Selector<'_>) -> Option<DispatchKey> {
    let compound = subject_compound(selector).first_compound();
    compound.simple_selectors.iter().find_map(|simple| match simple {
        SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
            PseudoClassType::Heading => Some(DispatchKey::Heading),
            PseudoClassType::Root => Some(DispatchKey::Root),
            kind => super::compiler::state_fact_for(kind).map(DispatchKey::State),
        },
        _ => None,
    })
}

/// How many keys a compound may carry before checking them all costs more than the rejections save.
const MAX_DISPATCH_KEYS: usize = 8;

/// The keys an element matching the subject compound of `selector` carries one of, which its entry
/// is bucketed by, or none where it carries none in particular: the universal bucket's.
///
/// A name not written in ASCII lowercase is bucketed under its lowercase form too, which a
/// quirks-mode document, an HTML element's tag name and an HTML attribute name are carried in.
pub(super) fn subject_dispatch_keys(selector: Selector<'_>) -> SmallVec<[DispatchKey; 4]> {
    let mut keys = compound_dispatch_keys(&subject_compound(selector).first_compound()).unwrap_or_default();
    keys.sort_unstable();
    keys.dedup();
    keys
}

/// The cheapest set of keys one of which every element matching `compound` carries.
fn compound_dispatch_keys(compound: &CompoundRef<'_>) -> Option<SmallVec<[DispatchKey; 4]>> {
    compound
        .simple_selectors
        .iter()
        .filter_map(|simple| simple_dispatch_keys(&simple))
        .min_by_key(|keys| {
            keys.iter()
                .map(|&key| u32::from(dispatch_selectivity(key)) + 1)
                .sum::<u32>()
        })
}

fn simple_dispatch_keys(simple: &SimpleSelector<'_>) -> Option<SmallVec<[DispatchKey; 4]>> {
    let atoms = super::process_atoms();
    let both = |make: fn(StyleAtomID) -> DispatchKey, name: StyleAtomID, lowercase: bool| {
        let mut keys: SmallVec<[DispatchKey; 4]> = smallvec::smallvec![make(name)];
        if !lowercase {
            keys.push(make(atoms.ascii_lowercase(name)));
        }
        Some(keys)
    };
    let alternatives = |list: SelectorListView<'_>| {
        let mut keys: SmallVec<[DispatchKey; 4]> = SmallVec::new();
        for selector in list {
            keys.extend(compound_dispatch_keys(&selector.first_compound())?);
            if keys.len() > MAX_DISPATCH_KEYS {
                return None;
            }
        }
        (!keys.is_empty()).then_some(keys)
    };
    match simple {
        SimpleSelector::Id(name) => both(DispatchKey::Id, name.name, name.is_ascii_lowercase),
        SimpleSelector::Class(name) => both(DispatchKey::Class, name.name, name.is_ascii_lowercase),
        SimpleSelector::TagName(name) => both(DispatchKey::TagName, name.name, name.name_is_ascii_lowercase),
        SimpleSelector::Attribute(attribute) => both(
            DispatchKey::AttributeName,
            attribute.qualified_name.name,
            attribute.qualified_name.name_is_ascii_lowercase,
        )
        // NB: A name qualified by a namespace is carried qualified, which the components do not hold.
        .filter(|_| {
            matches!(
                attribute.qualified_name.namespace_type,
                crate::css::selector::NamespaceType::Default | crate::css::selector::NamespaceType::None
            )
        }),
        SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
            PseudoClassType::Is | PseudoClassType::Where => alternatives(pseudo_class.argument_selector_list),
            PseudoClassType::Host => pseudo_class
                .argument_selector_list
                .single()
                .and_then(|argument| compound_dispatch_keys(&argument.first_compound())),
            PseudoClassType::State if !pseudo_class.identifier_identity.is_none() => {
                Some(smallvec::smallvec![DispatchKey::CustomState(
                    pseudo_class.identifier_identity
                )])
            }
            // NB: A state without a posting names nothing to find its elements by, and admitting it
            //     would take the compound away from names that do.
            kind => super::compiler::state_fact_for(kind)
                .filter(|fact| fact.has_selector_posting())
                .map(|fact| smallvec::smallvec![DispatchKey::State(fact)]),
        },
        SimpleSelector::PseudoElement(pseudo_element) => match pseudo_element.value {
            PseudoElementValue::CompoundSelector(argument)
                if pseudo_element.pseudo_element == PseudoElementType::Slotted =>
            {
                compound_dispatch_keys(&argument.first_compound())
            }
            _ if pseudo_element.pseudo_element == PseudoElementType::Part => pseudo_element
                .identifier_identities
                .first()
                .map(|&part| smallvec::smallvec![DispatchKey::Part(part)]),
            _ => None,
        },
        SimpleSelector::Nesting(Nesting::Rule(parent)) => alternatives(*parent),
        _ => None,
    }
}

/// How selective a dispatch key is expected to be, lowest first.
pub(super) fn dispatch_selectivity(key: DispatchKey) -> u8 {
    match key {
        DispatchKey::Part(_) | DispatchKey::CustomState(_) | DispatchKey::Id(_) => 0,
        DispatchKey::Class(_) => 1,
        DispatchKey::AttributeName(_) => 2,
        DispatchKey::TagName(_) => 3,
        DispatchKey::Directionality(_) => 4,
        _ => 5,
    }
}

/// What the host and the arrival routing ask of a document's selectors: the attribute names they
/// test, those whose values they read as text, whether they test `:local-link`, and what an element
/// arriving has to carry to reach the anchor of one of their `:has()`.
#[derive(Clone, Default)]
pub(in crate::css::style) struct SelectorSummary {
    pub(super) tested_attribute_names: Vec<StyleAtomID>,
    pub(super) attribute_value_text_names: Vec<StyleAtomID>,
    pub(super) tests_local_link: bool,
    pub(super) relational_arrival_keys: Vec<DispatchKey>,
}

impl SelectorSummary {
    /// The summary of the selectors of `list`, inside the `@scope` levels `scope`.
    pub(super) fn of(list: SelectorListView<'_>, scope: Option<&super::bridge::BoundScopeChain>) -> Self {
        let mut summary = Self::default();
        for selector in list {
            summary.add_selector(selector, false);
        }
        for bound in scope
            .into_iter()
            .flat_map(|scope| scope.roots.iter().chain(&scope.limits))
        {
            let selector = bound.view();
            summary.add_selector(selector, false);
        }
        summary
    }

    fn add_selector(&mut self, selector: Selector<'_>, in_argument: bool) {
        for current in selector.compounds() {
            for simple in current.simple_selectors.iter() {
                self.add_simple(&simple, selector.namespaces(), in_argument);
            }
        }
    }

    fn add_list(&mut self, list: SelectorListView<'_>, in_argument: bool) {
        for selector in list {
            self.add_selector(selector, in_argument);
        }
    }

    fn arrival_key(&mut self, in_argument: bool, key: DispatchKey) {
        if in_argument && !self.relational_arrival_keys.contains(&key) {
            self.relational_arrival_keys.push(key);
        }
    }

    fn add_simple(&mut self, simple: &SimpleSelector<'_>, namespaces: SheetNamespaces<'_>, in_argument: bool) {
        let atoms = super::process_atoms();
        let forms = |name: StyleAtomID, lowercase: bool| -> SmallVec<[StyleAtomID; 2]> {
            match lowercase {
                true => smallvec::smallvec![name],
                false => smallvec::smallvec![name, atoms.ascii_lowercase(name)],
            }
        };
        match simple {
            SimpleSelector::Universal(_) => self.arrival_key(in_argument, DispatchKey::Structural),
            SimpleSelector::TagName(name) => {
                for form in forms(name.name, name.name_is_ascii_lowercase) {
                    self.arrival_key(in_argument, DispatchKey::TagName(form));
                }
            }
            SimpleSelector::Id(name) => {
                for form in forms(name.name, name.is_ascii_lowercase) {
                    self.arrival_key(in_argument, DispatchKey::Id(form));
                }
            }
            SimpleSelector::Class(name) => {
                for form in forms(name.name, name.is_ascii_lowercase) {
                    self.arrival_key(in_argument, DispatchKey::Class(form));
                }
            }
            SimpleSelector::Attribute(attribute) => {
                // `[*|x]` tests the name qualified by no namespace, `[ns|x]` the one qualified by the
                // namespace the prefix names, and `[x]` the bare local name.
                let qualifier = match attribute.qualified_name.namespace_type {
                    crate::css::selector::NamespaceType::Any => Some(StyleAtomID::NONE),
                    crate::css::selector::NamespaceType::Named => {
                        match namespaces.namespace_of(attribute.qualified_name.namespace) {
                            Some(namespace) => Some(namespace),
                            None => return,
                        }
                    }
                    _ => None,
                };
                // NB: The selector's arena leases the qualified names too, which keeps them live
                //     while the rule is.
                let names: SmallVec<[StyleAtomID; 2]> = forms(
                    attribute.qualified_name.name,
                    attribute.qualified_name.name_is_ascii_lowercase,
                )
                .into_iter()
                .map(|name| match qualifier {
                    Some(namespace) => super::AtomLease::acquire_qualified(atoms, namespace, name).atom(),
                    None => name,
                })
                .collect();
                let reads_text = attribute.match_type != crate::css::selector::AttributeMatchType::HasAttribute
                    && (attribute.match_type != crate::css::selector::AttributeMatchType::ExactValue
                        || attribute.case_type == crate::css::selector::AttributeCaseType::Insensitive
                        || attribute.names_a_legacy_case_insensitive_attribute);
                for &name in &names {
                    if !self.tested_attribute_names.contains(&name) {
                        self.tested_attribute_names.push(name);
                    }
                    if reads_text && !self.attribute_value_text_names.contains(&name) {
                        self.attribute_value_text_names.push(name);
                    }
                    self.arrival_key(in_argument, DispatchKey::AttributeName(name));
                }
            }
            SimpleSelector::PseudoClass(pseudo_class) => {
                let list = pseudo_class.argument_selector_list;
                match pseudo_class.pseudo_class {
                    PseudoClassType::Has => self.add_list(list, true),
                    PseudoClassType::State if !pseudo_class.identifier_identity.is_none() => {
                        self.arrival_key(in_argument, DispatchKey::CustomState(pseudo_class.identifier_identity));
                    }
                    PseudoClassType::Lang => self.arrival_key(in_argument, DispatchKey::Language),
                    // NB: Which direction `:dir()` names is a fact of the tree's shape as an arrival
                    //     sees it.
                    PseudoClassType::Root
                    | PseudoClassType::Empty
                    | PseudoClassType::Scope
                    | PseudoClassType::Heading
                    | PseudoClassType::Dir => self.arrival_key(in_argument, DispatchKey::Structural),
                    kind => {
                        if let Some(fact) = super::compiler::state_fact_for(kind) {
                            self.tests_local_link |= fact == super::input_types::StateFact::LocalLink;
                            self.arrival_key(in_argument, DispatchKey::State(fact));
                        }
                        if reads_position(pseudo_class) {
                            self.arrival_key(in_argument, DispatchKey::Structural);
                        }
                        self.add_list(list, in_argument);
                    }
                }
            }
            SimpleSelector::PseudoElement(pseudo_element) => match pseudo_element.value {
                PseudoElementValue::CompoundSelector(argument) => self.add_selector(argument, in_argument),
                _ if pseudo_element.pseudo_element == PseudoElementType::Part => {
                    for &part in pseudo_element.identifier_identities {
                        self.arrival_key(in_argument, DispatchKey::Part(part));
                    }
                }
                _ => {}
            },
            SimpleSelector::Nesting(Nesting::Rule(parent)) => self.add_list(*parent, in_argument),
            SimpleSelector::Nesting(_) => self.arrival_key(in_argument, DispatchKey::Structural),
            SimpleSelector::Invalid(_) => {}
        }
    }
}

/// Whether a pseudo-class reads where the element stands among its siblings.
fn reads_position(pseudo_class: &crate::css::selector::PseudoClassSelector<'_>) -> bool {
    matches!(
        pseudo_class.pseudo_class,
        PseudoClassType::FirstChild
            | PseudoClassType::LastChild
            | PseudoClassType::OnlyChild
            | PseudoClassType::FirstOfType
            | PseudoClassType::LastOfType
            | PseudoClassType::OnlyOfType
            | PseudoClassType::NthChild
            | PseudoClassType::NthLastChild
            | PseudoClassType::NthOfType
            | PseudoClassType::NthLastOfType
    )
}
