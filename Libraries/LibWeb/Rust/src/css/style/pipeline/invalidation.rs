/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which elements a change to one element's facts restyles. A selector is a chain of compounds
//! joined by combinators, evaluated from its subject; for every fact a compound of the rule index
//! tests, the map holds where that compound starts among the selector's components, which say the
//! combinator right of it and the compound it leads to. An element whose fact changed restyles
//! itself where the fact is the subject's, and otherwise the elements the combinator reaches from
//! it that match the next compound on their own, onwards along the chain to the subject.
//!
//! A `:has()` compound is tested on its anchor, whose answer a change to an element of the
//! argument can flip: the map holds, for every fact an argument tests, the queries it reads, and
//! how the anchors of an element stand to it, which the anchors' own compounds are tested on.

use super::super::compiler::state_fact_for;
use super::super::component_matching::{
    CompoundTarget, EntrySelector, backed_pseudo_element_kind, compound_target, mentions, relative_axis,
};
use super::super::index::dispatch_bloom_bit;
use super::super::selector_analysis::dispatch_selectivity;
use super::super::selector_evaluation::ElementFeatures;
use super::super::*;
use super::extent_map::SharedExtentMap;
use super::projection::{CarriedKey, ProjectionRow, WatchedKeys};
use super::rules::{RuleIndex, SheetSegment, is_probed, proves_the_whole_selector};
use super::shared_prefix::SharedPrefixVec;
use crate::css::selector::{
    Combinator, CompoundRef, PseudoClassType, PseudoElementSelector, PseudoElementType, PseudoElementValue, Selector,
    SelectorListView, SimpleSelector,
};
use crate::css::selector_arena::{ComponentOffset, Nesting};
use smallvec::SmallVec;
use std::sync::Arc;

/// A fact of one element that selectors test.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) enum Trigger {
    Class(StyleAtomID),
    Id(StyleAtomID),
    Attribute(StyleAtomID),
    State(StateFact),
    CustomState(StyleAtomID),
    Directionality,
    Language,
    Heading,
    Emptiness,
    /// A name the element is exposed to `::part()` rules under.
    Part(StyleAtomID),
    /// A tag name, which only an element arriving or leaving changes.
    Tag(StyleAtomID),
    /// Where the element stands among its siblings, or among those of its type, counted from the
    /// end or from the start, which an element arriving or leaving beside it changes, for the
    /// compounds requiring the key.
    Position {
        from_end: bool,
        of_type: bool,
        key: PositionKey,
    },
    /// What every element carries, for the compounds testing no other fact, which only an element
    /// arriving or leaving, or becoming another's previous sibling, changes anything for.
    Any,
    /// The answer of the `:has()` of the map's relative queries at the index, on its anchor.
    Relative(u32),
}

/// What every element a compound reading its position matches carries: a class, an id, a tag
/// name, or nothing in particular.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) enum PositionKey {
    Any,
    Class(StyleAtomID),
    Id(StyleAtomID),
    Tag(StyleAtomID),
}

/// How the elements a combinator reaches stand to the element left of it.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) enum ReachCombinator {
    /// `>`: its children.
    Child,
    /// ` `: its descendants.
    Descendant,
    /// `+`: its next sibling.
    NextSibling,
    /// `~`: its later siblings.
    LaterSibling,
    /// `:host > `: the children of its shadow root.
    ShadowChild,
    /// `:host `: the elements of its shadow tree.
    ShadowDescendant,
    /// `slot::slotted()`: the elements assigned to it, through the slots assigned to it.
    AssignedElements,
    /// `host::part()`: the elements of its shadow trees exposed to it as parts.
    PartsOfHost,
    /// `host::file-selector-button:hover`: the shadow host of the element backing its
    /// pseudo-element.
    HostOfBackingElement,
    /// The end of a chain that reaches no one compound: the element left of it and its descendants,
    /// as an `@scope` bound does the elements it holds.
    Subtree,
    /// The end of a chain that reaches no one compound: the siblings of the element left of it and
    /// their descendants, as an `of <selector>` does what it counts.
    ParentSubtree,
}

/// A compound of the selector of a rule index entry, or of a selector nested in it, that tests a
/// fact: where it starts in the arena of the entry's selector. A change to the fact restyles the
/// element itself where the compound is the subject's, and otherwise what the combinator right of
/// the compound reaches, as the components say.
///
/// NB: A dependency names its compound by the map's number for it, under which the map holds the
///     selector and where the compound starts in it, and a nested selector steps out to the
///     compound holding its list.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(super) struct Dependency {
    pub(super) entry: u32,
    pub(super) compound: CompoundID,
}

/// A compound the map holds, numbered in the part holding it, the part tagged in its high bits as
/// a query's is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(super) struct CompoundID(u32);

/// A compound of an entry's selector as the map holds it, worked out once as the map takes it:
/// where it starts in the selector, which its dependency's entry names, the combinator left of it
/// with the compound beyond, and the compound right of it, so that following a change reads neither
/// the entry nor the selector's components to find them.
#[derive(Clone)]
struct HeldCompound {
    /// The selector of the compound's entry, which following a change reads in one load, its arena holding the
    /// compound.
    selector: crate::css::selector::SelectorRef,
    /// Where the compound starts, in the arena of the entry's selector.
    offset: ComponentOffset,
    /// The compound right of this one, by its number in the part, or `NO_COMPOUND` for the
    /// subject's.
    next: u32,
    /// The compound left of the combinator left of this one, or `NO_COMPOUND`.
    left: u32,
    /// A key every element matching the compound carries, where it has one.
    required_key: Option<DispatchKey>,
    /// The number the part gives the compound's own tests, alike for every compound of the part
    /// testing the same facts alike, as [`Selector::own_tests`] says, under which answers of them
    /// are shared, or `NO_SHAPE`.
    shape: u32,
    /// The number the part gives the compound with what is left of it, alike for every compound
    /// of the part whose own tests and combinator are alike and whose left compound's number is
    /// the same, under which answers from it leftwards are shared, or `NO_SHAPE`.
    chain: u32,
    /// Where the names the compound tests start in the part's `operand_names`, where it tests
    /// nothing else and they are more than one, or the one it tests, which an element's facts answer
    /// for without the compound being read.
    operand_names: u32,
    /// How many names the compound tests, or zero where it tests something else. A lone name is
    /// held in place of the place of the names, with what kind of name it is in `flags`.
    operand_name_count: u8,
    /// The combinator left of the compound, a descendant combinator's where it has none.
    combinator: ReachCombinator,
    /// The `HeldCompound::REACHES_EVERY_SUBJECT` and the other flags.
    flags: u8,
}

impl HeldCompound {
    /// Every element matches the compound, which is the subject's.
    const REACHES_EVERY_SUBJECT: u8 = 1 << 0;
    /// The compound evaluates a `:has()`, or a compound left of it does.
    const READS_RELATIVE: u8 = 1 << 1;
    /// The entry's rule is inside `@scope`, whose levels its selector is matched with.
    const SCOPED: u8 = 1 << 2;
    /// The lone name the compound tests is a class's.
    const LONE_CLASS: u8 = 1 << 3;
    /// The lone name the compound tests is an id's. It is a type's where this and the above are not set.
    const LONE_ID: u8 = 1 << 4;
    /// The compound is of a bound of the entry's `@scope` levels, which can be in another arena
    /// than the entry's selector.
    const IN_BOUND: u8 = 1 << 5;
    const NO_SHAPE: u32 = u32::MAX;
    /// The chain of a compound the entry being collected took, which is numbered once the entry is.
    const CHAIN_PENDING: u32 = u32::MAX - 1;

    fn has(&self, flag: u8) -> bool {
        self.flags & flag != 0
    }
}

const _: () = assert!(size_of::<HeldCompound>() == 56);

const NO_COMPOUND: u32 = u32::MAX;

/// The names a compound tests, where it tests nothing else.
pub(super) enum OperandNames<'a> {
    None,
    One(OperandName),
    Many(&'a [OperandName]),
}

impl OperandNames<'_> {
    pub(super) fn is_empty(&self) -> bool {
        matches!(self, Self::None)
    }

    /// Whether an element with `facts` carries every name.
    pub(super) fn carried_by(&self, facts: &impl ElementFeatures) -> bool {
        match self {
            Self::None => true,
            Self::One(name) => name.carried_by(facts),
            Self::Many(names) => names.iter().all(|name| name.carried_by(facts)),
        }
    }
}

/// One name a compound tests, written in ASCII lowercase, which every document carries as written.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) enum OperandName {
    Class(StyleAtomID),
    Id(StyleAtomID),
    /// A type selector in any namespace, of a sheet declaring no default namespace.
    Tag(StyleAtomID),
}

impl OperandName {
    /// The names `compound` tests, where it tests nothing else and they are few enough to hold.
    fn all_of(compound: &CompoundRef<'_>, has_default_namespace: bool) -> Option<SmallVec<[Self; 4]>> {
        compound
            .simple_selectors
            .iter()
            .map(|simple| match simple {
                SimpleSelector::Class(name) if name.is_ascii_lowercase => Some(Self::Class(name.name)),
                SimpleSelector::Id(name) if name.is_ascii_lowercase => Some(Self::Id(name.name)),
                SimpleSelector::TagName(name)
                    if name.name_is_ascii_lowercase
                        && matches!(
                            name.namespace_type,
                            crate::css::selector::NamespaceType::Default | crate::css::selector::NamespaceType::Any
                        )
                        && !has_default_namespace =>
                {
                    Some(Self::Tag(name.name))
                }
                _ => None,
            })
            .collect::<Option<SmallVec<[Self; 4]>>>()
            .filter(|names| !names.is_empty() && names.len() <= usize::from(u8::MAX))
    }

    /// Whether an element with `facts` carries the name.
    pub(super) fn carried_by(self, facts: &impl ElementFeatures) -> bool {
        match self {
            Self::Class(class) => facts.has_class(class),
            Self::Id(id) => facts.has_id(id),
            Self::Tag(tag) => facts.local_name_is(tag),
        }
    }
}

const _: () = assert!(size_of::<Dependency>() == 8);

/// A dependency as the map holds it, with the keys some ancestor of an element matching its compound,
/// with what is left of it, carries, which the map works out once as it takes the dependency, so
/// that following a change rejects it on the ancestor filter without reading the selector.
#[derive(Clone, Copy)]
pub(super) struct HeldDependency {
    pub(super) dependency: Dependency,
    pub(super) ancestor_hashes: super::bloom::AncestorHashes,
}

const _: () = assert!(size_of::<HeldDependency>() == 20);

/// The combinator left of a compound of an entry's selector, as the components say, which a walk
/// follows from an element matching the compound left of it to test what it reaches against the
/// compound.
#[derive(Clone, Copy)]
pub(super) struct Step {
    pub(super) entry: u32,
    pub(super) combinator: ReachCombinator,
    /// The step of the compound right of this one, or none where this one is the subject's.
    pub(super) next: Option<Dependency>,
    /// Whether every element matches the compound, which is the subject's.
    pub(super) reaches_every_subject: bool,
    /// A key every element matching the compound carries, where it has one.
    pub(super) required_key: Option<DispatchKey>,
}

impl InvalidationMap {
    /// Whether an element with `facts` carries the classes, id and tag name this compound requires
    /// of it, and stands for the pseudo-element it names, in itself or in any alternative of its
    /// `:is()`, `:where()` and `&`, whatever its states and other facts.
    pub(super) fn may_match_by_keys(
        &self,
        index: &RuleIndex,
        dependency: Dependency,
        facts: ProjectionRow<'_>,
    ) -> bool {
        // NB: The name the compound is held under answers for most elements without its components being read.
        let carried = |key: Option<DispatchKey>| match key {
            Some(DispatchKey::Class(class)) => facts.classes().contains(&class),
            Some(DispatchKey::Id(id)) => facts.id() == id,
            Some(DispatchKey::TagName(tag)) => facts.tag() == tag || facts.folded_tag() == tag,
            _ => true,
        };
        carried(self.held(dependency).0.required_key) && compound_carried(self.resolve(dependency, index).1, facts)
    }
}

/// See [`InvalidationMap::may_match_by_keys`].
fn compound_carried(selector: Selector<'_>, facts: ProjectionRow<'_>) -> bool {
    fn simple_carried(simple: &SimpleSelector<'_>, facts: ProjectionRow<'_>) -> bool {
        let atoms = process_atoms();
        match simple {
            SimpleSelector::Class(name) => {
                facts.classes().contains(&name.name) || facts.classes().contains(&atoms.ascii_lowercase(name.name))
            }
            SimpleSelector::Id(name) => facts.id() == name.name || facts.id() == atoms.ascii_lowercase(name.name),
            // NB: An element carries a folded tag only where its tag is not its own lowercase
            //     form, and a sheet's name matches a tag in any case in an HTML document.
            SimpleSelector::TagName(name) => {
                let lowercase = atoms.ascii_lowercase(name.name);
                facts.tag() == name.name || facts.tag() == lowercase || facts.folded_tag() == lowercase
            }
            SimpleSelector::PseudoClass(pseudo_class)
                if matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where) =>
            {
                pseudo_class
                    .argument_selector_list
                    .iter()
                    .any(|selector| compound_carried(selector, facts))
            }
            SimpleSelector::Nesting(Nesting::Rule(parent)) => {
                parent.iter().any(|selector| compound_carried(selector, facts))
            }
            _ => true,
        }
    }
    let compound = selector.first_compound();
    if let Some(SimpleSelector::PseudoElement(pseudo_element)) = compound.simple_selectors.first() {
        let kind = pseudo_element.pseudo_element as u16;
        let backed_kinds = u16::from(super::super::bridge::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND)
            ..=u16::from(super::super::bridge::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND);
        // NB: An element-backed pseudo-element is an element of its own, whichever element its compound is tested on.
        if backed_kinds.contains(&kind) {
            return facts.backing_pseudo_kind().map(u16::from) == Some(kind)
                && compound
                    .simple_selectors
                    .after_first()
                    .iter()
                    .all(|simple| simple_carried(&simple, facts));
        }
        // NB: What a pseudo-element without an element of its own tests is tested on the element it originates from.
        if is_transparent(&compound) {
            return match selector.left() {
                Some((Combinator::PseudoElement, left)) => compound_carried(left, facts),
                _ => true,
            };
        }
    }
    compound
        .simple_selectors
        .iter()
        .all(|simple| simple_carried(&simple, facts))
}

/// Keys of which every element [`compound_carried`] holds for carries at least one, or none where any element may be
/// such: the keys of one test of the compound, the most selective one, or of the alternatives of its `:is()`, `:where()`
/// or `&`, each in every form an element carries it. A change to one of the two is a change to the other.
fn carried_keys(selector: Selector<'_>) -> Option<SmallVec<[CarriedKey; 4]>> {
    fn of_any<'a>(selectors: impl Iterator<Item = Selector<'a>>) -> Option<SmallVec<[CarriedKey; 4]>> {
        let mut keys = SmallVec::new();
        for selector in selectors {
            keys.extend(carried_keys(selector)?);
        }
        Some(keys)
    }
    fn in_every_form(make: fn(StyleAtomID) -> CarriedKey, name: StyleAtomID) -> SmallVec<[CarriedKey; 4]> {
        let folded = process_atoms().ascii_lowercase(name);
        match folded == name {
            true => smallvec::smallvec![make(name)],
            false => smallvec::smallvec![make(name), make(folded)],
        }
    }
    fn of_simple(simple: &SimpleSelector<'_>) -> Option<SmallVec<[CarriedKey; 4]>> {
        match simple {
            SimpleSelector::Class(name) => Some(in_every_form(CarriedKey::Class, name.name)),
            SimpleSelector::Id(name) => Some(in_every_form(CarriedKey::Id, name.name)),
            SimpleSelector::TagName(name) => Some(in_every_form(CarriedKey::Tag, name.name)),
            SimpleSelector::PseudoClass(pseudo_class)
                if matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where) =>
            {
                of_any(pseudo_class.argument_selector_list.iter())
            }
            SimpleSelector::Nesting(Nesting::Rule(parent)) => of_any(parent.iter()),
            _ => None,
        }
    }
    let selectivity = |keys: &SmallVec<[CarriedKey; 4]>| {
        let tags = keys.iter().any(|key| matches!(key, CarriedKey::Tag(_)));
        (tags, keys.len())
    };
    let compound = selector.first_compound();
    if let Some(SimpleSelector::PseudoElement(pseudo_element)) = compound.simple_selectors.first() {
        let kind = pseudo_element.pseudo_element as u16;
        let backed_kinds = u16::from(super::super::bridge::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND)
            ..=u16::from(super::super::bridge::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND);
        if backed_kinds.contains(&kind) {
            return Some(smallvec::smallvec![CarriedKey::BackedPseudoElement(
                u8::try_from(kind).expect("a backed pseudo-element kind fits a u8")
            )]);
        }
        if is_transparent(&compound) {
            return match selector.left() {
                Some((Combinator::PseudoElement, left)) => carried_keys(left),
                _ => None,
            };
        }
    }
    compound
        .simple_selectors
        .iter()
        .filter_map(|simple| of_simple(&simple))
        .min_by_key(selectivity)
}

/// The compounds of a map testing `:hover`, by the keys of the elements that may match them, compiled once for the map
/// so that an element is tested by looking up its own keys rather than by reading every compound.
pub(super) struct HoverCompounds {
    /// Whether any element may match a compound no key selects, or an element's hover flips a query.
    every_element: bool,
    by_key: HashMap<CarriedKey, SmallVec<[Dependency; 2]>>,
    keys: Arc<WatchedKeys>,
    /// What tells the compounds from others, which a map testing the same ones says too.
    identities: Vec<(crate::css::selector_arena::SelectorArenaSerial, ComponentOffset)>,
}

impl HoverCompounds {
    #[must_use]
    pub(super) fn every_element(&self) -> bool {
        self.every_element
    }

    #[must_use]
    pub(super) fn keys(&self) -> &Arc<WatchedKeys> {
        &self.keys
    }

    /// Whether the two hold the same compounds, whichever maps they were compiled from.
    #[must_use]
    pub(super) fn are_the_same_as(&self, other: &Self) -> bool {
        self.every_element == other.every_element && self.identities == other.identities
    }

    /// Whether the element `facts` are of may match one of the compounds, by the same test as
    /// [`InvalidationMap::may_match_by_keys`] for each, of the map and index they were compiled from.
    #[must_use]
    pub(super) fn may_match(&self, map: &InvalidationMap, index: &RuleIndex, facts: ProjectionRow<'_>) -> bool {
        facts.carried_keys().any(|key| {
            self.by_key.get(&key).is_some_and(|compounds| {
                compounds
                    .iter()
                    .any(|&compound| map.may_match_by_keys(index, compound, facts))
            })
        })
    }
}

impl InvalidationMap {
    /// The compounds of the map testing `:hover`, compiled.
    pub(super) fn hover_compounds(&self, index: &RuleIndex) -> HoverCompounds {
        let hover = Trigger::State(StateFact::Hover);
        let mut every_element = self.queries_testing(&hover).next().is_some();
        let mut by_key: HashMap<CarriedKey, SmallVec<[Dependency; 2]>> = HashMap::default();
        let mut identities = Vec::new();
        for compound in self.effects(&hover) {
            let selector = self.resolve(compound, index).1;
            identities.push(selector.identity());
            match carried_keys(selector) {
                Some(keys) => {
                    for key in keys {
                        by_key.entry(key).or_default().push(compound);
                    }
                }
                None => every_element = true,
            }
        }
        identities.sort_unstable();
        identities.dedup();
        if every_element {
            by_key.clear();
        }
        let keys = Arc::new(WatchedKeys::new(by_key.keys().copied()));
        HoverCompounds {
            every_element,
            by_key,
            keys,
            identities,
        }
    }
}

/// Whether `compound` evaluates a `:has()`, or a compound left of it does.
fn reads_relative(compound: Selector<'_>) -> bool {
    compound.compounds().any(|compound| {
        compound
            .simple_selectors
            .iter()
            .any(|simple| super::super::component_matching::mentions_relative(&simple))
    })
}

/// The compound right of `compound` in `root`, the selector from its subject or from a compound
/// where a chain ends leftwards, where a walk that reached an element matching `compound` goes on:
/// the one right of it in the selector it is of, or right of the compound holding the list a nested
/// selector is in, past any pseudo-element no element stands for. None for `root` itself.
pub(super) fn right_of<'a>(root: Selector<'a>, compound: Selector<'a>) -> Option<Selector<'a>> {
    let mut current = compound;
    loop {
        if current == root {
            return None;
        }
        let Some(right) = current.right() else {
            current = root.compound_owning(current)?;
            continue;
        };
        if is_transparent(&right.first_compound()) {
            current = right;
            continue;
        }
        return Some(right);
    }
}

/// Whether `compound` is a pseudo-element no element stands for, whose originating element is the
/// one the compound left of it describes.
fn is_transparent(compound: &CompoundRef<'_>) -> bool {
    matches!(compound_target(compound), CompoundTarget::Transparent)
}

/// The combinator left of `compound`, with the compound left of it, as invalidation follows it, or
/// none for the leftmost compound of its selector and for a compound only some alternatives of
/// which name the shadow host.
pub(super) fn combinator_left_of(compound: Selector<'_>) -> Option<(ReachCombinator, Selector<'_>)> {
    let (relation, left) = compound.left()?;
    let combinator = match relation {
        Combinator::ImmediateChild => match host_step(left)? {
            true => ReachCombinator::ShadowChild,
            false => ReachCombinator::Child,
        },
        Combinator::Descendant => match host_step(left)? {
            true => ReachCombinator::ShadowDescendant,
            false => ReachCombinator::Descendant,
        },
        Combinator::NextSibling => ReachCombinator::NextSibling,
        Combinator::SubsequentSibling => ReachCombinator::LaterSibling,
        Combinator::PseudoElement => {
            let first = compound.first_compound();
            match first.simple_selectors.first() {
                Some(SimpleSelector::PseudoElement(pseudo_element))
                    if pseudo_element.pseudo_element == PseudoElementType::Slotted =>
                {
                    ReachCombinator::AssignedElements
                }
                Some(SimpleSelector::PseudoElement(pseudo_element))
                    if pseudo_element.pseudo_element == PseudoElementType::Part =>
                {
                    ReachCombinator::PartsOfHost
                }
                _ if backed_pseudo_element_kind(&first).is_some() => ReachCombinator::HostOfBackingElement,
                _ => return None,
            }
        }
        Combinator::Column | Combinator::None => return None,
    };
    Some((combinator, left))
}

/// Whether `compound` names the host of the shadow tree its rule is in, which a combinator reaches
/// out of the tree to, or none where only some of its alternatives do.
fn host_step(compound: Selector<'_>) -> Option<bool> {
    let first = compound.first_compound();
    let is_host = |simple: &SimpleSelector<'_>| matches!(simple, SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Host);
    if first.simple_selectors.iter().any(|simple| is_host(&simple)) {
        return Some(true);
    }
    match first.simple_selectors.iter().any(|simple| mentions(&simple, &is_host)) {
        true => None,
        false => Some(false),
    }
}

/// What every element matching `compound` carries that a combinator's walk filters on: a class,
/// an id, an attribute name, a tag name or a state, the most selective one.
pub(super) fn required_key(compound: &CompoundRef<'_>) -> Option<DispatchKey> {
    compound
        .simple_selectors
        .iter()
        .filter_map(|simple| match simple {
            // NB: A name not written in ASCII lowercase is carried folded in a quirks-mode document.
            SimpleSelector::Id(name) if name.is_ascii_lowercase => Some(DispatchKey::Id(name.name)),
            SimpleSelector::Class(name) if name.is_ascii_lowercase => Some(DispatchKey::Class(name.name)),
            // NB: An attribute in a namespace is carried under its qualified forms, not its local name.
            SimpleSelector::Attribute(attribute)
                if matches!(
                    attribute.qualified_name.namespace_type,
                    crate::css::selector::NamespaceType::Default | crate::css::selector::NamespaceType::None
                ) =>
            {
                Some(DispatchKey::AttributeName(
                    process_atoms().ascii_lowercase(attribute.qualified_name.name),
                ))
            }
            SimpleSelector::TagName(name) if name.name_is_ascii_lowercase => Some(DispatchKey::TagName(name.name)),
            // NB: Only an argument list of one compound requires what that compound does: Of a list of several, an
            //     element matches whichever alternative it carries, and a compound left of a combinator is an
            //     ancestor's or a sibling's.
            SimpleSelector::PseudoClass(pseudo_class)
                if matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where) =>
            {
                sole_compound(pseudo_class.argument_selector_list).and_then(|compound| required_key(&compound))
            }
            SimpleSelector::PseudoClass(pseudo_class) => {
                state_fact_for(pseudo_class.pseudo_class).map(DispatchKey::State)
            }
            _ => None,
        })
        .min_by_key(|&key| dispatch_selectivity(key))
}

/// The one compound `list` consists of, where it holds a single selector with no combinator.
fn sole_compound(list: SelectorListView<'_>) -> Option<CompoundRef<'_>> {
    let mut selectors = list.iter();
    let selector = selectors.next()?;
    (selectors.next().is_none() && selector.left().is_none()).then(|| selector.first_compound())
}

/// Whether every element matches `compound`, as far as its own facts go.
fn matches_every_element(compound: &CompoundRef<'_>) -> bool {
    compound
        .simple_selectors
        .iter()
        .all(|simple| matches!(simple, SimpleSelector::Universal(_)))
}

/// The triggers a simple selector names on its own, of the element it is of: a class, an id, an
/// attribute name, a tag name, a state or another fact, in each form an element carries it.
fn own_triggers(simple: &SimpleSelector<'_>, names: &mut AcquiredNames) -> SmallVec<[Trigger; 2]> {
    let atoms = process_atoms();
    let both = |make: fn(StyleAtomID) -> Trigger, name: StyleAtomID, lowercase: bool| -> SmallVec<[Trigger; 2]> {
        let folded = match lowercase {
            true => name,
            false => atoms.ascii_lowercase(name),
        };
        match folded == name {
            true => smallvec::smallvec![make(name)],
            false => smallvec::smallvec![make(name), make(folded)],
        }
    };
    match simple {
        SimpleSelector::Class(name) => both(Trigger::Class, name.name, name.is_ascii_lowercase),
        SimpleSelector::Id(name) => both(Trigger::Id, name.name, name.is_ascii_lowercase),
        SimpleSelector::TagName(name) => both(Trigger::Tag, name.name, name.name_is_ascii_lowercase),
        // NB: `[*|x]` and `[ns|x]` test names qualified by a namespace, and every attribute in a
        //     namespace publishes its name qualified by none as well, which a change to it names.
        SimpleSelector::Attribute(attribute)
            if matches!(
                attribute.qualified_name.namespace_type,
                crate::css::selector::NamespaceType::Any | crate::css::selector::NamespaceType::Named
            ) =>
        {
            both(
                Trigger::Attribute,
                attribute.qualified_name.name,
                attribute.qualified_name.name_is_ascii_lowercase,
            )
            .into_iter()
            .map(|trigger| match trigger {
                Trigger::Attribute(name) => Trigger::Attribute(names.qualified_by_no_namespace(name)),
                trigger => trigger,
            })
            .collect()
        }
        SimpleSelector::Attribute(attribute) => both(
            Trigger::Attribute,
            attribute.qualified_name.name,
            attribute.qualified_name.name_is_ascii_lowercase,
        ),
        SimpleSelector::PseudoClass(pseudo_class) => {
            if let Some(fact) = state_fact_for(pseudo_class.pseudo_class) {
                return smallvec::smallvec![Trigger::State(fact)];
            }
            match pseudo_class.pseudo_class {
                PseudoClassType::Dir => smallvec::smallvec![Trigger::Directionality],
                PseudoClassType::Lang => smallvec::smallvec![Trigger::Language],
                PseudoClassType::Heading => smallvec::smallvec![Trigger::Heading],
                PseudoClassType::Empty => smallvec::smallvec![Trigger::Emptiness],
                PseudoClassType::State if !pseudo_class.identifier_identity.is_none() => {
                    smallvec::smallvec![Trigger::CustomState(pseudo_class.identifier_identity)]
                }
                _ => SmallVec::new(),
            }
        }
        SimpleSelector::PseudoElement(pseudo_element) if pseudo_element.pseudo_element == PseudoElementType::Part => {
            pseudo_element
                .identifier_identities
                .iter()
                .map(|&part| Trigger::Part(part))
                .collect()
        }
        _ => SmallVec::new(),
    }
}

/// The positions a pseudo-class reads, as `(from_end, of_type)`, with the selectors an `of` counts.
fn positions_read<'a>(
    pseudo_class: &crate::css::selector::PseudoClassSelector<'a>,
) -> (SmallVec<[(bool, bool); 2]>, Option<SelectorListView<'a>>) {
    let of = (!pseudo_class.argument_selector_list.is_empty()).then_some(pseudo_class.argument_selector_list);
    let positions: SmallVec<[(bool, bool); 2]> = match pseudo_class.pseudo_class {
        PseudoClassType::FirstChild => smallvec::smallvec![(false, false)],
        PseudoClassType::LastChild => smallvec::smallvec![(true, false)],
        PseudoClassType::OnlyChild => smallvec::smallvec![(false, false), (true, false)],
        PseudoClassType::FirstOfType => smallvec::smallvec![(false, true)],
        PseudoClassType::LastOfType => smallvec::smallvec![(true, true)],
        PseudoClassType::OnlyOfType => smallvec::smallvec![(false, true), (true, true)],
        PseudoClassType::NthChild => return (smallvec::smallvec![(false, false)], of),
        PseudoClassType::NthLastChild => return (smallvec::smallvec![(true, false)], of),
        PseudoClassType::NthOfType => smallvec::smallvec![(false, true)],
        PseudoClassType::NthLastOfType => smallvec::smallvec![(true, true)],
        _ => SmallVec::new(),
    };
    (positions, None)
}

/// The qualified names of the attributes a map keys triggers on, which it leases so that their
/// atoms name no other name while it holds them.
#[derive(Clone, Default)]
pub(super) struct NameLeases {
    atoms: Vec<StyleAtomID>,
    leases: Option<super::super::AtomLeases>,
}

impl NameLeases {
    /// Lease what a collection acquired.
    fn settle(&mut self, acquired: AcquiredNames) {
        if acquired.leases.is_empty() {
            return;
        }
        self.atoms.extend(acquired.atoms);
        self.atoms.sort_unstable_by_key(|atom| atom.0);
        self.atoms.dedup();
        self.atoms.shrink_to_fit();
        self.leases = Some(process_atoms().lease(self.atoms.iter().copied()));
    }
}

/// What a collection acquired of qualified names, until the map leases it.
#[derive(Default)]
struct AcquiredNames {
    atoms: Vec<StyleAtomID>,
    leases: Vec<super::super::AtomLease>,
}

impl AcquiredNames {
    /// The atom of `name` qualified by no namespace.
    fn qualified_by_no_namespace(&mut self, name: StyleAtomID) -> StyleAtomID {
        let lease = super::super::AtomLease::acquire_qualified(process_atoms(), StyleAtomID::NONE, name);
        let atom = lease.atom();
        self.atoms.push(atom);
        self.leases.push(lease);
        atom
    }
}

/// What collecting a compound's simple selectors adds its dependencies with.
#[derive(Clone, Copy)]
struct Collecting {
    dependency: Dependency,
    /// The keys some ancestor of an element matching the compound, with what is left of it, carries.
    ancestor_hashes: super::bloom::AncestorHashes,
    /// Whether the combinator right of the compound is a sibling combinator's.
    follows_siblings: bool,
    /// What every element matching the compound carries, for the positions it reads.
    position_key: PositionKey,
}

/// What a change to a fact can restyle.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum TriggerReach {
    /// Nothing tests the fact.
    None,
    /// Only `:has()` arguments test the fact, whose anchors a change reaches.
    Indirect,
    /// A compound tests the fact.
    Direct,
}

/// Which positions compounds read: among all siblings or those of the element's type, counted from
/// the start or from the end.
#[derive(Clone, Copy, Default)]
pub(super) struct ReadPositions(u8);

impl ReadPositions {
    const COUNTS_BY_SELECTOR: u8 = 1 << 4;

    fn bit(from_end: bool, of_type: bool) -> u8 {
        1 << (u8::from(of_type) * 2 + u8::from(from_end))
    }

    fn note(&mut self, from_end: bool, of_type: bool) {
        self.0 |= Self::bit(from_end, of_type);
    }

    fn add(&mut self, other: Self) {
        self.0 |= other.0;
    }

    pub(super) fn reads(self, from_end: bool, of_type: bool) -> bool {
        self.0 & Self::bit(from_end, of_type) != 0
    }

    /// Note that an `of <selector>` counts only the siblings it matches.
    fn note_counting_by_selector(&mut self) {
        self.0 |= Self::COUNTS_BY_SELECTOR;
    }

    /// Whether an `of <selector>` counts only the siblings it matches, which change without an element's place moving.
    pub(super) fn counts_by_selector(self) -> bool {
        self.0 & Self::COUNTS_BY_SELECTOR != 0
    }

    pub(super) fn any(self) -> bool {
        self.0 != 0
    }
}

/// What a chain of a selector leads on to where that is no compound of its rule's.
#[derive(Clone, Copy)]
enum ChainEnd {
    /// The element and its descendants.
    Subtree,
    /// The element's siblings and their descendants.
    ParentSubtree,
}

/// The bound at `place` among those of the `@scope` levels of `entry`.
fn bound_of(entry: EntrySelector<'_>, place: u16) -> Selector<'_> {
    let scope = entry.scope().expect("the entry of a bound is inside `@scope`");
    let place = usize::from(place);
    match scope.roots.get(place) {
        Some(root) => root.view(),
        None => scope.limits[place - scope.roots.len()].view(),
    }
}

/// The map of the selectors of one part of a document's rule index, numbering their entries and
/// queries from zero: the user agent's with the document's own after them, or those of a sheet
/// segment, which every document binding it shares.
#[derive(Clone, Default)]
pub(super) struct InvalidationPart {
    /// The compounds testing each fact of an element, whose match a change to it can flip.
    pub(super) by_trigger: SharedExtentMap<Trigger, HeldDependency>,
    /// The dependencies of each fact whose combinator right of them is a sibling combinator's,
    /// again, which an element arriving or leaving follows.
    pub(super) sibling_by_trigger: SharedExtentMap<Trigger, HeldDependency>,
    /// The entries whose whole selector is the one name each fact is of, written in ASCII lowercase,
    /// which a change to the fact restyles the element for by name: no compound is held for them,
    /// nothing being left of the name, right of it or else in it.
    pub(super) lone_by_trigger: SharedExtentMap<Trigger, u32>,
    /// Whether the buckets of the map's entries tell those entries, as the bucket of a name holds
    /// each entry it proves, so that the map need not list them: those of a sheet segment do.
    lone_entries_in_buckets: bool,
    /// The compounds the map holds, by their numbers, with those they lead to and from.
    compounds: SharedPrefixVec<HeldCompound>,
    /// The names of the compounds testing names alone, each compound's in a run of its own.
    operand_names: SharedPrefixVec<OperandName>,
    /// How many kinds of own tests the compounds the map holds have.
    shape_count: u32,
    /// How many chains the compounds the map holds have.
    chain_count: u32,
    /// The compound each [`ChainEnd`] names, by its number, once a chain ends so.
    chain_ends: [Option<u32>; 2],
    /// The bounds compounds are of: the first compound of each bound the map took, by its number
    /// which these run up in, with the bound's place among the bounds of its levels.
    bound_runs: Vec<(u32, u16)>,
    /// Which positions compounds read.
    pub(super) reads_positions: ReadPositions,
    /// The `:has()` queries of the selectors, one per relative selector, which `Trigger::Relative`
    /// names by index.
    pub(super) relative: SharedPrefixVec<RelativeEntry>,
    /// The queries whose arguments test each fact, with whether a compound testing it is of a
    /// selector nested in the argument, which describes elements anywhere in the tree.
    ///
    /// NB: A change to a fact no argument tests flips no anchor, and is not followed to any.
    pub(super) in_relative_argument: HashMap<Trigger, SmallVec<[(u32, bool); 2]>>,
    /// The qualified attribute names the triggers name.
    names: NameLeases,
    /// How many dependencies the map added itself.
    dependencies: usize,
}

/// Where the numbers of a part of a map start: the high bits of a query number name its part,
/// and the low ones its number there.
const PART_SHIFT: u32 = 22;
const LOCAL_MASK: u32 = (1 << PART_SHIFT) - 1;

fn split(number: u32) -> (usize, u32) {
    ((number >> PART_SHIFT) as usize, number & LOCAL_MASK)
}

fn tag(part: usize, local: u32) -> u32 {
    debug_assert!(local <= LOCAL_MASK, "a part numbers fewer than its space holds");
    (part as u32) << PART_SHIFT | local
}

/// A sheet segment's map, whose entries the document numbers from `first_entry`.
#[derive(Clone)]
struct SegmentPart {
    segment: Arc<SheetSegment>,
    first_entry: u32,
}

/// Which elements a change to one element's facts restyles, for every entry of a document's rule
/// index: the map of the user agent's entries and the document's own, part zero, and the map of
/// each sheet segment the index binds, which every document binding the segment shares, after it.
/// Its queries are numbered by part, and its entries as the index numbers them.
///
/// NB: A segment's part is built once for the parsed sheet, and a document reads the part of
///     each segment it binds as it reads its own.
#[derive(Clone, Default)]
pub(super) struct InvalidationMap {
    base: InvalidationPart,
    segments: Vec<SegmentPart>,
    /// Which positions compounds read, of every part.
    pub(super) reads_positions: ReadPositions,
    /// Where the queries of each part start, numbered one after another across the parts, with how
    /// many there are in all after the last.
    query_slots: SmallVec<[u32; 4]>,
    /// Where the memo slots of the compounds of each part start, and those of its kinds of own
    /// tests, after the compounds of every part.
    memo_starts: SmallVec<[(u32, u32, u32); 4]>,
    memo_slot_count: u32,
    /// See [`Self::arrival_bits`].
    arrival_bits: u64,
    /// See [`Self::every_query_needs_an_arrival_key`].
    every_query_needs_an_arrival_key: bool,
}

impl InvalidationMap {
    /// The map of the entries of `index`: that of the user agent index it shares, which the
    /// process built once, with the document's own entries added after it, and the maps of the
    /// segments it binds.
    pub(super) fn build(index: &RuleIndex) -> Self {
        let mut base = index
            .user_agent_index()
            .map_or_else(InvalidationPart::default, |user_agent| user_agent.invalidation.clone());
        base.extend(index.entries.own_numbers(), Self::own_selectors(index));
        base.shrink_to_fit();
        let mut map = Self {
            base,
            segments: Self::segments_of(index),
            ..Self::default()
        };
        map.refresh();
        map
    }

    /// Have the map hold what `index` does after an update: the entries `died` leave it, those
    /// numbered in `arrived` join it, and it binds the segments the index binds, the entries of
    /// one it bound before and the index buckets itself now joining its own part.
    pub(super) fn update(&mut self, index: &RuleIndex, died: &[u32], arrived: std::ops::Range<usize>) {
        self.base.remove(index, died);
        let segments = Self::segments_of(index);
        for segment in std::mem::take(&mut self.segments) {
            let kept = segments
                .iter()
                .any(|kept| Arc::ptr_eq(&kept.segment, &segment.segment) && kept.first_entry == segment.first_entry);
            if !kept {
                let first = segment.first_entry as usize;
                self.base.extend(
                    first..first + segment.segment.len() as usize,
                    Self::own_selectors(index),
                );
            }
        }
        self.base.extend(arrived, Self::own_selectors(index));
        self.segments = segments;
        self.refresh();
    }

    /// The selector of each live entry of `index` the index buckets itself.
    fn own_selectors<'a>(
        index: &'a RuleIndex,
    ) -> impl Fn(u32) -> Option<(EntrySelector<'a>, &'a crate::css::selector::SelectorListRef)> + 'a {
        move |entry| {
            let indexed = index.entries.view(entry as usize);
            (indexed.live() && !index.in_segment(entry))
                .then(|| (index.entry_selector(entry), index.arena_of_entry(entry)))
        }
    }

    fn segments_of(index: &RuleIndex) -> Vec<SegmentPart> {
        index
            .bound_segments()
            .map(|(segment, first_entry)| SegmentPart {
                segment: Arc::clone(segment),
                first_entry,
            })
            .collect()
    }

    /// Gather what the parts say together, and where their numbers start.
    fn refresh(&mut self) {
        assert!(
            self.base.relative.len() <= LOCAL_MASK as usize,
            "invalidation map space exhausted"
        );
        assert!(
            self.segments.len() < (1 << (32 - PART_SHIFT)) - 1,
            "invalidation map parts exhausted"
        );
        let mut reads_positions = ReadPositions::default();
        let mut query_slots = SmallVec::new();
        let mut queries = 0u32;
        let compound_count: u32 = self.parts().map(|(_, map, _)| map.compounds.len() as u32).sum();
        let shape_count: u32 = self.parts().map(|(_, map, _)| map.shape_count).sum();
        let (mut compounds, mut shapes, mut chains) = (0u32, compound_count, compound_count + shape_count);
        let mut memo_starts = SmallVec::new();
        let (mut arrival_bits, mut every_query_needs_an_arrival_key) = (0, true);
        for (_, map, _) in self.parts() {
            reads_positions.add(map.reads_positions);
            query_slots.push(queries);
            queries += map.relative.len() as u32;
            for query in (0..map.relative.len()).map(|query| &map.relative[query]) {
                arrival_bits |= query.arrival_bits;
                every_query_needs_an_arrival_key &=
                    !(query.reads_positions || query.reads_emptiness || query.nested_reaches_out);
            }
            memo_starts.push((compounds, shapes, chains));
            compounds += map.compounds.len() as u32;
            shapes += map.shape_count;
            chains += map.chain_count;
        }
        self.memo_starts = memo_starts;
        self.memo_slot_count = chains;
        query_slots.push(queries);
        self.reads_positions = reads_positions;
        self.query_slots = query_slots;
        (self.arrival_bits, self.every_query_needs_an_arrival_key) = (arrival_bits, every_query_needs_an_arrival_key);
    }

    /// The map of `part`, with the number its entries start from in the index.
    fn part(&self, part: usize) -> (&InvalidationPart, u32) {
        match part {
            0 => (&self.base, 0),
            _ => {
                let segment = &self.segments[part - 1];
                (&segment.segment.invalidation, segment.first_entry)
            }
        }
    }

    fn parts(&self) -> impl Iterator<Item = (usize, &InvalidationPart, u32)> + Clone {
        (0..=self.segments.len()).map(move |part| {
            let (map, first_entry) = self.part(part);
            (part, map, first_entry)
        })
    }

    /// The bytes of the map's own part, the segments' being the process's.
    pub(super) fn heap_size(&self) -> usize {
        use crate::heap_size::{small_vec_bytes, vec_bytes};
        self.base.heap_size() + vec_bytes(&self.segments) + small_vec_bytes(&self.query_slots)
    }

    /// The map of the user agent's entries and the document's own.
    #[cfg(test)]
    pub(super) fn base(&self) -> &InvalidationPart {
        &self.base
    }

    /// The compound `dependency` names, with the number the entries of its part start from.
    fn held(&self, dependency: Dependency) -> (&HeldCompound, u32) {
        let (part, local) = split(dependency.compound.0);
        let (map, first_entry) = self.part(part);
        (&map.compounds[local as usize], first_entry)
    }

    /// The compound numbered `local` in the part `of` names, of the entry of `of`.
    fn in_same_part(of: Dependency, local: u32) -> Option<Dependency> {
        (local != NO_COMPOUND).then(|| Dependency {
            entry: of.entry,
            compound: CompoundID(tag(split(of.compound.0).0, local)),
        })
    }

    /// The step left of the compound `link`, as the map worked it out.
    pub(super) fn step(&self, link: Dependency) -> Step {
        let (held, _) = self.held(link);
        Step {
            entry: link.entry,
            combinator: held.combinator,
            next: Self::in_same_part(link, held.next),
            reaches_every_subject: held.has(HeldCompound::REACHES_EVERY_SUBJECT),
            required_key: held.required_key,
        }
    }

    /// The compound right of `dependency`, whose step leads on from an element matching it, or
    /// none for the subject compound.
    pub(super) fn right(&self, dependency: Dependency) -> Option<Dependency> {
        Self::in_same_part(dependency, self.held(dependency).0.next)
    }

    /// The combinator left of `dependency`, with the compound left of it, where invalidation
    /// follows one.
    pub(super) fn left(&self, dependency: Dependency) -> Option<(ReachCombinator, Dependency)> {
        let (held, _) = self.held(dependency);
        Self::in_same_part(dependency, held.left).map(|left| (held.combinator, left))
    }

    /// How many answers of each kind a memo of what one element matches holds: one for each
    /// compound the map holds, and one for each kind of own tests.
    pub(super) fn memo_slot_count(&self) -> usize {
        self.memo_slot_count as usize
    }

    /// The memo slot of the compound of `dependency`.
    pub(super) fn compound_memo_slot(&self, dependency: Dependency) -> u32 {
        let (part, local) = split(dependency.compound.0);
        self.memo_starts[part].0 + local
    }

    /// The memo slot of the own tests of the compound of `dependency`, which the compounds testing
    /// the same facts alike share, where they share them.
    pub(super) fn own_tests_memo_slot(&self, dependency: Dependency) -> Option<u32> {
        let (part, _) = split(dependency.compound.0);
        let (held, _) = self.held(dependency);
        (held.shape != HeldCompound::NO_SHAPE).then(|| self.memo_starts[part].1 + held.shape)
    }

    /// The memo slot of the compound of `dependency` with what is left of it, which the
    /// compounds whose chains are alike share, where they share it.
    pub(super) fn chain_memo_slot(&self, dependency: Dependency) -> Option<u32> {
        let (part, _) = split(dependency.compound.0);
        let (held, _) = self.held(dependency);
        (held.chain != HeldCompound::NO_SHAPE).then(|| self.memo_starts[part].2 + held.chain)
    }

    /// The names the compound of `dependency` tests, where it tests nothing else, or none.
    pub(super) fn operand_names(&self, dependency: Dependency) -> OperandNames<'_> {
        let (part, local) = split(dependency.compound.0);
        let (map, _) = self.part(part);
        let held = &map.compounds[local as usize];
        let start = held.operand_names as usize;
        match held.operand_name_count {
            0 => OperandNames::None,
            1 => {
                let name = StyleAtomID(held.operand_names);
                OperandNames::One(match () {
                    _ if held.has(HeldCompound::LONE_CLASS) => OperandName::Class(name),
                    _ if held.has(HeldCompound::LONE_ID) => OperandName::Id(name),
                    _ => OperandName::Tag(name),
                })
            }
            count => OperandNames::Many(&map.operand_names[start..start + usize::from(count)]),
        }
    }

    /// Whether the compound of `dependency` evaluates a `:has()`, or a compound left of it does.
    pub(super) fn reads_relative(&self, dependency: Dependency) -> bool {
        self.held(dependency).0.has(HeldCompound::READS_RELATIVE)
    }

    /// The selector of the dependency's entry, and the selector its compound is of, in that or a
    /// bound of its `@scope` levels, from the compound leftwards.
    pub(super) fn resolve<'a>(
        &'a self,
        dependency: Dependency,
        index: &'a RuleIndex,
    ) -> (EntrySelector<'a>, Selector<'a>) {
        let (part, local) = split(dependency.compound.0);
        let (map, _) = self.part(part);
        let held = &map.compounds[local as usize];
        if !held.has(HeldCompound::SCOPED) {
            let selector = held.selector.view();
            return (EntrySelector::unscoped(selector), selector.at_compound(held.offset));
        }
        let entry = index.entry_selector(dependency.entry);
        let selector = match held.has(HeldCompound::IN_BOUND) {
            true => {
                let run = map.bound_runs.partition_point(|&(first, _)| first <= local) - 1;
                bound_of(entry, map.bound_runs[run].1)
            }
            false => entry.selector,
        };
        (entry, selector.at_compound(held.offset))
    }

    /// What a change to `trigger` can restyle: the compounds testing it.
    pub(super) fn effects(&self, trigger: &Trigger) -> impl Iterator<Item = Dependency> + Clone + '_ {
        self.held_effects(trigger).map(|held| held.dependency)
    }

    /// The compounds testing `trigger`, with the keys some ancestor of an element matching each carries.
    pub(super) fn held_effects(&self, trigger: &Trigger) -> impl Iterator<Item = HeldDependency> + Clone + '_ {
        let trigger = *trigger;
        self.parts().flat_map(move |(part, map, first_entry)| {
            local_trigger(trigger, part)
                .into_iter()
                .flat_map(move |local| map.by_trigger.values(&local))
                .map(move |held| HeldDependency {
                    dependency: rebased(&held.dependency, part, first_entry),
                    ancestor_hashes: held.ancestor_hashes,
                })
        })
    }

    /// The entries whose whole selector is the name `trigger` is of.
    pub(super) fn lone_entries(&self, trigger: &Trigger) -> impl Iterator<Item = u32> + '_ {
        let trigger = *trigger;
        let in_buckets = name_key_of(trigger).into_iter().flat_map(move |key| {
            self.segments.iter().flat_map(move |part| {
                part.segment
                    .entries_proven_by(key)
                    .map(move |entry| entry + part.first_entry)
            })
        });
        self.base
            .lone_by_trigger
            .values(&trigger)
            .map(move |&entry| entry + self.part(0).1)
            .chain(in_buckets)
    }

    /// Whether the buckets of a segment hold entries of the name `trigger` is of, whose selectors
    /// test it.
    fn segment_buckets_test(&self, trigger: &Trigger) -> bool {
        name_key_of(*trigger).is_some_and(|key| self.segments.iter().any(|part| part.segment.buckets_hold(key)))
    }

    /// The dependencies of `trigger` whose combinator right of them is a sibling combinator's, with
    /// the ancestor keys each holds.
    pub(super) fn held_sibling_effects(&self, trigger: &Trigger) -> impl Iterator<Item = HeldDependency> + Clone + '_ {
        let trigger = *trigger;
        self.parts().flat_map(move |(part, map, first_entry)| {
            local_trigger(trigger, part)
                .into_iter()
                .flat_map(move |local| map.sibling_by_trigger.values(&local))
                .map(move |held| HeldDependency {
                    dependency: rebased(&held.dependency, part, first_entry),
                    ancestor_hashes: held.ancestor_hashes,
                })
        })
    }

    /// Whether a compound tests `trigger`.
    pub(super) fn has_effects(&self, trigger: &Trigger) -> bool {
        self.segment_buckets_test(trigger)
            || self
                .parts()
                .any(|(part, map, _)| local_trigger(*trigger, part).is_some_and(|local| map.has_effects(&local)))
    }

    /// Whether a change to `trigger` can restyle anything.
    pub(super) fn tests(&self, trigger: &Trigger) -> bool {
        self.reach_of(trigger) != TriggerReach::None
    }

    /// What a change to `trigger` can restyle, in one look at each part.
    pub(super) fn reach_of(&self, trigger: &Trigger) -> TriggerReach {
        if self.segment_buckets_test(trigger) {
            return TriggerReach::Direct;
        }
        let mut reach = TriggerReach::None;
        for (part, map, _) in self.parts() {
            let Some(local) = local_trigger(*trigger, part) else {
                continue;
            };
            if map.has_effects(&local) {
                return TriggerReach::Direct;
            }
            if map.in_relative_argument.contains_key(&local) {
                reach = TriggerReach::Indirect;
            }
        }
        reach
    }

    /// How many `:has()` queries there are.
    pub(super) fn query_count(&self) -> usize {
        self.query_slots.last().copied().unwrap_or(0) as usize
    }

    /// Every `:has()` query, by its number.
    pub(super) fn queries(&self) -> impl Iterator<Item = u32> + '_ {
        self.parts()
            .flat_map(|(part, map, _)| (0..map.relative.len() as u32).map(move |local| tag(part, local)))
    }

    /// The slot of `query` among the [`Self::query_count`] queries.
    pub(super) fn query_slot(&self, query: u32) -> usize {
        let (part, local) = split(query);
        (self.query_slots[part] + local) as usize
    }

    /// The union of the queries' [`RelativeEntry::arrival_bits`].
    pub(super) fn arrival_bits(&self) -> u64 {
        self.arrival_bits
    }

    /// Whether an element arriving or leaving reaches a query only by carrying one of its
    /// `arrival_keys`: no query reads positions or emptiness, or reaches out through a selector
    /// nested in its argument.
    pub(super) fn every_query_needs_an_arrival_key(&self) -> bool {
        self.every_query_needs_an_arrival_key
    }

    /// The `:has()` query `query`.
    pub(super) fn relative(&self, query: u32) -> &RelativeEntry {
        let (part, local) = split(query);
        &self.part(part).0.relative[local as usize]
    }

    /// Whether the argument of some query tests a fact.
    #[cfg(test)]
    pub(super) fn has_relative_arguments(&self) -> bool {
        self.parts().any(|(_, map, _)| !map.in_relative_argument.is_empty())
    }

    /// The rule index entry whose selector `query` is of.
    pub(super) fn relative_entry(&self, query: u32) -> u32 {
        let (part, local) = split(query);
        let (map, first_entry) = self.part(part);
        map.relative[local as usize].entry + first_entry
    }

    /// The queries whose arguments test `trigger`, with whether a compound testing it is of a
    /// selector nested in the argument.
    pub(super) fn queries_testing(&self, trigger: &Trigger) -> impl Iterator<Item = (u32, bool)> + '_ {
        let trigger = *trigger;
        self.parts().flat_map(move |(part, map, _)| {
            local_trigger(trigger, part)
                .and_then(|local| map.in_relative_argument.get(&local))
                .into_iter()
                .flatten()
                .map(move |&(query, nested)| (tag(part, query), nested))
        })
    }
}

/// `trigger` as `part` numbers it, where it is a fact any part tests, or a query of that part.
fn local_trigger(trigger: Trigger, part: usize) -> Option<Trigger> {
    match trigger {
        Trigger::Relative(query) => {
            let (of, local) = split(query);
            (of == part).then_some(Trigger::Relative(local))
        }
        trigger => Some(trigger),
    }
}

fn rebased(dependency: &Dependency, part: usize, first_entry: u32) -> Dependency {
    Dependency {
        entry: dependency.entry + first_entry,
        compound: CompoundID(tag(part, dependency.compound.0)),
    }
}

/// The dispatch key the bucket of the name `trigger` is of has, where it is an id's or a class's.
fn name_key_of(trigger: Trigger) -> Option<DispatchKey> {
    match trigger {
        Trigger::Class(class) => Some(DispatchKey::Class(class)),
        Trigger::Id(id) => Some(DispatchKey::Id(id)),
        _ => None,
    }
}

/// How the anchors of a `:has()` stand to an element its argument reaches.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum AnchorReach {
    /// `:has(> x)`: its parent.
    Parent,
    /// `:has(x)`, and arguments with more compounds going down: its ancestors.
    Ancestors,
    /// `:has(+ x)`: its previous sibling.
    PreviousSibling,
    /// `:has(~ x)`: its previous siblings.
    PreviousSiblings,
    /// `:has(+ x y)`: the previous sibling of it and of each of its ancestors.
    PreviousSiblingOfAncestors,
    /// `:has(~ x y)`, `:has(+ x + y)`: the previous siblings of it and of each of its ancestors.
    PreviousSiblingsOfAncestors,
}

/// One relative selector of a `:has()` of a rule index entry's selector.
#[derive(Clone)]
pub(super) struct RelativeEntry {
    /// The rule index entry whose selector the query is of, numbered in its part: see
    /// [`InvalidationMap::relative_entry`].
    entry: u32,
    /// Where the relative selector starts, in the arena of the entry's selector or of `bound`.
    argument: ComponentOffset,
    /// The bound the relative selector is in, by its place among the bounds of the entry's `@scope`
    /// levels, where it is in one.
    bound: Option<u16>,
    pub(super) reach: AnchorReach,
    /// Whether the anchor is the host of the shadow tree the argument is walked in, which an
    /// element stands in rather than around.
    pub(super) in_shadow_tree: bool,
    /// What one element of the subtree of an element arriving or leaving has to carry for the
    /// arrival to flip the answer for an anchor, or none where any element can.
    ///
    /// NB: An argument whose compounds only go down from the anchor is answered by the subject
    ///     compound of the argument alone: the other compounds are of the ancestors of a witness,
    ///     which arrive and leave with it.
    pub(super) arrival_keys: Option<SmallVec<[Trigger; 4]>>,
    /// The dispatch bloom bits of `arrival_keys`, or every bit where an element can carry what it
    /// takes without a bit of its own. An element carrying one of the keys has its bit.
    pub(super) arrival_bits: u64,
    /// Whether the argument reads where elements stand among their siblings, which any element
    /// arriving or leaving moves for the others.
    pub(super) reads_positions: bool,
    /// Whether the argument reads whether an element is empty, which an element arriving or
    /// leaving moves for its parent.
    pub(super) reads_emptiness: bool,
    /// Whether a selector nested in the argument leads from its subject to elements out of the
    /// argument's chain, which can stand anywhere in the tree.
    pub(super) nested_reaches_out: bool,
    /// Whether such a selector steps sideways from such an element, to its later siblings.
    pub(super) nested_steps_sideways: bool,
    /// What one element of the argument's chain carries, or none where it can carry anything.
    pub(super) chain_keys: Option<SmallVec<[Trigger; 4]>>,
    /// Whether an anchor answers true wherever one of its descendants does, before as after, so
    /// that the ancestors of an element above an anchor answering so keep their answers.
    ///
    /// NB: A witness below a descendant of an anchor is below the anchor too, so an anchor above
    ///     one that already answers true answers true as well.
    pub(super) answers_above_descendants: bool,
}

impl RelativeEntry {
    /// The relative selector, of a query of the entry `entry` is the selector of.
    pub(super) fn argument<'a>(&self, entry: EntrySelector<'a>) -> Selector<'a> {
        let selector = match self.bound {
            Some(place) => bound_of(entry, place),
            None => entry.selector,
        };
        selector.at_compound(self.argument)
    }

    fn new(
        entry: u32,
        argument: Selector<'_>,
        bound: Option<u16>,
        in_shadow_tree: bool,
        names: &mut AcquiredNames,
    ) -> Self {
        use super::super::selector::RelativeAxis;
        let axis = match argument.is_relative_anchor() {
            true => RelativeAxis::Descendant,
            false => relative_axis(argument).0,
        };
        let reach = match axis {
            RelativeAxis::Child => AnchorReach::Parent,
            RelativeAxis::Descendant => AnchorReach::Ancestors,
            RelativeAxis::NextSibling => AnchorReach::PreviousSibling,
            RelativeAxis::FollowingSibling => AnchorReach::PreviousSiblings,
            RelativeAxis::NextSiblingSubtree => AnchorReach::PreviousSiblingOfAncestors,
            RelativeAxis::FollowingSiblingSubtree => AnchorReach::PreviousSiblingsOfAncestors,
        };
        let mut shape = ArgumentShape::default();
        shape.read(argument);
        // An element arriving or leaving beside an anchor's next sibling is its next sibling then.
        let adjacent_to_the_anchor = matches!(
            reach,
            AnchorReach::PreviousSibling | AnchorReach::PreviousSiblingOfAncestors
        );
        let arrival_keys = match (adjacent_to_the_anchor || shape.next_sibling_steps, shape.sibling_steps) {
            (true, _) => None,
            // NB: With a step sideways, the element arriving can be the one left of it.
            (false, true) => chain_keys(argument, names),
            (false, false) => compound_keys(&argument.first_compound(), names),
        };
        Self {
            entry,
            argument: argument.offset(),
            bound,
            reach,
            in_shadow_tree,
            arrival_bits: arrival_bits_of(arrival_keys.as_deref()),
            arrival_keys,
            reads_positions: shape.positions,
            reads_emptiness: shape.emptiness,
            nested_reaches_out: false,
            nested_steps_sideways: shape.nested_sibling_steps,
            chain_keys: chain_keys(argument, names),
            answers_above_descendants: reach == AnchorReach::Ancestors && !shape.ties_to_the_anchor_as_child,
        }
    }
}

/// The dispatch bloom bits an element carrying one of `keys` has one of, or every bit where it may
/// carry any of them without one.
fn arrival_bits_of(keys: Option<&[Trigger]>) -> u64 {
    let Some(keys) = keys else {
        return u64::MAX;
    };
    keys.iter().fold(0, |bits, key| {
        bits | match *key {
            Trigger::Class(atom) => dispatch_bloom_bit(DispatchKey::Class(atom)),
            Trigger::Id(atom) => dispatch_bloom_bit(DispatchKey::Id(atom)),
            Trigger::Attribute(atom) => dispatch_bloom_bit(DispatchKey::AttributeName(atom)),
            Trigger::State(fact) => dispatch_bloom_bit(DispatchKey::State(fact)),
            Trigger::CustomState(atom) => dispatch_bloom_bit(DispatchKey::CustomState(atom)),
            Trigger::Part(atom) => dispatch_bloom_bit(DispatchKey::Part(atom)),
            Trigger::Tag(atom) => dispatch_bloom_bit(DispatchKey::TagName(atom)),
            _ => u64::MAX,
        }
    })
}

/// What the argument of a `:has()` reads of the tree.
#[derive(Default)]
struct ArgumentShape {
    sibling_steps: bool,
    next_sibling_steps: bool,
    nested_sibling_steps: bool,
    /// Whether the selector being read is nested in the argument.
    nested: bool,
    positions: bool,
    emptiness: bool,
    /// Whether the leftmost compound is the anchor's child, as in `:has(> x y)`.
    ties_to_the_anchor_as_child: bool,
}

impl ArgumentShape {
    fn read(&mut self, selector: Selector<'_>) {
        let mut compound = Some(selector);
        while let Some(current) = compound {
            for simple in current.first_compound().simple_selectors.iter() {
                let simple = &simple;
                self.read_simple(simple);
            }
            let Some((relation, left)) = current.left() else {
                break;
            };
            match relation {
                Combinator::ImmediateChild if left.is_relative_anchor() => {
                    self.ties_to_the_anchor_as_child = true;
                }
                Combinator::NextSibling => {
                    self.sibling_steps = true;
                    self.next_sibling_steps = true;
                    self.nested_sibling_steps |= self.nested;
                }
                Combinator::SubsequentSibling => {
                    self.sibling_steps = true;
                    self.nested_sibling_steps |= self.nested;
                }
                _ => {}
            }
            compound = Some(left);
        }
    }

    fn read_simple(&mut self, simple: &SimpleSelector<'_>) {
        match simple {
            SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
                PseudoClassType::Is | PseudoClassType::Where | PseudoClassType::Not => {
                    let nested = std::mem::replace(&mut self.nested, true);
                    for selector in pseudo_class.argument_selector_list {
                        self.read(selector);
                    }
                    self.nested = nested;
                }
                PseudoClassType::Host => {
                    for selector in pseudo_class.argument_selector_list {
                        self.read(selector);
                    }
                }
                PseudoClassType::Empty => self.emptiness = true,
                _ => {
                    let (positions, of) = positions_read(pseudo_class);
                    self.positions |= !positions.is_empty();
                    if let Some(of) = of {
                        let nested = std::mem::replace(&mut self.nested, true);
                        for selector in of {
                            self.read(selector);
                        }
                        self.nested = nested;
                    }
                }
            },
            SimpleSelector::PseudoElement(PseudoElementSelector {
                value: PseudoElementValue::CompoundSelector(argument),
                ..
            }) => self.read(*argument),
            SimpleSelector::Nesting(Nesting::Rule(parent)) => {
                let nested = std::mem::replace(&mut self.nested, true);
                for selector in *parent {
                    self.read(selector);
                }
                self.nested = nested;
            }
            _ => {}
        }
    }
}

/// The facts one of which every element matching `compound` carries, as triggers, or none where
/// an element carrying none of the facts can match it.
fn compound_keys(compound: &CompoundRef<'_>, names: &mut AcquiredNames) -> Option<SmallVec<[Trigger; 4]>> {
    fn simple_keys(simple: &SimpleSelector<'_>, names: &mut AcquiredNames) -> Option<SmallVec<[Trigger; 4]>> {
        match simple {
            SimpleSelector::PseudoClass(pseudo_class)
                if matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where) =>
            {
                alternatives_keys(pseudo_class.argument_selector_list, names)
            }
            SimpleSelector::Nesting(Nesting::Rule(parent)) => alternatives_keys(*parent, names),
            SimpleSelector::PseudoClass(pseudo_class)
                if matches!(
                    pseudo_class.pseudo_class,
                    PseudoClassType::Dir | PseudoClassType::Lang | PseudoClassType::Heading | PseudoClassType::Empty
                ) =>
            {
                None
            }
            SimpleSelector::Class(_)
            | SimpleSelector::Id(_)
            | SimpleSelector::TagName(_)
            | SimpleSelector::Attribute(_)
            | SimpleSelector::PseudoClass(_)
            | SimpleSelector::PseudoElement(_) => {
                let triggers = own_triggers(simple, names);
                (!triggers.is_empty()).then(|| triggers.into_iter().collect())
            }
            _ => None,
        }
    }
    fn alternatives_keys(list: SelectorListView<'_>, names: &mut AcquiredNames) -> Option<SmallVec<[Trigger; 4]>> {
        let mut keys = SmallVec::new();
        for selector in list {
            keys.extend(compound_keys(&selector.first_compound(), names)?);
        }
        (!keys.is_empty()).then_some(keys)
    }
    // One required simple selector is enough, the fewest keys the better.
    compound
        .simple_selectors
        .iter()
        .filter_map(|simple| simple_keys(&simple, names))
        .min_by_key(|keys| keys.len())
}

/// The facts one of which every element matching each compound of the relative selector
/// `argument` carries, but for the anchor's, or none where an element carrying none of them can
/// match one.
fn chain_keys(argument: Selector<'_>, names: &mut AcquiredNames) -> Option<SmallVec<[Trigger; 4]>> {
    fn add(keys: &mut SmallVec<[Trigger; 4]>, more: SmallVec<[Trigger; 4]>) {
        for key in more {
            if !keys.contains(&key) {
                keys.push(key);
            }
        }
    }
    // The compounds the selectors of a simple selector lead to describe other elements.
    fn other_elements(
        compound: &CompoundRef<'_>,
        keys: &mut SmallVec<[Trigger; 4]>,
        names: &mut AcquiredNames,
    ) -> Option<()> {
        for simple in compound.simple_selectors.iter() {
            let simple = &simple;
            let lists: SmallVec<[SelectorListView<'_>; 1]> = match simple {
                SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class != PseudoClassType::Has => {
                    smallvec::smallvec![pseudo_class.argument_selector_list]
                }
                SimpleSelector::Nesting(Nesting::Rule(parent)) => smallvec::smallvec![*parent],
                _ => SmallVec::new(),
            };
            for selector in lists.into_iter().flatten() {
                other_elements(&selector.first_compound(), keys, names)?;
                for left in selector.compounds().skip(1) {
                    add(keys, compound_keys(&left, names)?);
                    other_elements(&left, keys, names)?;
                }
            }
        }
        Some(())
    }
    let subject = argument.first_compound();
    let mut keys = compound_keys(&subject, names)?;
    other_elements(&subject, &mut keys, names)?;
    for compound in argument.compounds().skip(1) {
        add(&mut keys, compound_keys(&compound, names)?);
        other_elements(&compound, &mut keys, names)?;
    }
    Some(keys)
}

impl InvalidationPart {
    /// The bytes of the map's triggers and queries.
    pub(super) fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, small_vec_bytes, vec_bytes};
        self.by_trigger.heap_size()
            + self.sibling_by_trigger.heap_size()
            + self.lone_by_trigger.heap_size()
            + self.compounds.heap_size()
            + self.operand_names.heap_size()
            + self.relative.heap_size()
            + map_bytes(&self.in_relative_argument)
            + self.in_relative_argument.values().map(small_vec_bytes).sum::<usize>()
            + vec_bytes(&self.names.atoms)
            + vec_bytes(&self.bound_runs)
    }

    /// The bytes of what the map shares with the maps that start from it, which it shares with
    /// none but those.
    pub(super) fn shared_heap_size(&self) -> usize {
        self.by_trigger.shared_heap_size()
            + self.sibling_by_trigger.shared_heap_size()
            + self.lone_by_trigger.shared_heap_size()
            + self.compounds.shared_heap_size()
            + self.operand_names.shared_heap_size()
            + self.relative.shared_heap_size()
    }

    /// Whether the map starts from `base`, sharing what it holds.
    #[cfg(test)]
    pub(super) fn starts_from(&self, base: &InvalidationPart) -> bool {
        self.relative.shares_prefix_of(&base.relative)
            && self.by_trigger.shared_heap_size() == base.by_trigger.shared_heap_size()
    }

    /// Make everything the map holds shared, so that the maps of the documents that start from it
    /// share it rather than holding copies.
    pub(super) fn freeze(&mut self) {
        self.by_trigger.freeze();
        self.sibling_by_trigger.freeze();
        self.lone_by_trigger.freeze();
        self.compounds.freeze();
        self.operand_names.freeze();
        self.relative.freeze();
        self.dependencies = 0;
        self.shrink_to_fit();
    }

    /// Releases what building the map reserved beyond what it holds. An extension grows it again
    /// as it needs, and the next build gives the room back.
    fn shrink_to_fit(&mut self) {
        self.by_trigger.shrink_to_fit();
        self.sibling_by_trigger.shrink_to_fit();
        self.lone_by_trigger.shrink_to_fit();
        self.compounds.shrink_to_fit();
        self.operand_names.shrink_to_fit();
        self.relative.shrink_to_fit();
        self.in_relative_argument.shrink_to_fit();
        for queries in self.in_relative_argument.values_mut() {
            queries.shrink_to_fit();
        }
        self.bound_runs.shrink_to_fit();
    }

    /// Add the entries `entries` the map is to hold to it, which holds none of them, by the
    /// selector of each.
    ///
    /// NB: An entry that dies stays in the map, which is built again for it.
    pub(super) fn extend<'a>(
        &mut self,
        entries: std::ops::Range<usize>,
        selector_of: impl Fn(u32) -> Option<(EntrySelector<'a>, &'a crate::css::selector::SelectorListRef)>,
    ) {
        let before = self.dependencies;
        let mut builder = PartBuilder::new(self);
        for entry in entries {
            let entry = u32::try_from(entry).expect("rule entry space exhausted");
            if let Some((selector, selectors)) = selector_of(entry) {
                builder.collect_entry(entry, selector, selectors);
                let collected: SmallVec<[u32; 8]> = builder.interned.values().copied().collect();
                for local in collected {
                    builder.number_chain(local);
                }
            }
        }
        let (touched, names) = (builder.touched, builder.names);
        self.names.settle(names);
        let touched: HashSet<Trigger> = touched.into_iter().collect();
        for trigger in &touched {
            for map in [&mut self.by_trigger, &mut self.sibling_by_trigger] {
                map.sort_and_dedup_own_by_key(trigger, |held| held.dependency);
            }
            self.lone_by_trigger.sort_and_dedup_own_by_key(trigger, |&entry| entry);
        }
        // An extension that added at least as many dependencies as the map had gives back the room
        // its growth reserved, as a build does: each dependency pays for that once, however the
        // rules arrive. One that moved many lists gives back the room they left.
        if self.dependencies - before >= before {
            self.shrink_to_fit();
        } else {
            for map in [&mut self.by_trigger, &mut self.sibling_by_trigger] {
                if map.has_slack() {
                    map.shrink_to_fit();
                }
            }
            if self.lone_by_trigger.has_slack() {
                self.lone_by_trigger.shrink_to_fit();
            }
        }
    }

    /// Leave the entries whose whole selector is one name to the buckets of the map's entries, which
    /// hold each of them under that name.
    pub(super) fn leave_lone_entries_to_buckets(&mut self) {
        self.lone_entries_in_buckets = true;
    }

    /// Take the entries `died` of `index` out of the map: a change to a fact their selectors test
    /// restyles nothing for them any more. Their queries stay, unreachable, until the map is built
    /// again, as the dead entries keep their numbers until the index is.
    ///
    /// NB: Which positions the compounds read stays as it was, which has a change look at more
    ///     elements at worst.
    pub(super) fn remove(&mut self, index: &RuleIndex, died: &[u32]) {
        if died.is_empty() {
            return;
        }
        let mut dead = vec![false; index.entries.len()];
        for &entry in died {
            dead[entry as usize] = true;
        }
        let is_dead = |entry: u32| dead.get(entry as usize).copied().unwrap_or(false);
        // The facts the selectors that died test are those a map of them alone has dependencies or
        // queries for, but for the queries, which that map numbers anew.
        let mut died_alone = InvalidationPart::default();
        let mut builder = PartBuilder::new(&mut died_alone);
        // NB: An entry of a segment the index let go of was in the segment's map alone.
        for &entry in died.iter().filter(|&&entry| index.holds_selector_of(entry)) {
            builder.collect_entry(entry, index.entry_selector(entry), index.arena_of_entry(entry));
        }
        let dead_queries: Vec<u32> = (0..self.relative.len())
            .filter(|&query| is_dead(self.relative[query].entry))
            .map(|query| u32::try_from(query).expect("relative query space exhausted"))
            .collect();
        for trigger in died_alone.lone_by_trigger.own_keys() {
            if self.lone_by_trigger.retain_own(trigger, |&entry| !is_dead(entry)) == Some(0) {
                self.lone_by_trigger.remove_own(trigger);
            }
        }
        let triggers = died_alone
            .by_trigger
            .own_keys()
            .filter(|trigger| !matches!(trigger, Trigger::Relative(_)))
            .copied()
            .chain(dead_queries.iter().map(|&query| Trigger::Relative(query)));
        for trigger in triggers {
            // NB: The entries of the map this one starts from die only with it.
            let Some(left) = self
                .by_trigger
                .retain_own(&trigger, |held| !is_dead(held.dependency.entry))
            else {
                continue;
            };
            let sibling_left = self
                .sibling_by_trigger
                .retain_own(&trigger, |held| !is_dead(held.dependency.entry));
            if sibling_left == Some(0) {
                self.sibling_by_trigger.remove_own(&trigger);
            }
            // NB: A query keeps its trigger, which names it by its number, with no dependencies.
            if left == 0 && !matches!(trigger, Trigger::Relative(_)) {
                self.by_trigger.remove_own(&trigger);
            }
        }
        if !dead_queries.is_empty() {
            let relative = &self.relative;
            for trigger in died_alone.in_relative_argument.keys() {
                let Some(queries) = self.in_relative_argument.get_mut(trigger) else {
                    continue;
                };
                queries.retain(|(query, _)| !is_dead(relative[*query as usize].entry));
                if queries.is_empty() {
                    self.in_relative_argument.remove(trigger);
                }
            }
        }
    }

    /// Whether a selector tests `trigger` itself.
    fn has_effects(&self, trigger: &Trigger) -> bool {
        self.by_trigger.contains_key(trigger) || self.lone_by_trigger.contains_key(trigger)
    }
}

/// What adding entries to a part takes beside what the part holds, for one extension.
struct PartBuilder<'a> {
    part: &'a mut InvalidationPart,
    /// The numbers of the compounds of the selector being collected, by where they start.
    interned: HashMap<ComponentOffset, u32>,
    /// The number of each kind of own tests of the compounds the extension took: the kinds of one
    /// arena's compounds are no other arena's.
    shapes: HashMap<(crate::css::selector_arena::SelectorArenaSerial, u32), u32>,
    /// The number of each chain the extension took, by the compound's kind of own tests, the
    /// combinator left of it and the chain of its left compound.
    chains: HashMap<(u32, ReachCombinator, u32), u32>,
    /// The selector of the entry being collected.
    collecting_entry_selector: Option<crate::css::selector::SelectorRef>,
    /// The selector being collected, from the compound it ends in, with whether its rule is inside
    /// `@scope`.
    collecting_selector: Option<(crate::css::selector::SelectorRef, bool)>,
    /// What the selector being collected leads on to, where that is no compound of its rule's.
    collecting_end: Option<ChainEnd>,
    /// The bound the selector being collected is, by its place among the bounds of its `@scope`
    /// levels, where it is one.
    collecting_bound: Option<u16>,
    /// The triggers dependencies were added for.
    touched: Vec<Trigger>,
    /// The attribute names the triggers name.
    names: AcquiredNames,
    /// The entry whose selector is being collected.
    collecting: u32,
}

impl<'a> PartBuilder<'a> {
    fn new(part: &'a mut InvalidationPart) -> Self {
        Self {
            part,
            interned: HashMap::default(),
            shapes: HashMap::default(),
            chains: HashMap::default(),
            collecting_entry_selector: None,
            collecting_selector: None,
            collecting_end: None,
            collecting_bound: None,
            touched: Vec::new(),
            names: AcquiredNames::default(),
            collecting: 0,
        }
    }

    /// Collect the selector of `entry`, and the `@scope` levels of its rule.
    fn collect_entry(
        &mut self,
        entry: u32,
        selector: EntrySelector<'_>,
        selectors: &crate::css::selector::SelectorListRef,
    ) {
        self.collecting = entry;
        self.interned.clear();
        let scoped = selector.scope().is_some();
        let selector_ref = selectors.selector_ref_at(selector.selector.offset());
        self.collecting_entry_selector = Some(selector_ref.clone());
        // https://drafts.csswg.org/css-cascade-6/#scope-atrule
        // Which elements are in the scope depends on what its roots and limits match, a change to
        // which restyles the subtree of the element matching them for the rule: a bound is followed
        // as the left of a selector, up to that subtree.
        if let Some(scope) = selector.scope() {
            for (place, bound) in scope.roots.iter().chain(&scope.limits).enumerate() {
                self.collect_bound(entry, bound, u16::try_from(place).expect("scope bound space exhausted"));
            }
            self.interned.clear();
        }
        self.collecting_selector = Some((selector_ref, scoped));
        if let Some(trigger) = self.lone_trigger(selector) {
            if !self.part.lone_entries_in_buckets {
                self.touched.push(trigger);
                self.part.dependencies += 1;
                self.part.lone_by_trigger.push(trigger, entry);
            }
            return;
        }
        self.collect_chain(entry, selector.selector, false);
    }

    /// Collect `bound`, a root or limit of the `@scope` levels of `entry`, at `place` among them.
    fn collect_bound(&mut self, entry: u32, bound: &crate::css::selector::SelectorRef, place: u16) {
        self.interned.clear();
        self.collecting_selector = Some((bound.clone(), true));
        self.collecting_bound = Some(place);
        self.collecting_end = Some(ChainEnd::Subtree);
        self.part.bound_runs.push((self.next_compound_number(), place));
        self.collect_chain(entry, bound.view(), false);
        self.collecting_bound = None;
        self.collecting_end = None;
    }

    /// Collect `selector`, a compound of the selector being collected from which leftwards a chain
    /// of its own starts, which leads on to `end`.
    fn collect_chain_ending_in(&mut self, entry: u32, selector: Selector<'_>, end: ChainEnd) {
        let (root, scoped) = self
            .collecting_selector
            .clone()
            .expect("a chain is taken while its selector is collected");
        let outer_selector = self
            .collecting_selector
            .replace((root.at_compound(selector.offset()), scoped));
        let outer_end = self.collecting_end.replace(end);
        self.collect_chain(entry, selector, false);
        self.collecting_selector = outer_selector;
        self.collecting_end = outer_end;
    }

    fn next_compound_number(&self) -> u32 {
        u32::try_from(self.part.compounds.len()).expect("invalidation map space exhausted")
    }

    /// The compound the selector being collected ends in where it leads on to no compound of its
    /// rule's, or `NO_COMPOUND`.
    fn compound_of_collecting_end(&mut self, offset: ComponentOffset) -> u32 {
        let Some(end) = self.collecting_end else {
            return NO_COMPOUND;
        };
        if let Some(local) = self.part.chain_ends[end as usize] {
            return local;
        }
        let local = self.next_compound_number();
        assert!(local <= LOCAL_MASK, "invalidation map space exhausted");
        self.part.chain_ends[end as usize] = Some(local);
        // NB: No fact names the compound, whose place is read by none.
        self.part.compounds.push(HeldCompound {
            selector: self
                .collecting_entry_selector
                .clone()
                .expect("a compound is taken while its entry is collected"),
            offset,
            next: NO_COMPOUND,
            left: NO_COMPOUND,
            required_key: None,
            shape: HeldCompound::NO_SHAPE,
            chain: HeldCompound::NO_SHAPE,
            operand_names: 0,
            operand_name_count: 0,
            combinator: match end {
                ChainEnd::Subtree => ReachCombinator::Subtree,
                ChainEnd::ParentSubtree => ReachCombinator::ParentSubtree,
            },
            flags: 0,
        });
        local
    }

    /// The fact the selector of an entry is the one name of, with a pseudo-element at most: an id
    /// or a class its bucket's key proves, outside `@scope`.
    fn lone_trigger(&mut self, selector: EntrySelector<'_>) -> Option<Trigger> {
        let root = selector.selector;
        if selector.scope().is_some() || !proves_the_whole_selector(root) {
            return None;
        }
        let subject = super::super::selector_analysis::subject_compound(root).first_compound();
        let simple = subject.simple_selectors.iter().next()?;
        if !matches!(&simple, SimpleSelector::Class(_) | SimpleSelector::Id(_)) {
            return None;
        }
        match own_triggers(&simple, &mut self.names)[..] {
            [trigger] => Some(trigger),
            _ => None,
        }
    }

    fn add(&mut self, trigger: Trigger, collecting: Collecting) {
        self.touched.push(trigger);
        self.part.dependencies += 1;
        let held = HeldDependency {
            dependency: collecting.dependency,
            ancestor_hashes: collecting.ancestor_hashes,
        };
        if collecting.follows_siblings {
            self.part.sibling_by_trigger.push(trigger, held);
        }
        self.part.by_trigger.push(trigger, held);
    }

    /// Number the chain of the compound numbered `local`, from its left compound's.
    fn number_chain(&mut self, local: u32) -> u32 {
        let held = &self.part.compounds[local as usize];
        if held.chain != HeldCompound::CHAIN_PENDING {
            return held.chain;
        }
        let (shape, combinator, left, scoped) =
            (held.shape, held.combinator, held.left, held.has(HeldCompound::SCOPED));
        let left_chain = match left {
            NO_COMPOUND => NO_COMPOUND,
            left => self.number_chain(left),
        };
        // NB: A compound with tests of its own selector, or of a rule inside `@scope`, shares
        //     nothing, nor does one left of which a compound does.
        let chain = match () {
            _ if shape == HeldCompound::NO_SHAPE || scoped || left_chain == HeldCompound::NO_SHAPE => {
                HeldCompound::NO_SHAPE
            }
            _ => *self.chains.entry((shape, combinator, left_chain)).or_insert_with(|| {
                self.part.chain_count += 1;
                self.part.chain_count - 1
            }),
        };
        self.part.compounds[local as usize].chain = chain;
        chain
    }

    /// The number of the compound of the selector being collected starting at `offset`, which the
    /// map holds from here on with the compounds it leads to and from.
    fn intern(&mut self, offset: ComponentOffset) -> CompoundID {
        if let Some(&local) = self.interned.get(&offset) {
            return CompoundID(local);
        }
        let (root, scoped) = self
            .collecting_selector
            .clone()
            .expect("a compound is taken while its selector is collected");
        let root_selector = root.view();
        let compound = root_selector.at_compound(offset);
        let first = compound.first_compound();
        let next = right_of(root_selector, compound).map(|right| right.offset());
        let left = combinator_left_of(compound).map(|(combinator, left)| (combinator, left.offset()));
        let mut flags = 0;
        if next.is_none() && self.collecting_end.is_none() && matches_every_element(&first) {
            flags |= HeldCompound::REACHES_EVERY_SUBJECT;
        }
        if self.collecting_bound.is_some() {
            flags |= HeldCompound::IN_BOUND;
        }
        if reads_relative(compound) {
            flags |= HeldCompound::READS_RELATIVE;
        }
        if scoped {
            flags |= HeldCompound::SCOPED;
        }
        let local = u32::try_from(self.part.compounds.len()).expect("invalidation map space exhausted");
        assert!(local <= LOCAL_MASK, "invalidation map space exhausted");
        let required_key = required_key(&first).filter(is_probed);
        let shape = compound.own_tests().map_or(HeldCompound::NO_SHAPE, |own_tests| {
            *self.shapes.entry(own_tests).or_insert_with(|| {
                self.part.shape_count += 1;
                self.part.shape_count - 1
            })
        });
        let names = OperandName::all_of(&first, compound.namespaces().default.is_some()).unwrap_or_default();
        let operand_name_count = u8::try_from(names.len()).expect("a compound holds few enough names");
        let operand_names = match names[..] {
            [OperandName::Class(name)] => {
                flags |= HeldCompound::LONE_CLASS;
                name.0
            }
            [OperandName::Id(name)] => {
                flags |= HeldCompound::LONE_ID;
                name.0
            }
            [OperandName::Tag(name)] => name.0,
            _ => {
                let start = u32::try_from(self.part.operand_names.len()).expect("invalidation map space exhausted");
                for name in names {
                    self.part.operand_names.push(name);
                }
                start
            }
        };
        self.part.compounds.push(HeldCompound {
            selector: self
                .collecting_entry_selector
                .clone()
                .expect("a compound is taken while its entry is collected"),
            offset,
            next: NO_COMPOUND,
            left: NO_COMPOUND,
            required_key,
            shape,
            // NB: The compounds of a `@scope` bound leave the interned ones before chains are numbered, and share none.
            chain: match scoped {
                true => HeldCompound::NO_SHAPE,
                false => HeldCompound::CHAIN_PENDING,
            },
            operand_names,
            operand_name_count,
            combinator: left.map_or(ReachCombinator::Descendant, |(combinator, _)| combinator),
            flags,
        });
        self.interned.insert(offset, local);
        let next = match next {
            Some(next) => self.intern(next).0,
            None => self.compound_of_collecting_end(offset),
        };
        let left = left.map_or(NO_COMPOUND, |(_, left)| self.intern(left).0);
        let held = &mut self.part.compounds[local as usize];
        (held.next, held.left) = (next, left);
        CompoundID(local)
    }

    /// Collect the compounds of `selector`, of `entry`, from its subject leftwards, the subject's
    /// leading on to a sibling combinator where `subject_follows_siblings`.
    fn collect_chain(&mut self, entry: u32, selector: Selector<'_>, subject_follows_siblings: bool) {
        let mut compound = selector;
        let mut follows_siblings = subject_follows_siblings;
        // Whether a compound right of the one being collected is one an element stands for.
        let mut has_right = false;
        loop {
            if compound.is_relative_anchor() {
                break;
            }
            let first = compound.first_compound();
            if !is_transparent(&first) {
                let collecting = Collecting {
                    dependency: Dependency {
                        entry,
                        compound: self.intern(compound.offset()),
                    },
                    ancestor_hashes: super::bloom::AncestorHashes::required_left_of(compound),
                    follows_siblings,
                    position_key: match required_key(&first) {
                        Some(DispatchKey::Class(class)) => PositionKey::Class(class),
                        Some(DispatchKey::Id(id)) => PositionKey::Id(id),
                        Some(DispatchKey::TagName(tag)) => PositionKey::Tag(tag),
                        _ => PositionKey::Any,
                    },
                };
                // An element arriving or leaving, or another becoming an element's previous
                // sibling, is what changes for a compound left of a combinator testing no fact.
                if has_right && compound_keys(&first, &mut self.names).is_none() {
                    self.add(Trigger::Any, collecting);
                }
                self.collect_compound(entry, first.simple_selectors, collecting);
                has_right = true;
            }
            let Some((relation, left)) = compound.left() else {
                break;
            };
            match relation {
                // A compound only some alternatives of which name the host reaches out of the
                // shadow tree to it or not: a change to a fact it tests restyles the subtree of the
                // element carrying it, which holds the children of the host in its shadow tree.
                Combinator::ImmediateChild | Combinator::Descendant if host_step(left).is_none() => {
                    self.collect_chain_ending_in(entry, left, ChainEnd::Subtree);
                    break;
                }
                // FIXME: A column combinator matches nothing yet, whatever its compounds test.
                Combinator::Column => break,
                _ => {}
            }
            follows_siblings = matches!(relation, Combinator::NextSibling | Combinator::SubsequentSibling);
            compound = left;
        }
    }

    /// Collect the facts `simple_selectors`, of one compound of `entry`, test, under the
    /// dependency `collecting` says.
    fn collect_compound(
        &mut self,
        entry: u32,
        simple_selectors: crate::css::selector_arena::SimpleSelectors<'_>,
        collecting: Collecting,
    ) {
        for simple in simple_selectors.iter() {
            let simple = &simple;
            for trigger in own_triggers(simple, &mut self.names) {
                self.add(trigger, collecting);
            }
            match simple {
                SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
                    PseudoClassType::Is | PseudoClassType::Where | PseudoClassType::Not => {
                        for selector in pseudo_class.argument_selector_list {
                            self.collect_nested(entry, selector, collecting);
                        }
                    }
                    // The host a `:host()` compound describes is the element the compound is of.
                    PseudoClassType::Host => {
                        for selector in pseudo_class.argument_selector_list {
                            self.collect_compound(entry, selector.first_compound().simple_selectors, collecting);
                        }
                    }
                    PseudoClassType::Has => {
                        for argument in pseudo_class.argument_selector_list {
                            let index =
                                u32::try_from(self.part.relative.len()).expect("relative query space exhausted");
                            let in_shadow_tree = simple_selectors.iter().any(|simple| {
                                matches!(simple, SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Host)
                            });
                            self.part.relative.push(RelativeEntry::new(
                                entry,
                                argument,
                                self.collecting_bound,
                                in_shadow_tree,
                                &mut self.names,
                            ));
                            self.add(Trigger::Relative(index), collecting);
                            self.collect_argument(argument, (index, false), false);
                        }
                    }
                    _ => {
                        let (positions, of) = positions_read(pseudo_class);
                        for (from_end, of_type) in positions {
                            self.part.reads_positions.note(from_end, of_type);
                            self.add(
                                Trigger::Position {
                                    from_end,
                                    of_type,
                                    key: collecting.position_key,
                                },
                                collecting,
                            );
                        }
                        if of.is_some() {
                            self.part.reads_positions.note_counting_by_selector();
                        }
                        // Which siblings an `of <selector>` counts depends on facts of each of them.
                        for selector in of.into_iter().flatten() {
                            self.collect_chain_ending_in(entry, selector, ChainEnd::ParentSubtree);
                        }
                    }
                },
                // The slotted element a `::slotted()` compound describes is the element the
                // compound is of.
                SimpleSelector::PseudoElement(PseudoElementSelector {
                    value: PseudoElementValue::CompoundSelector(argument),
                    ..
                }) => self.collect_compound(entry, argument.first_compound().simple_selectors, collecting),
                SimpleSelector::Nesting(Nesting::Rule(parent)) => {
                    for selector in *parent {
                        self.collect_nested(entry, selector, collecting);
                    }
                }
                _ => {}
            }
        }
    }

    /// Collect `selector`, nested in the compound `collecting` says. A selector with combinators
    /// of its own is a chain of its own, whose subject is the element of that compound and leads
    /// on to what that compound does: what its combinators reach is tested against its own subject
    /// compound, whatever the rest of the compound, which a `:not()` around it answers the other
    /// way, tests.
    ///
    /// NB: A selector nested in `:is()` or `:not()` continues with the dependency of the compound
    ///     it is nested in.
    fn collect_nested(&mut self, entry: u32, selector: Selector<'_>, collecting: Collecting) {
        match selector.left() {
            None => self.collect_compound(entry, selector.first_compound().simple_selectors, collecting),
            Some(_) => self.collect_chain(entry, selector, collecting.follows_siblings),
        }
    }

    /// Note the facts the relative selector `argument` of the query `query` tests, where `nested`
    /// says whether it is a selector nested in the argument, and the second half of `query`
    /// whether it is of a compound such a selector leads to from its subject.
    fn collect_argument(&mut self, argument: Selector<'_>, query: (u32, bool), nested: bool) {
        let mut compound = argument;
        let mut current_query = query;
        loop {
            if compound.is_relative_anchor() {
                return;
            }
            let first = compound.first_compound();
            for simple in first.simple_selectors.iter() {
                let simple = &simple;
                self.collect_argument_simple(simple, current_query, nested);
            }
            let Some((relation, left)) = compound.left() else {
                return;
            };
            match relation {
                // A compound left of a step is of an element anywhere in the tree, but where the
                // step is of the argument's own chain, which leads to the anchor.
                Combinator::ImmediateChild
                | Combinator::Descendant
                | Combinator::NextSibling
                | Combinator::SubsequentSibling => {
                    current_query = (query.0, current_query.1 || nested);
                    self.part.relative[query.0 as usize].nested_reaches_out |= current_query.1;
                }
                Combinator::PseudoElement if is_transparent(&first) => {}
                // A slot, the host exposing a part, or the host of a backing element.
                _ => {
                    self.part.relative[query.0 as usize].nested_reaches_out = true;
                    current_query = (query.0, true);
                }
            }
            compound = left;
        }
    }

    fn collect_argument_simple(&mut self, simple: &SimpleSelector<'_>, query: (u32, bool), nested: bool) {
        for trigger in own_triggers(simple, &mut self.names) {
            // A tag name only changes as an element arrives, which the arrival keys answer for.
            if matches!(trigger, Trigger::Tag(_)) {
                continue;
            }
            let queries = self.part.in_relative_argument.entry(trigger).or_default();
            if !queries.contains(&query) {
                queries.push(query);
            }
        }
        match simple {
            SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
                PseudoClassType::Is | PseudoClassType::Where | PseudoClassType::Not => {
                    for selector in pseudo_class.argument_selector_list {
                        self.collect_argument(selector, query, true);
                    }
                }
                PseudoClassType::Host => {
                    for selector in pseudo_class.argument_selector_list {
                        self.collect_argument(selector, query, nested);
                    }
                }
                // NB: `:has()` takes no `:has()`, which does not parse.
                PseudoClassType::Has => {}
                // The siblings an `of <selector>` counts are other elements than the one it is of.
                _ => {
                    if let Some(of) = positions_read(pseudo_class).1 {
                        self.part.relative[query.0 as usize].nested_reaches_out = true;
                        for selector in of {
                            self.collect_argument(selector, (query.0, true), true);
                        }
                    }
                }
            },
            SimpleSelector::PseudoElement(PseudoElementSelector {
                value: PseudoElementValue::CompoundSelector(argument),
                ..
            }) => self.collect_argument(*argument, query, nested),
            SimpleSelector::Nesting(Nesting::Rule(parent)) => {
                for selector in *parent {
                    self.collect_argument(selector, query, true);
                }
            }
            _ => {}
        }
    }
}
