/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Parsed selectors, as 8-byte components in one immutable arena per parsed list.
//!
//! Every selector of a list, nested argument lists included, is a run of [`Component`]s in
//! matching order: the subject compound first, and inside a compound the simple selectors in the
//! order they were written. The last component of each compound carries the [`Relation`] to the
//! compound left of it. A nested list is stored before the selector that names it, so runs never
//! interleave. Payloads that do not fit a component's 32-bit data go in typed side pools.
//!
//! Names are process style atoms, which the arena leases once each. The arena is `Send` and
//! `Sync`: it holds atoms, indices and text only.
//!
//! Readers see the components through borrowed views ([`Selector`], [`CompoundRef`],
//! [`SimpleSelector`]) that decode them on the fly.

use smallvec::SmallVec;
use std::sync::Arc;

use crate::css::selector::{
    AnPlusBPattern, AttributeCaseType, AttributeMatchType, Combinator, Direction, NameSelector, NamespaceType,
    PseudoClassParameterType, PseudoClassType, PseudoElementType, QualifiedName, SearchTextMatchFilter, Specificity,
    pseudo_class_from_ffi,
};
use crate::css::selector_tree as tree;
use crate::css::style::AtomLeases;
use crate::css::style::index::StyleAtomID;

/// The number a selector arena takes as it is frozen, which no other arena of the process ever
/// takes: what a cache keyed by a selector names its arena by, where an address could be reused
/// by the arena of a later parse.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct SelectorArenaSerial(u64);

impl SelectorArenaSerial {
    /// The next serial of the process, from `next`, the counter of its process style.
    pub(crate) fn next(next: &std::sync::atomic::AtomicU64) -> Self {
        Self(next.fetch_add(1, std::sync::atomic::Ordering::Relaxed))
    }
}

/// Where a selector list starts in its arena.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct ListStart(u32);

/// A component's position in its arena.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct ComponentOffset(u32);

impl ComponentOffset {
    /// Where the page of `1 << page_bits` components the component is in starts, and where it is
    /// in that page.
    #[must_use]
    pub(crate) fn in_page(self, page_bits: u32) -> (Self, u32) {
        let mask = (1 << page_bits) - 1;
        (Self(self.0 & !mask), self.0 & mask)
    }

    /// The component `offset` components into the page starting at `self`.
    #[must_use]
    pub(crate) fn into_page(self, offset: u32) -> Self {
        Self(self.0 + offset)
    }
}

/// A style rule's place among the style rules whose selectors an arena holds: the rule a `&`
/// names, through the arena's rule lists.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct RuleOrdinal(u32);

/// What a `&` names.
///
/// https://drafts.csswg.org/css-nesting/#nest-selector
/// The nesting selector represents the elements matched by the parent rule. When the parent rule
/// is a style rule, it represents the elements matched by that rule's selector list; inside a
/// scoped rule it represents the scoping root, and anywhere else the root of the tree.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Nesting<Rule> {
    /// The root of the tree, or the scoping root where a scope is bound.
    #[default]
    Root,
    /// The scoping root of the `@scope` the rule is directly in.
    Scope,
    /// The selectors of the style rule the rule is nested in, in the same arena.
    Rule(Rule),
    /// A style rule the parse did not see, as when the CSSOM parses a nested rule's selectors
    /// before it places the rule. Placing it binds the `&` to its parent.
    Unbound,
}

/// What a `&` names, as the parse that wrote it knew.
pub(crate) type NestingTarget = Nesting<RuleOrdinal>;

/// What a `&` in an arena names, decoded: the selectors of the parent style rule are `:is()` of them.
pub type NestingOfArena<'a> = Nesting<SelectorListView<'a>>;

const NESTING_ROOT: u8 = 0;
const NESTING_SCOPE: u8 = 1;
const NESTING_RULE: u8 = 2;
const NESTING_UNBOUND: u8 = 3;

impl NestingTarget {
    fn kind(self) -> u8 {
        match self {
            Self::Root => NESTING_ROOT,
            Self::Scope => NESTING_SCOPE,
            Self::Rule(_) => NESTING_RULE,
            Self::Unbound => NESTING_UNBOUND,
        }
    }
}

/// A run of entries in one of an arena's pools.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct PoolRange {
    start: u32,
    length: u32,
}

impl PoolRange {
    fn of<T>(pool: &[T], range: PoolRange) -> &[T] {
        &pool[range.start as usize..(range.start + range.length) as usize]
    }
}

fn index_u32(index: usize) -> u32 {
    u32::try_from(index).expect("a selector list holds fewer than 2^32 parts")
}

/// What a component is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
enum ComponentKind {
    Universal,
    Tag,
    Id,
    Class,
    Attribute,
    PseudoClass,
    PseudoElement,
    Nesting,
    Invalid,
    /// The implicit leftmost compound of a selector that starts with a combinator.
    RelativeAnchor,
}

/// How the compound a component ends relates to the compound left of it, or `SubSelector` for a
/// component that does not end its compound.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum Relation {
    SubSelector,
    Descendant,
    Child,
    NextSibling,
    SubsequentSibling,
    Column,
    PseudoElement,
    /// The component ends the leftmost compound, and so the complex selector.
    Leftmost,
}

impl Relation {
    fn of(combinator: Combinator) -> Self {
        match combinator {
            Combinator::None => Self::Leftmost,
            Combinator::Descendant => Self::Descendant,
            Combinator::ImmediateChild => Self::Child,
            Combinator::NextSibling => Self::NextSibling,
            Combinator::SubsequentSibling => Self::SubsequentSibling,
            Combinator::Column => Self::Column,
            Combinator::PseudoElement => Self::PseudoElement,
        }
    }

    fn combinator(self) -> Combinator {
        match self {
            Self::Descendant => Combinator::Descendant,
            Self::Child => Combinator::ImmediateChild,
            Self::NextSibling => Combinator::NextSibling,
            Self::SubsequentSibling => Combinator::SubsequentSibling,
            Self::Column => Combinator::Column,
            Self::PseudoElement => Combinator::PseudoElement,
            Self::SubSelector | Self::Leftmost => Combinator::None,
        }
    }
}

const KIND_BITS: u32 = 0x0000_000f;
const RELATION_SHIFT: u32 = 4;
const RELATION_BITS: u32 = 0x0000_00f0;
const PSEUDO_SHIFT: u32 = 8;
const PSEUDO_BITS: u32 = 0x0000_ff00;
const LAST_IN_LIST: u32 = 1 << 16;
const IMPLICIT: u32 = 1 << 17;
const NAMESPACE_SHIFT: u32 = 18;
const NAMESPACE_BITS: u32 = 0b11 << NAMESPACE_SHIFT;
/// On a `:not()` component: its argument is `:current` alone, which picks the kind of search text match an entry
/// styles rather than constraining any element.
const SEARCH_TEXT_FILTER: u32 = 1 << 20;
/// On the first component of a selector: its subject compound holds no pseudo-element, `::part()` or `::slotted()`,
/// so that it describes the element it is matched against from its subject on.
const DESCRIBES_ITS_SUBJECT: u32 = 1 << 21;
const INLINE: u32 = 1 << 22;
const FORGIVING: u32 = 1 << 23;
/// The name of a type, id, class or attribute selector is its own ASCII-lowercase form.
const ASCII_LOWERCASE: u32 = 1 << 24;
/// An attribute selector names one of the attributes whose values HTML compares ASCII case-insensitively.
const LEGACY_CASE_INSENSITIVE: u32 = 1 << 25;
/// On the first component of a selector: the selector names the scoping root, with `:scope` or a `&` directly inside
/// `@scope`, anywhere in it.
const NAMES_THE_SCOPE: u32 = 1 << 26;
/// On the first component of a selector: the selector names an element in another tree, with `::slotted()` or
/// `::part()`, where the sheet's default namespace does not follow it.
const ESCAPES_ITS_NAMESPACE: u32 = 1 << 27;
/// On the first component of a compound: how many components the compound has, where it has fewer than 16, so that a
/// walk steps from a compound to the next one without reading the components between; zero for a longer compound,
/// which a walk reads to its end.
const COMPOUND_LENGTH_SHIFT: u32 = 28;
const COMPOUND_LENGTH_BITS: u32 = 0xf << COMPOUND_LENGTH_SHIFT;

/// The kind, relation and flags of a component, in 32 bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(transparent)]
struct ComponentBits(u32);

impl ComponentBits {
    fn new(kind: ComponentKind) -> Self {
        Self(kind as u32)
    }

    fn kind(self) -> ComponentKind {
        const KINDS: [ComponentKind; 10] = [
            ComponentKind::Universal,
            ComponentKind::Tag,
            ComponentKind::Id,
            ComponentKind::Class,
            ComponentKind::Attribute,
            ComponentKind::PseudoClass,
            ComponentKind::PseudoElement,
            ComponentKind::Nesting,
            ComponentKind::Invalid,
            ComponentKind::RelativeAnchor,
        ];
        KINDS[(self.0 & KIND_BITS) as usize]
    }

    fn relation(self) -> Relation {
        const RELATIONS: [Relation; 8] = [
            Relation::SubSelector,
            Relation::Descendant,
            Relation::Child,
            Relation::NextSibling,
            Relation::SubsequentSibling,
            Relation::Column,
            Relation::PseudoElement,
            Relation::Leftmost,
        ];
        RELATIONS[((self.0 & RELATION_BITS) >> RELATION_SHIFT) as usize]
    }

    fn with_relation(self, relation: Relation) -> Self {
        Self((self.0 & !RELATION_BITS) | ((relation as u32) << RELATION_SHIFT))
    }

    fn pseudo(self) -> u8 {
        ((self.0 & PSEUDO_BITS) >> PSEUDO_SHIFT) as u8
    }

    fn with_pseudo(self, pseudo: u8) -> Self {
        Self((self.0 & !PSEUDO_BITS) | (u32::from(pseudo) << PSEUDO_SHIFT))
    }

    fn namespace(self) -> NamespaceType {
        const TYPES: [NamespaceType; 4] = [
            NamespaceType::Default,
            NamespaceType::None,
            NamespaceType::Any,
            NamespaceType::Named,
        ];
        TYPES[((self.0 & NAMESPACE_BITS) >> NAMESPACE_SHIFT) as usize]
    }

    fn with_namespace(self, namespace: NamespaceType) -> Self {
        Self((self.0 & !NAMESPACE_BITS) | ((namespace as u32) << NAMESPACE_SHIFT))
    }

    fn with(self, flag: u32, set: bool) -> Self {
        Self(if set { self.0 | flag } else { self.0 & !flag })
    }

    fn has(self, flag: u32) -> bool {
        self.0 & flag != 0
    }
}

/// One simple selector of a complex selector. The last component of each compound carries the
/// relation to the compound left of it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(C)]
struct Component {
    bits: ComponentBits,
    /// What `bits` says it is: an atom, a list start, an inline `an+b`, or an index in a pool.
    data: u32,
}
const _: () = assert!(size_of::<Component>() == 8);

/// No list, no payload.
const NONE: u32 = u32::MAX;

/// A name written with an explicit namespace prefix.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct QualifiedRare {
    prefix: StyleAtomID,
    local: StyleAtomID,
}

/// `[x op v]`. The operator and case live in the component's pseudo bits, the namespace in its
/// namespace bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct AttributeRare {
    name: QualifiedRare,
    value: StyleAtomID,
    text: PoolRange,
}

/// `:nth-*()` outside the inline range, or with `of S`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct NthRare {
    a: i32,
    b: i32,
    of: u32,
}

/// The arguments of a pseudo-class that takes something other than selectors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ArgumentRare {
    /// `:dir()`, `:state()` and the other pseudo-classes taking one identifier.
    Ident {
        text: PoolRange,
        atom: StyleAtomID,
        direction: Option<Direction>,
    },
    /// `:lang()`: ranges into the arena's text ranges; a range with its top length bit set was written as a string.
    Languages { ranges: PoolRange },
    /// `:heading()`: levels into the arena's numbers.
    Levels { levels: PoolRange },
}

const STRING_BIT: u32 = 1 << 31;

/// What a pseudo-element names beside its kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct PseudoElementRare {
    serialized_name: Option<PoolRange>,
    value: PseudoElementRareValue,
    identifier_identities: PoolRange,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PseudoElementRareValue {
    None,
    CompoundSelector(ListStart),
    /// Ranges into the arena's text ranges.
    Identifiers(PoolRange),
    TransitionName {
        /// None for no name, `Some(None)` for `*`.
        name: Option<Option<PoolRange>>,
        /// Ranges into the arena's text ranges.
        classes: PoolRange,
    },
}

/// The `@namespace` declarations of the sheet an arena's selectors are of, as process atoms.
///
/// A prefix means whatever its sheet says it means, and an unprefixed type or universal selector
/// means the sheet's default namespace where one is declared.
#[derive(Clone, Copy, Debug, Default)]
pub struct SheetNamespaces<'a> {
    /// The sheet's default namespace, or `None` when it declared none - in which case an
    /// unprefixed name constrains no namespace at all.
    pub default: Option<StyleAtomID>,
    /// Each declared prefix followed by the namespace it names.
    prefixes: &'a [StyleAtomID],
}

impl<'a> SheetNamespaces<'a> {
    /// The declarations of a default namespace, where `default` is one, and of the prefixes
    /// `prefixes` holds, each followed by the namespace it names.
    #[must_use]
    pub fn new(default: Option<StyleAtomID>, prefixes: &'a [StyleAtomID]) -> Self {
        debug_assert!(
            prefixes.len().is_multiple_of(2),
            "a prefix is followed by its namespace"
        );
        Self { default, prefixes }
    }

    /// The namespace `prefix` names, where the sheet declares it.
    #[must_use]
    pub fn namespace_of(self, prefix: StyleAtomID) -> Option<StyleAtomID> {
        self.prefixes
            .as_chunks::<2>()
            .0
            .iter()
            .find(|declared| declared[0] == prefix)
            .map(|declared| declared[1])
    }
}

/// The `@namespace` declarations of a sheet, with the leases keeping the names they intern live
/// until an arena leases them.
#[derive(Default)]
pub(crate) struct NamespaceDeclarations {
    default: Option<StyleAtomID>,
    /// Each declared prefix followed by the namespace it names.
    prefixes: Vec<StyleAtomID>,
    names: Vec<crate::css::style::AtomLease>,
}

impl NamespaceDeclarations {
    /// The namespaces `declared` declares as prefix and namespace texts, an empty prefix declaring
    /// the default namespace, and a later declaration of a prefix replacing an earlier one.
    pub(crate) fn of_texts<'a>(declared: impl IntoIterator<Item = (&'a [u16], &'a [u16])>) -> Self {
        let mut declarations = Self::default();
        for (prefix, uri) in declared {
            declarations.declare(prefix, uri);
        }
        declarations
    }

    fn intern(&mut self, text: &[u16]) -> StyleAtomID {
        let lease = crate::css::style::process_atoms().intern(text);
        let atom = lease.atom();
        self.names.push(lease);
        atom
    }

    /// Declare `prefix` to name `uri`, an empty prefix declaring the default namespace.
    fn declare(&mut self, prefix: &[u16], uri: &[u16]) {
        let uri = match uri.is_empty() {
            true => StyleAtomID::NONE,
            false => self.intern(uri),
        };
        if prefix.is_empty() {
            self.default = Some(uri);
            return;
        }
        let prefix = self.intern(prefix);
        match self
            .prefixes
            .as_chunks_mut::<2>()
            .0
            .iter_mut()
            .find(|declared| declared[0] == prefix)
        {
            Some(declared) => declared[1] = uri,
            None => self.prefixes.extend([prefix, uri]),
        }
    }
}

/// The selectors of one parsed list and the lists nested in it. Immutable once built, and shared
/// by every document and thread that reaches the list.
#[derive(Debug)]
pub(crate) struct SelectorArena {
    components: Box<[Component]>,
    qualified: Box<[QualifiedRare]>,
    attributes: Box<[AttributeRare]>,
    nths: Box<[NthRare]>,
    arguments: Box<[ArgumentRare]>,
    pseudo_elements: Box<[PseudoElementRare]>,
    atoms: Box<[StyleAtomID]>,
    numbers: Box<[i64]>,
    text_ranges: Box<[PoolRange]>,
    text: Box<[u16]>,
    /// Where the selectors of each style rule the arena holds start, by the rule's ordinal: what a
    /// `&` naming the rule reads.
    rule_lists: Box<[ListStart]>,
    /// The `@namespace` declarations of the sheet the selectors are of: the default namespace, and
    /// the prefixes, each followed by the namespace it names, in `atoms`.
    default_namespace: Option<StyleAtomID>,
    namespace_prefixes: PoolRange,
    serial: SelectorArenaSerial,
    /// What matching asks of each compound as a whole, by where the compound starts, in the order of the
    /// components.
    compounds: Box<[CompoundFacts]>,
    /// Which components start a compound, a bit each, which [`Self::compounds`] is numbered by.
    compound_starts: Box<[u64]>,
    /// How many compounds start before each word of `compound_starts`, so that a compound's facts
    /// are found from where it starts without a search.
    compound_ranks: Box<[u32]>,
    _leases: AtomLeases,
}

const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<SelectorArena>();
};

impl SelectorArena {
    fn text(&self, range: PoolRange) -> &[u16] {
        PoolRange::of(&self.text, range)
    }

    fn selectors_of(&self, start: Option<ListStart>) -> SelectorsOf<'_> {
        SelectorsOf {
            arena: self,
            at: start.map_or(usize::MAX, |start| start.0 as usize),
        }
    }

    fn decode(&self, at: usize) -> SimpleSelector<'_> {
        let Component { bits, data } = self.components[at];
        let qualified = |namespace: NamespaceType| -> QualifiedName {
            match namespace {
                NamespaceType::Named => {
                    let rare = self.qualified[data as usize];
                    QualifiedName {
                        namespace_type: namespace,
                        namespace: rare.prefix,
                        name: rare.local,
                        name_is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
                    }
                }
                _ => QualifiedName {
                    namespace_type: namespace,
                    namespace: StyleAtomID::NONE,
                    name: StyleAtomID(data),
                    name_is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
                },
            }
        };
        match bits.kind() {
            ComponentKind::Universal => SimpleSelector::Universal(qualified(bits.namespace())),
            ComponentKind::Tag => SimpleSelector::TagName(qualified(bits.namespace())),
            ComponentKind::Id => SimpleSelector::Id(NameSelector {
                name: StyleAtomID(data),
                is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
            }),
            ComponentKind::Class => SimpleSelector::Class(NameSelector {
                name: StyleAtomID(data),
                is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
            }),
            ComponentKind::Attribute => {
                let rare = &self.attributes[data as usize];
                let namespace_type = bits.namespace();
                SimpleSelector::Attribute(AttributeSelector {
                    match_type: attribute_operator(bits.pseudo() & 0b111),
                    qualified_name: QualifiedName {
                        namespace_type,
                        namespace: rare.name.prefix,
                        name: rare.name.local,
                        name_is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
                    },
                    value: self.text(rare.text),
                    value_identity: rare.value,
                    case_type: attribute_case(bits.pseudo() >> 3),
                    names_a_legacy_case_insensitive_attribute: bits.has(LEGACY_CASE_INSENSITIVE),
                })
            }
            ComponentKind::PseudoClass => self.decode_pseudo_class(bits, data),
            ComponentKind::PseudoElement => self.decode_pseudo_element(bits, data),
            ComponentKind::Nesting => SimpleSelector::Nesting(match bits.pseudo() {
                NESTING_ROOT => Nesting::Root,
                NESTING_SCOPE => Nesting::Scope,
                NESTING_RULE => Nesting::Rule(SelectorListView {
                    arena: self,
                    start: Some(self.rule_lists[data as usize]),
                }),
                _ => Nesting::Unbound,
            }),
            ComponentKind::Invalid => SimpleSelector::Invalid(self.text(self.text_ranges[data as usize])),
            ComponentKind::RelativeAnchor => unreachable!("a relative anchor is not among a compound's selectors"),
        }
    }

    /// The list a component names by where it starts, or none by `NONE`.
    fn list_at(&self, data: u32) -> SelectorListView<'_> {
        SelectorListView {
            arena: self,
            start: (data != NONE).then_some(ListStart(data)),
        }
    }

    fn decode_pseudo_class(&self, bits: ComponentBits, data: u32) -> SimpleSelector<'_> {
        let pseudo_class = pseudo_class_from_ffi(bits.pseudo());
        let mut selector = PseudoClassSelector {
            pseudo_class,
            an_plus_b_pattern: AnPlusBPattern::default(),
            argument_selector_list: self.list_at(NONE),
            languages: Languages {
                arena: self,
                ranges: &[],
            },
            direction: None,
            identifier: None,
            identifier_identity: StyleAtomID::NONE,
            levels: &[],
            is_forgiving: bits.has(FORGIVING),
        };
        match pseudo_class.metadata().parameter_type {
            PseudoClassParameterType::AnPlusB | PseudoClassParameterType::AnPlusBOf => {
                if data != NONE {
                    let (a, b, of) = match bits.has(INLINE) {
                        true => (
                            i32::from((data >> 16) as u16 as i16),
                            i32::from(data as u16 as i16),
                            NONE,
                        ),
                        false => {
                            let nth = self.nths[data as usize];
                            (nth.a, nth.b, nth.of)
                        }
                    };
                    selector.an_plus_b_pattern = AnPlusBPattern {
                        step_size: a,
                        offset: b,
                    };
                    selector.argument_selector_list = self.list_at(of);
                }
            }
            PseudoClassParameterType::Ident
            | PseudoClassParameterType::LanguageRanges
            | PseudoClassParameterType::LevelList => {
                if data != NONE {
                    match self.arguments[data as usize] {
                        ArgumentRare::Ident { text, atom, direction } => {
                            selector.identifier = Some(self.text(text));
                            selector.identifier_identity = atom;
                            selector.direction = direction;
                        }
                        ArgumentRare::Languages { ranges } => {
                            selector.languages.ranges = PoolRange::of(&self.text_ranges, ranges);
                        }
                        ArgumentRare::Levels { levels } => {
                            selector.levels = PoolRange::of(&self.numbers, levels);
                        }
                    }
                }
            }
            PseudoClassParameterType::None => {}
            PseudoClassParameterType::CompoundSelector
            | PseudoClassParameterType::ForgivingSelectorList
            | PseudoClassParameterType::RelativeSelectorList
            | PseudoClassParameterType::SelectorList => selector.argument_selector_list = self.list_at(data),
        }
        SimpleSelector::PseudoClass(selector)
    }

    fn decode_pseudo_element(&self, bits: ComponentBits, data: u32) -> SimpleSelector<'_> {
        let pseudo_element = crate::css::selector::pseudo_element_from_ffi(bits.pseudo());
        let Some(rare) = (data != NONE).then(|| self.pseudo_elements[data as usize]) else {
            return SimpleSelector::PseudoElement(PseudoElementSelector {
                pseudo_element,
                serialized_name: None,
                value: PseudoElementValue::None,
                identifier_identities: &[],
            });
        };
        let texts = |range: PoolRange| TextList {
            arena: self,
            ranges: PoolRange::of(&self.text_ranges, range),
        };
        SimpleSelector::PseudoElement(PseudoElementSelector {
            pseudo_element,
            serialized_name: rare.serialized_name.map(|range| self.text(range)),
            value: match rare.value {
                PseudoElementRareValue::None => PseudoElementValue::None,
                PseudoElementRareValue::CompoundSelector(start) => PseudoElementValue::CompoundSelector(
                    self.selectors_of(Some(start)).next().expect("a list holds a selector"),
                ),
                PseudoElementRareValue::Identifiers(range) => PseudoElementValue::Identifiers(texts(range)),
                PseudoElementRareValue::TransitionName { name, classes } => {
                    PseudoElementValue::TransitionName(TransitionNameAndClassSelector {
                        name: name.map(|name| match name {
                            None => TransitionNameSelector::Universal,
                            Some(range) => TransitionNameSelector::Name(self.text(range)),
                        }),
                        classes: texts(classes),
                    })
                }
            },
            identifier_identities: PoolRange::of(&self.atoms, rare.identifier_identities),
        })
    }
}

impl SelectorArena {
    /// Where the compound starting at `start` ends, past its last component.
    #[inline]
    fn compound_end(&self, start: usize) -> usize {
        // NB: A compound shorter than 16 components says its length on its first component.
        let length = ((self.components[start].bits.0 & COMPOUND_LENGTH_BITS) >> COMPOUND_LENGTH_SHIFT) as usize;
        if length != 0 {
            return start + length;
        }
        let mut end = start;
        while self.components[end].bits.relation() == Relation::SubSelector {
            end += 1;
        }
        end + 1
    }
}

fn attribute_operator(value: u8) -> AttributeMatchType {
    const OPERATORS: [AttributeMatchType; 7] = [
        AttributeMatchType::HasAttribute,
        AttributeMatchType::ExactValue,
        AttributeMatchType::ContainsWord,
        AttributeMatchType::ContainsString,
        AttributeMatchType::StartsWithSegment,
        AttributeMatchType::StartsWithString,
        AttributeMatchType::EndsWithString,
    ];
    OPERATORS[value as usize]
}

fn attribute_case(value: u8) -> AttributeCaseType {
    const CASES: [AttributeCaseType; 3] = [
        AttributeCaseType::Default,
        AttributeCaseType::Sensitive,
        AttributeCaseType::Insensitive,
    ];
    CASES[value as usize]
}

/// A selector's identity in the process, from one of its compounds leftwards.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct SelectorKey {
    pub(crate) arena: SelectorArenaSerial,
    pub(crate) at: ComponentOffset,
}

/// One complex selector, by the offset of its subject component in its arena.
#[derive(Clone, Copy, Debug)]
pub struct Selector<'a> {
    arena: &'a SelectorArena,
    at: ComponentOffset,
}

impl PartialEq for Selector<'_> {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self.arena, other.arena) && self.at == other.at
    }
}

impl Eq for Selector<'_> {}

/// One compound of a complex selector, as a range of its components, which it decodes one by one
/// as they are read.
#[derive(Clone, Copy, Debug)]
pub struct CompoundRef<'a> {
    /// The combinator relating this compound to the compound immediately to its left. The
    /// leftmost compound therefore has `Combinator::None`, unless the selector is relative.
    pub combinator: Combinator,
    pub is_implicit_universal_anchor: bool,
    pub simple_selectors: SimpleSelectors<'a>,
}

/// The simple selectors of a compound, decoded from their components as they are read.
#[derive(Clone, Copy, Debug)]
pub struct SimpleSelectors<'a> {
    arena: &'a SelectorArena,
    start: u32,
    end: u32,
}

impl<'a> SimpleSelectors<'a> {
    fn new(arena: &'a SelectorArena, start: usize, end: usize) -> Self {
        Self {
            arena,
            start: index_u32(start),
            end: index_u32(end),
        }
    }

    #[must_use]
    pub fn len(self) -> usize {
        (self.end - self.start) as usize
    }

    #[must_use]
    pub fn is_empty(self) -> bool {
        self.start == self.end
    }

    #[must_use]
    pub fn get(self, index: usize) -> Option<SimpleSelector<'a>> {
        (index < self.len()).then(|| self.arena.decode(self.start as usize + index))
    }

    #[must_use]
    pub fn first(self) -> Option<SimpleSelector<'a>> {
        self.get(0)
    }

    /// The simple selectors after the first.
    #[must_use]
    pub fn after_first(self) -> Self {
        Self {
            start: (self.start + 1).min(self.end),
            ..self
        }
    }

    pub fn iter(self) -> impl DoubleEndedIterator<Item = SimpleSelector<'a>> + ExactSizeIterator + 'a {
        let arena = self.arena;
        (self.start as usize..self.end as usize).map(move |index| arena.decode(index))
    }

    /// The simple selectors as their components say them: an id or a class by its name alone, read where it lies,
    /// and any other to be decoded where it is asked for.
    pub(crate) fn views(self) -> impl Iterator<Item = SimpleView<'a>> + 'a {
        let arena = self.arena;
        (self.start as usize..self.end as usize).map(move |index| {
            let Component { bits, data } = arena.components[index];
            let name = || NameSelector {
                name: StyleAtomID(data),
                is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
            };
            match bits.kind() {
                ComponentKind::Id => SimpleView::Id(name()),
                ComponentKind::Class => SimpleView::Class(name()),
                ComponentKind::Tag if bits.namespace() != NamespaceType::Named => SimpleView::TagName(QualifiedName {
                    namespace_type: bits.namespace(),
                    namespace: StyleAtomID::NONE,
                    name: StyleAtomID(data),
                    name_is_ascii_lowercase: bits.has(ASCII_LOWERCASE),
                }),
                ComponentKind::PseudoClass => SimpleView::PseudoClass {
                    kind: pseudo_class_from_ffi(bits.pseudo()),
                    is_search_text_filter: bits.has(SEARCH_TEXT_FILTER),
                    component: ComponentRef { arena, index },
                },
                _ => SimpleView::Other(ComponentRef { arena, index }),
            }
        })
    }
}

/// A simple selector as its component says it, which matching reads without decoding the common kinds.
#[derive(Clone, Copy, Debug)]
pub(crate) enum SimpleView<'a> {
    Id(NameSelector),
    Class(NameSelector),
    /// A type selector naming no namespace prefix.
    TagName(QualifiedName),
    /// A pseudo-class, by its kind as the component says it, which needs no decode to tell a state or a structural
    /// test taking no argument, with the component to decode for the rest.
    PseudoClass {
        kind: PseudoClassType,
        /// Whether a `:not()` only picks the kind of search text match its entry styles.
        is_search_text_filter: bool,
        component: ComponentRef<'a>,
    },
    /// Any other simple selector.
    Other(ComponentRef<'a>),
}

/// A name of a compound its id and class selectors test.
#[derive(Clone, Copy, Debug)]
pub(crate) enum IdOrClass {
    Id(NameSelector),
    Class(NameSelector),
}

/// A component of an arena, to be decoded where it is asked for.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ComponentRef<'a> {
    arena: &'a SelectorArena,
    index: usize,
}

impl<'a> SimpleSelector<'a> {
    /// The selectors nested in the simple selector: what a pseudo-class takes, what a `::slotted()` takes, and the
    /// selectors of the style rule a `&` names.
    pub(crate) fn nested(&self) -> impl Iterator<Item = Selector<'a>> + 'a {
        let (list, argument) = match self {
            Self::PseudoClass(pseudo_class) => (Some(pseudo_class.argument_selector_list), None),
            Self::PseudoElement(PseudoElementSelector {
                value: PseudoElementValue::CompoundSelector(argument),
                ..
            }) => (None, Some(*argument)),
            Self::Nesting(Nesting::Rule(parent)) => (Some(*parent), None),
            _ => (None, None),
        };
        list.into_iter().flatten().chain(argument)
    }
}

impl<'a> SimpleView<'a> {
    /// The simple selector, decoded.
    #[must_use]
    pub(crate) fn decode(self) -> SimpleSelector<'a> {
        match self {
            Self::Id(name) => SimpleSelector::Id(name),
            Self::Class(name) => SimpleSelector::Class(name),
            Self::TagName(name) => SimpleSelector::TagName(name),
            Self::Other(other) | Self::PseudoClass { component: other, .. } => other.arena.decode(other.index),
        }
    }
}

impl<'a> Selector<'a> {
    /// Where the selector starts in its arena.
    #[must_use]
    pub(crate) fn offset(self) -> ComponentOffset {
        self.at
    }

    /// Whether the selector, which starts here, names the scoping root: `:scope`, or a `&` directly inside `@scope`,
    /// anywhere in it.
    #[must_use]
    pub(crate) fn names_the_scope(self) -> bool {
        self.arena.components[self.at.0 as usize].bits.has(NAMES_THE_SCOPE)
    }

    /// Whether the selector, which starts here, holds no pseudo-element, `::part()` or `::slotted()` in its subject
    /// compound, as the arena noted as it froze.
    #[must_use]
    pub(crate) fn describes_its_subject(self) -> bool {
        self.arena.components[self.at.0 as usize]
            .bits
            .has(DESCRIBES_ITS_SUBJECT)
    }

    /// Whether the selector, which starts here, names an element in another tree, with `::slotted()` or `::part()`.
    #[must_use]
    pub(crate) fn escapes_its_namespace(self) -> bool {
        self.arena.components[self.at.0 as usize]
            .bits
            .has(ESCAPES_ITS_NAMESPACE)
    }

    /// The selector's identity in the process: its arena's serial, and where it starts there.
    #[must_use]
    pub(crate) fn key(self) -> SelectorKey {
        SelectorKey {
            arena: self.arena.serial,
            at: self.at,
        }
    }

    /// The compounds of the selector in matching order, each as the range of its components and
    /// its relation to the compound left of it.
    fn compound_ranges(self) -> SmallVec<[(usize, usize, Relation); 8]> {
        let mut compounds = SmallVec::new();
        let mut start = self.at.0 as usize;
        let mut index = start;
        loop {
            let relation = self.arena.components[index].bits.relation();
            index += 1;
            if relation == Relation::SubSelector {
                continue;
            }
            compounds.push((start, index, relation));
            if relation == Relation::Leftmost {
                return compounds;
            }
            start = index;
        }
    }

    /// The selector of the same arena from `at`, the start of one of its compounds, leftwards: what
    /// is left to match of a selector once a walk along it reached that compound.
    #[must_use]
    pub(crate) fn at_compound(self, at: ComponentOffset) -> Self {
        Self { arena: self.arena, at }
    }

    /// The `@namespace` declarations of the sheet the selector is of.
    #[must_use]
    pub(crate) fn namespaces(self) -> SheetNamespaces<'a> {
        SheetNamespaces::new(
            self.arena.default_namespace,
            PoolRange::of(&self.arena.atoms, self.arena.namespace_prefixes),
        )
    }

    /// Where the first compound ends, past its last component, with that component's relation.
    #[inline]
    fn first_compound_end(self) -> (usize, Relation) {
        let end = self.arena.compound_end(self.at.0 as usize);
        (end, self.arena.components[end - 1].bits.relation())
    }

    /// The first compound, the subject's, as the matcher reads it.
    #[must_use]
    pub(crate) fn first_compound(self) -> CompoundRef<'a> {
        let start = self.at.0 as usize;
        let (end, relation) = self.first_compound_end();
        let first = self.arena.components[start];
        CompoundRef {
            combinator: relation.combinator(),
            is_implicit_universal_anchor: first.bits.kind() == ComponentKind::Universal && first.bits.has(IMPLICIT),
            simple_selectors: match first.bits.kind() {
                ComponentKind::RelativeAnchor => SimpleSelectors::new(self.arena, start, start),
                _ => SimpleSelectors::new(self.arena, start, end),
            },
        }
    }

    /// What the arena noted of the first compound as it froze.
    #[inline]
    fn compound_facts(self) -> Option<&'a CompoundFacts> {
        let at = self.at.0 as usize;
        let (word, bit) = (at / 64, at % 64);
        let starts = *self.arena.compound_starts.get(word)?;
        if starts & (1 << bit) == 0 {
            return None;
        }
        let index = self.arena.compound_ranks[word] + (starts & ((1 << bit) - 1)).count_ones();
        self.arena.compounds.get(index as usize)
    }

    /// The dispatch bloom bits of the ids, classes, lowercase types and states the first compound tests, as the arena
    /// noted them as it froze.
    #[must_use]
    pub(crate) fn required_dispatch_bloom(self) -> u64 {
        self.compound_facts().map_or(0, |facts| facts.bloom)
    }

    /// What tells the tests of the first compound from those of other compounds, alike for compounds of the arena
    /// testing the same facts alike, whatever selector they are of: what an answer of the compound's own tests can be
    /// shared by. None for a compound whose tests read the selector they are of or the tree its rule decides in, as
    /// `:scope`, `&`, `:host` and pseudo-elements do.
    #[must_use]
    pub(crate) fn own_tests(self) -> Option<(SelectorArenaSerial, u32)> {
        self.compound_facts()
            .filter(|facts| facts.shape != CompoundFacts::NO_SHAPE)
            .map(|facts| (self.arena.serial, facts.shape))
    }

    /// What tells the selector from any other of the process, for as long as the process lives.
    #[must_use]
    pub(crate) fn identity(self) -> (SelectorArenaSerial, ComponentOffset) {
        (self.arena.serial, self.at)
    }

    /// The ids, as `false`, and the classes, as `true`, the first compound tests, read from its components.
    pub(crate) fn ids_and_classes(self) -> impl Iterator<Item = IdOrClass> + 'a {
        let (end, _) = self.first_compound_end();
        let components = &self.arena.components[self.at.0 as usize..end];
        components.iter().filter_map(|component| {
            let name = NameSelector {
                name: StyleAtomID(component.data),
                is_ascii_lowercase: component.bits.has(ASCII_LOWERCASE),
            };
            match component.bits.kind() {
                ComponentKind::Id => Some(IdOrClass::Id(name)),
                ComponentKind::Class => Some(IdOrClass::Class(name)),
                _ => None,
            }
        })
    }

    /// Whether the first compound is the implicit anchor a relative selector ends in.
    #[must_use]
    pub(crate) fn is_relative_anchor(self) -> bool {
        self.arena.components[self.at.0 as usize].bits.kind() == ComponentKind::RelativeAnchor
    }

    /// The combinator left of the first compound, with the selector from the compound left of it,
    /// or none where the first compound is the leftmost.
    #[must_use]
    pub(crate) fn left(self) -> Option<(Combinator, Self)> {
        let (end, relation) = self.first_compound_end();
        (relation != Relation::Leftmost)
            .then(|| (relation.combinator(), self.at_compound(ComponentOffset(index_u32(end)))))
    }

    /// The selector from the compound right of the first one, in the complex selector the first
    /// compound is of, or none where the first compound is that selector's subject.
    #[must_use]
    pub(crate) fn right(self) -> Option<Self> {
        let at = self.at.0 as usize;
        // NB: Every list ends its last selector with a leftmost compound, and nested lists stand
        //     before the selectors holding them, so what stands right before a subject ends
        //     another selector.
        if at == 0 || self.arena.components[at - 1].bits.relation() == Relation::Leftmost {
            return None;
        }
        let mut start = at - 1;
        while start > 0 && self.arena.components[start - 1].bits.relation() == Relation::SubSelector {
            start -= 1;
        }
        Some(self.at_compound(ComponentOffset(index_u32(start))))
    }

    /// The selector from the compound of this selector, or of one nested in it, holding the
    /// pseudo-class, pseudo-element or `&` whose list holds `nested`, or none where none does.
    #[must_use]
    pub(crate) fn compound_owning(self, nested: Self) -> Option<Self> {
        for (start, end, _) in self.compound_ranges() {
            for index in start..end {
                if self.arena.components[index].bits.kind() == ComponentKind::RelativeAnchor {
                    continue;
                }
                for selector in self.arena.decode(index).nested() {
                    if selector.at == nested.at {
                        return Some(self.at_compound(ComponentOffset(index_u32(start))));
                    }
                    if let Some(found) = selector.compound_owning(nested) {
                        return Some(found);
                    }
                }
            }
        }
        None
    }

    /// The compounds of the selector, left to right.
    #[must_use]
    pub fn compound_selectors(self) -> SmallVec<[CompoundRef<'a>; 4]> {
        let ranges = self.compound_ranges();
        let mut compounds = SmallVec::with_capacity(ranges.len());
        // NB: Each compound holds its relation to the compound left of it, the next in matching
        //     order, and a relative selector's leftmost compound is an implicit anchor, so the
        //     first compound written holds the leading combinator.
        for &(start, end, relation) in ranges.iter().rev() {
            let first = self.arena.components[start];
            if first.bits.kind() == ComponentKind::RelativeAnchor {
                continue;
            }
            compounds.push(CompoundRef {
                combinator: relation.combinator(),
                is_implicit_universal_anchor: first.bits.kind() == ComponentKind::Universal && first.bits.has(IMPLICIT),
                simple_selectors: SimpleSelectors::new(self.arena, start, end),
            });
        }
        compounds
    }

    /// The compounds of the selector in matching order, from the subject leftwards, without the
    /// implicit anchor a relative selector ends in. Each is read where it lies in the arena.
    pub(crate) fn compounds(self) -> impl Iterator<Item = CompoundRef<'a>> + 'a {
        self.chain().map(Selector::first_compound)
    }

    /// The selector from each of its compounds leftwards, from this one on, without the implicit
    /// anchor a relative selector ends in.
    pub(crate) fn chain(self) -> impl Iterator<Item = Self> + 'a {
        std::iter::successors(Some(self), |current| current.left().map(|(_, left)| left))
            .filter(|current| !current.is_relative_anchor())
    }

    /// The leftmost compound of the selector that is not a relative selector's implicit anchor:
    /// the one that holds a relative selector's leading combinator.
    #[must_use]
    pub(crate) fn leftmost_compound(self) -> CompoundRef<'a> {
        self.chain().last().unwrap_or(self).first_compound()
    }

    /// The pseudo-element required on the initial match target, if this selector ends in one.
    #[must_use]
    pub fn target_pseudo_element(self) -> Option<PseudoElementType> {
        // NB: The compound a selector ends in is its first in matching order, the subject's.
        let first = self.arena.components[self.at.0 as usize];
        if first.bits.kind() != ComponentKind::PseudoElement {
            return None;
        }
        let last = self.first_compound();
        let pseudo_element = match last.simple_selectors.first()? {
            SimpleSelector::PseudoElement(selector)
                if !matches!(
                    selector.pseudo_element,
                    PseudoElementType::Part | PseudoElementType::Slotted
                ) =>
            {
                selector.pseudo_element
            }
            _ => return None,
        };
        let names_current_match = pseudo_element == PseudoElementType::SearchText
            && last
                .simple_selectors
                .iter()
                .any(|simple| search_text_match_filter(&simple) == Some(SearchTextMatchFilter::Current));
        Some(if names_current_match {
            PseudoElementType::SearchTextCurrent
        } else {
            pseudo_element
        })
    }

    #[must_use]
    pub fn styles_every_search_text_match(self) -> bool {
        self.target_pseudo_element() == Some(PseudoElementType::SearchText)
            && self
                .first_compound()
                .simple_selectors
                .iter()
                .all(|simple| search_text_match_filter(&simple).is_none())
    }

    /// https://www.w3.org/TR/selectors-4/#specificity-rules
    ///
    /// NB: A `&` naming a style rule adds what `:is()` of that rule's selectors would, and any
    ///     other `&` adds nothing. https://drafts.csswg.org/css-nesting/#nest-selector
    #[must_use]
    pub fn specificity(self) -> Specificity {
        let greatest = |selectors: SelectorListView<'_>| -> Specificity {
            selectors.iter().map(Selector::specificity).max().unwrap_or_default()
        };
        let class = Specificity {
            classes: 1,
            ..Specificity::default()
        };
        let mut specificity = Specificity::default();
        for compound in self.compound_selectors() {
            for simple in compound.simple_selectors.iter() {
                let simple = &simple;
                let contribution = match simple {
                    SimpleSelector::Id(_) => Specificity {
                        ids: 1,
                        ..Specificity::default()
                    },
                    SimpleSelector::Class(_) | SimpleSelector::Attribute(_) => class,
                    SimpleSelector::TagName(_) => Specificity {
                        types: 1,
                        ..Specificity::default()
                    },
                    SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
                        PseudoClassType::Has | PseudoClassType::Is | PseudoClassType::Not => {
                            greatest(pseudo_class.argument_selector_list)
                        }
                        PseudoClassType::NthChild | PseudoClassType::NthLastChild | PseudoClassType::Host => {
                            class.saturating_add(greatest(pseudo_class.argument_selector_list))
                        }
                        PseudoClassType::Where => Specificity::default(),
                        _ => class,
                    },
                    SimpleSelector::PseudoElement(pseudo_element) => {
                        // https://drafts.csswg.org/css-view-transitions-2/#pseudo-element-class-additions
                        // The specificity of a named view transition pseudo-element selector with a * argument and with
                        // an empty <pt-class-selector> is zero.
                        if let PseudoElementValue::TransitionName(argument) = &pseudo_element.value
                            && argument.name == Some(TransitionNameSelector::Universal)
                            && argument.classes.is_empty()
                        {
                            Specificity::default()
                        } else {
                            let pseudo_specificity = Specificity {
                                types: 1,
                                ..Specificity::default()
                            };
                            match &pseudo_element.value {
                                PseudoElementValue::CompoundSelector(argument)
                                    if pseudo_element.pseudo_element == PseudoElementType::Slotted =>
                                {
                                    pseudo_specificity.saturating_add(argument.specificity())
                                }
                                _ => pseudo_specificity,
                            }
                        }
                    }
                    SimpleSelector::Nesting(Nesting::Rule(parent)) => greatest(*parent),
                    SimpleSelector::Nesting(_) => Specificity::default(),
                    SimpleSelector::Universal(_) | SimpleSelector::Invalid(_) => Specificity::default(),
                };
                specificity = specificity.saturating_add(contribution);
            }
        }
        specificity
    }
}

/// A selector list in its arena, or an empty one.
#[derive(Clone, Copy, Debug)]
pub struct SelectorListView<'a> {
    arena: &'a SelectorArena,
    start: Option<ListStart>,
}

impl<'a> SelectorListView<'a> {
    /// What identifies the list within the process, for as long as the process lives, or none for
    /// an empty list.
    #[must_use]
    pub(crate) fn identity(self) -> Option<(SelectorArenaSerial, ListStart)> {
        self.start.map(|start| (self.arena.serial, start))
    }

    pub fn iter(self) -> impl Iterator<Item = Selector<'a>> + 'a {
        self.arena.selectors_of(self.start)
    }

    #[must_use]
    pub fn is_empty(self) -> bool {
        self.start.is_none()
    }

    #[must_use]
    pub fn len(self) -> usize {
        self.iter().count()
    }

    /// What each `&` in the list names, nested lists included, or none where it has none. All of
    /// them name the same: the parse wrote them for one rule.
    #[must_use]
    pub fn nesting(self) -> Option<NestingOfArena<'a>> {
        fn in_selector(selector: Selector<'_>) -> Option<NestingOfArena<'_>> {
            selector
                .compound_selectors()
                .iter()
                .find_map(|compound| compound.simple_selectors.iter().find_map(|simple| in_simple(&simple)))
        }
        fn in_simple<'a>(simple: &SimpleSelector<'a>) -> Option<NestingOfArena<'a>> {
            match simple {
                SimpleSelector::Nesting(nesting) => Some(*nesting),
                _ => simple.nested().find_map(in_selector),
            }
        }
        self.iter().find_map(in_selector)
    }

    /// The list's one selector, where it holds exactly one.
    #[must_use]
    pub fn single(self) -> Option<Selector<'a>> {
        let mut selectors = self.iter();
        let first = selectors.next()?;
        selectors.next().is_none().then_some(first)
    }
}

/// A list of literals in an arena, as `:lang()` ranges or `::part()` names.
#[derive(Clone, Copy, Debug)]
pub struct TextList<'a> {
    arena: &'a SelectorArena,
    ranges: &'a [PoolRange],
}

impl<'a> TextList<'a> {
    pub fn iter(self) -> impl Iterator<Item = &'a [u16]> + 'a {
        self.ranges.iter().map(move |&range| self.arena.text(range))
    }

    #[must_use]
    pub fn is_empty(self) -> bool {
        self.ranges.is_empty()
    }

    #[must_use]
    pub fn len(self) -> usize {
        self.ranges.len()
    }
}

/// The language ranges of `:lang()`.
#[derive(Clone, Copy, Debug)]
pub struct Languages<'a> {
    arena: &'a SelectorArena,
    ranges: &'a [PoolRange],
}

/// One range of `:lang()`, as written.
#[derive(Clone, Copy, Debug)]
pub struct LanguageRange<'a> {
    pub value: &'a [u16],
    pub is_string: bool,
}

impl<'a> Languages<'a> {
    pub fn iter(self) -> impl Iterator<Item = LanguageRange<'a>> + 'a {
        self.ranges.iter().map(move |&range| LanguageRange {
            value: self.arena.text(PoolRange {
                start: range.start,
                length: range.length & !STRING_BIT,
            }),
            is_string: range.length & STRING_BIT != 0,
        })
    }

    #[must_use]
    pub fn is_empty(self) -> bool {
        self.ranges.is_empty()
    }
}

/// A simple selector, decoded from its component.
#[derive(Clone, Copy, Debug)]
pub enum SimpleSelector<'a> {
    Universal(QualifiedName),
    TagName(QualifiedName),
    Id(NameSelector),
    Class(NameSelector),
    Attribute(AttributeSelector<'a>),
    PseudoClass(PseudoClassSelector<'a>),
    PseudoElement(PseudoElementSelector<'a>),
    Nesting(NestingOfArena<'a>),
    Invalid(&'a [u16]),
}

#[derive(Clone, Copy, Debug)]
pub struct AttributeSelector<'a> {
    pub match_type: AttributeMatchType,
    pub qualified_name: QualifiedName,
    pub value: &'a [u16],
    /// The value as a process style atom, or none for a presence test.
    pub value_identity: StyleAtomID,
    pub case_type: AttributeCaseType,
    /// Whether the name is one of the attributes whose values HTML compares ASCII case-insensitively, written with
    /// no namespace.
    pub names_a_legacy_case_insensitive_attribute: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct PseudoClassSelector<'a> {
    pub pseudo_class: PseudoClassType,
    pub an_plus_b_pattern: AnPlusBPattern,
    pub argument_selector_list: SelectorListView<'a>,
    pub languages: Languages<'a>,
    pub direction: Option<Direction>,
    pub identifier: Option<&'a [u16]>,
    /// The identifier as a process style atom, where the pseudo-class names a state by it.
    pub identifier_identity: StyleAtomID,
    pub levels: &'a [i64],
    pub is_forgiving: bool,
}

#[derive(Clone, Copy, Debug)]
pub enum PseudoElementValue<'a> {
    None,
    CompoundSelector(Selector<'a>),
    Identifiers(TextList<'a>),
    TransitionName(TransitionNameAndClassSelector<'a>),
}

// https://drafts.csswg.org/css-view-transitions-1/#typedef-pt-name-selector
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransitionNameSelector<'a> {
    Universal,
    Name(&'a [u16]),
}

// https://drafts.csswg.org/css-view-transitions-2/#typedef-pt-name-and-class-selector
#[derive(Clone, Copy, Debug)]
pub struct TransitionNameAndClassSelector<'a> {
    pub name: Option<TransitionNameSelector<'a>>,
    pub classes: TextList<'a>,
}

#[derive(Clone, Copy, Debug)]
pub struct PseudoElementSelector<'a> {
    pub pseudo_element: PseudoElementType,
    pub serialized_name: Option<&'a [u16]>,
    pub value: PseudoElementValue<'a>,
    /// The process style atom of each name in `value`, when it holds identifiers.
    pub identifier_identities: &'a [StyleAtomID],
}

pub fn search_text_match_filter_of_pseudo_class(
    pseudo_class: &PseudoClassSelector<'_>,
) -> Option<SearchTextMatchFilter> {
    match pseudo_class.pseudo_class {
        PseudoClassType::Current => Some(SearchTextMatchFilter::Current),
        PseudoClassType::Not => {
            let argument = pseudo_class.argument_selector_list.single()?;
            let compounds = argument.compound_selectors();
            let [compound] = &*compounds else {
                return None;
            };
            (compound.simple_selectors.len() == 1
                && matches!(
                    compound.simple_selectors.first(),
                    Some(SimpleSelector::PseudoClass(inner)) if inner.pseudo_class == PseudoClassType::Current
                ))
            .then_some(SearchTextMatchFilter::NotCurrent)
        }
        _ => None,
    }
}

pub fn search_text_match_filter(simple: &SimpleSelector<'_>) -> Option<SearchTextMatchFilter> {
    match simple {
        SimpleSelector::PseudoClass(pseudo_class) => search_text_match_filter_of_pseudo_class(pseudo_class),
        _ => None,
    }
}

/// The arena a sheet's selectors go into as its parse finishes them, which every handle to them
/// holds from the start and reads once the parse froze it.
type SharedArena = Arc<std::sync::OnceLock<SelectorArena>>;

fn frozen(arena: &SharedArena) -> &SelectorArena {
    arena
        .get()
        .expect("a sheet's selectors are read once its parse has finished them")
}

/// Packs the selector lists of one parse, a whole sheet's or one list's, into one arena, which it
/// freezes as it drops: every handle it gave out then reads the arena.
#[derive(Default)]
pub(crate) struct SheetSelectorsBuilder {
    builder: Builder,
    /// The names the parse interned, which stay live until the arena leases them.
    names: Vec<crate::css::style::AtomLease>,
    /// The lists copied in, whose arenas keep their names live until this one leases them.
    copied: Vec<SelectorListRef>,
    arena: SharedArena,
}

impl SheetSelectorsBuilder {
    /// Packs `list`, whose names `names` keeps live and whose `&` names `nesting`, and answers its
    /// handle.
    pub(crate) fn push(
        &mut self,
        list: &[Arc<tree::CompiledSelector>],
        names: Vec<crate::css::style::AtomLease>,
        nesting: NestingTarget,
    ) -> SelectorListRef {
        let start = self.builder.push_list(list, nesting);
        self.names.extend(names);
        SelectorListRef {
            arena: Arc::clone(&self.arena),
            start,
        }
    }

    /// Packs a copy of `list` from another arena, whose `&` names `nesting` here.
    pub(crate) fn push_copy(&mut self, list: &SelectorListRef, nesting: NestingTarget) -> SelectorListRef {
        let copy = self.push(&list.view().to_tree(), Vec::new(), nesting);
        self.copied.push(list.clone());
        copy
    }

    /// Have the selectors of the arena be of a sheet declaring `declarations`.
    pub(crate) fn declare_namespaces(&mut self, declarations: NamespaceDeclarations) {
        self.builder.default_namespace = declarations.default;
        self.builder.namespace_prefixes = declarations.prefixes;
        self.names.extend(declarations.names);
    }

    /// Have the selectors pushed from here on be of a sheet declaring `prefix` to name `uri` as
    /// well, an empty prefix declaring the default namespace.
    pub(crate) fn declare_namespace(&mut self, prefix: &[u16], uri: &[u16]) {
        let mut declarations = NamespaceDeclarations {
            default: self.builder.default_namespace,
            prefixes: std::mem::take(&mut self.builder.namespace_prefixes),
            names: Vec::new(),
        };
        declarations.declare(prefix, uri);
        self.declare_namespaces(declarations);
    }

    /// Packs the selectors of a style rule as [`Self::push`] does, and answers too the ordinal a
    /// `&` in a rule nested in it names it by, which an empty list has none of.
    pub(crate) fn push_rule(
        &mut self,
        list: &[Arc<tree::CompiledSelector>],
        names: Vec<crate::css::style::AtomLease>,
        nesting: NestingTarget,
    ) -> (SelectorListRef, Option<RuleOrdinal>) {
        let selectors = self.push(list, names, nesting);
        let rule = selectors.start.map(|start| self.builder.push_rule(start));
        (selectors, rule)
    }
}

/// What a `&` in the selectors of a rule placed somewhere names: what the CSSOM knows of the place.
#[derive(Clone, Copy)]
pub(crate) enum NestingContext<'a> {
    Root,
    Scope,
    Rule(&'a SelectorListRef),
}

impl Drop for SheetSelectorsBuilder {
    fn drop(&mut self) {
        let arena = std::mem::take(&mut self.builder).finish();
        assert!(self.arena.set(arena).is_ok(), "a sheet's selectors are frozen once");
    }
}

/// A parsed selector list, holding its arena.
#[derive(Clone, Debug)]
pub struct SelectorListRef {
    arena: SharedArena,
    start: Option<ListStart>,
}

impl SelectorListRef {
    /// Packs the parsed list `list`, which names no style rule with a `&`, into an arena of its own.
    pub(crate) fn pack(list: &[Arc<tree::CompiledSelector>]) -> Self {
        SheetSelectorsBuilder::default().push(list, Vec::new(), NestingTarget::Root)
    }

    /// Whether each `&` in the list names what `context` says it does: the same selectors, or a
    /// copy of them naming what they name.
    #[must_use]
    pub(crate) fn is_nested_in(&self, context: NestingContext<'_>) -> bool {
        fn same(a: SelectorListView<'_>, b: SelectorListView<'_>) -> bool {
            if std::ptr::eq(a.arena, b.arena) && a.start == b.start {
                return true;
            }
            // NB: The trees name the atoms of live arenas.
            a.to_tree() == b.to_tree()
                && match (a.nesting(), b.nesting()) {
                    (Some(Nesting::Rule(a)), Some(Nesting::Rule(b))) => same(a, b),
                    (None, None)
                    | (Some(Nesting::Root), Some(Nesting::Root))
                    | (Some(Nesting::Scope), Some(Nesting::Scope))
                    | (Some(Nesting::Unbound), Some(Nesting::Unbound)) => true,
                    _ => false,
                }
        }
        match (self.view().nesting(), context) {
            (None, _) | (Some(Nesting::Root), NestingContext::Root) | (Some(Nesting::Scope), NestingContext::Scope) => {
                true
            }
            (Some(Nesting::Rule(parent)), NestingContext::Rule(rule)) => same(parent, rule.view()),
            _ => false,
        }
    }

    /// The list with each `&` in it naming what `context` says, which is the list itself where it
    /// already does. Otherwise it goes into an arena of its own after the selectors its `&` names,
    /// which name what they did.
    #[must_use]
    pub(crate) fn nested_in(&self, context: NestingContext<'_>) -> Self {
        if self.is_nested_in(context) {
            return self.clone();
        }
        let mut builder = SheetSelectorsBuilder::default();
        let nesting = match context {
            NestingContext::Root => NestingTarget::Root,
            NestingContext::Scope => NestingTarget::Scope,
            NestingContext::Rule(rule) => builder
                .builder
                .copy_rule(rule.view())
                .map_or(NestingTarget::Root, NestingTarget::Rule),
        };
        // NB: The tree names the atoms of this list's arena, which is live while it is packed again.
        builder.push(&self.view().to_tree(), Vec::new(), nesting)
    }

    /// The atoms the list's arena names, which it leases.
    pub(crate) fn leased_atoms(&self) -> &[StyleAtomID] {
        frozen(&self.arena)._leases.atoms()
    }

    /// The serial of the list's arena, which no other arena of the process takes.
    #[must_use]
    pub(crate) fn serial(&self) -> SelectorArenaSerial {
        frozen(&self.arena).serial
    }

    /// Where the list starts in its arena, or none for an empty list.
    #[must_use]
    pub(crate) fn start_offset(&self) -> Option<u32> {
        self.start.map(|start| start.0)
    }

    /// How many components the list's arena holds.
    #[must_use]
    pub(crate) fn arena_len(&self) -> u32 {
        index_u32(frozen(&self.arena).components.len())
    }

    /// The list of the same arena starting at `offset`, where one of its lists starts.
    #[must_use]
    pub(crate) fn list_at_offset(&self, offset: u32) -> Self {
        assert!(offset < self.arena_len(), "a list starts inside its arena");
        Self {
            arena: Arc::clone(&self.arena),
            start: Some(ListStart(offset)),
        }
    }

    #[must_use]
    pub fn view(&self) -> SelectorListView<'_> {
        SelectorListView {
            arena: frozen(&self.arena),
            start: self.start,
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = Selector<'_>> + '_ {
        self.view().iter()
    }

    /// The selector of the list starting at `offset`, holding its arena.
    #[must_use]
    pub(crate) fn selector_ref_at(&self, offset: ComponentOffset) -> SelectorRef {
        SelectorRef {
            arena: self.arena.clone(),
            at: offset,
        }
    }

    /// The selector of the list starting at `offset`, which one of the list's selectors names.
    #[must_use]
    pub(crate) fn selector_at(&self, offset: ComponentOffset) -> Selector<'_> {
        Selector {
            arena: frozen(&self.arena),
            at: offset,
        }
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.start.is_none()
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.view().len()
    }

    /// Each selector of the list, holding the arena.
    pub fn owned(&self) -> impl Iterator<Item = SelectorRef> + '_ {
        self.iter().map(|selector| SelectorRef {
            arena: Arc::clone(&self.arena),
            at: selector.at,
        })
    }
}

/// One parsed selector, holding its arena.
#[derive(Clone, Debug)]
pub struct SelectorRef {
    arena: SharedArena,
    at: ComponentOffset,
}

impl SelectorRef {
    #[must_use]
    pub fn view(&self) -> Selector<'_> {
        Selector {
            arena: frozen(&self.arena),
            at: self.at,
        }
    }

    /// The selector from the compound at `at` leftwards, in the same arena.
    #[must_use]
    pub(crate) fn at_compound(&self, at: ComponentOffset) -> Self {
        Self {
            arena: self.arena.clone(),
            at,
        }
    }
}

/// What matching asks of a compound as a whole, which the arena notes as it freezes.
#[derive(Debug)]
struct CompoundFacts {
    /// The compound's own tests, alike for compounds of the arena testing the same facts alike, or
    /// [`Self::NO_SHAPE`].
    shape: u32,
    /// The dispatch bloom bits of the ids, classes, types and attributes written in ASCII lowercase and states the
    /// compound tests: what an element has to carry to match it, which a walk up across a descendant combinator rejects ancestors on
    /// and a bucket's candidates are rejected on.
    bloom: u64,
}

const _: () = assert!(size_of::<CompoundFacts>() == 16);

impl CompoundFacts {
    const NO_SHAPE: u32 = u32::MAX;
}

/// Packs parsed trees into an arena.
#[derive(Default)]
struct Builder {
    components: Vec<Component>,
    qualified: Vec<QualifiedRare>,
    attributes: Vec<AttributeRare>,
    nths: Vec<NthRare>,
    arguments: Vec<ArgumentRare>,
    pseudo_elements: Vec<PseudoElementRare>,
    atoms: Vec<StyleAtomID>,
    numbers: Vec<i64>,
    text_ranges: Vec<PoolRange>,
    text: Vec<u16>,
    rule_lists: Vec<ListStart>,
    default_namespace: Option<StyleAtomID>,
    /// Each declared prefix followed by the namespace it names.
    namespace_prefixes: Vec<StyleAtomID>,
    /// What a `&` in the list being pushed names.
    nesting: NestingTarget,
    leased: Vec<StyleAtomID>,
}

fn pool_push<T>(pool: &mut Vec<T>, value: T) -> u32 {
    pool.push(value);
    index_u32(pool.len() - 1)
}

impl Builder {
    fn push_text(&mut self, text: &[u16]) -> PoolRange {
        let start = index_u32(self.text.len());
        self.text.extend_from_slice(text);
        PoolRange {
            start,
            length: index_u32(text.len()),
        }
    }

    fn push_text_list<'t>(&mut self, texts: impl IntoIterator<Item = (&'t [u16], bool)>) -> PoolRange {
        let ranges: Vec<_> = texts
            .into_iter()
            .map(|(text, is_string)| {
                let mut range = self.push_text(text);
                if is_string {
                    range.length |= STRING_BIT;
                }
                range
            })
            .collect();
        let start = index_u32(self.text_ranges.len());
        self.text_ranges.extend_from_slice(&ranges);
        PoolRange {
            start,
            length: index_u32(ranges.len()),
        }
    }

    fn lease(&mut self, atom: StyleAtomID) {
        if !atom.is_none() {
            self.leased.push(atom);
        }
    }

    /// Pushes `list`, nested lists first, and answers where it starts, or none for an empty list.
    fn push_list(&mut self, list: &[Arc<tree::CompiledSelector>], nesting: NestingTarget) -> Option<ListStart> {
        if list.is_empty() {
            return None;
        }
        self.nesting = nesting;
        // NB: The parser finished every nested list before the list naming it; they go first.
        let selectors: Vec<Vec<(Combinator, bool, Vec<Component>)>> = list
            .iter()
            .map(|selector| {
                selector
                    .compound_selectors()
                    .iter()
                    .map(|compound| {
                        let components = compound
                            .simple_selectors
                            .iter()
                            .map(|simple| self.component(simple))
                            .collect::<Vec<_>>();
                        (compound.combinator, compound.is_implicit_universal_anchor, components)
                    })
                    .collect()
            })
            .collect();
        let start = ListStart(index_u32(self.components.len()));
        let count = selectors.len();
        for (index, compounds) in selectors.into_iter().enumerate() {
            let leading = compounds.first().map_or(Combinator::None, |compound| compound.0);
            let compound_count = compounds.len();
            for (position, (combinator, implicit_anchor, mut components)) in compounds.into_iter().rev().enumerate() {
                assert!(!components.is_empty(), "a compound holds a simple selector");
                if implicit_anchor {
                    components[0].bits = components[0].bits.with(IMPLICIT, true);
                }
                let leftmost = position + 1 == compound_count;
                let relation = match leftmost {
                    true if leading != Combinator::None => Relation::of(leading),
                    true => Relation::Leftmost,
                    false => {
                        assert_ne!(
                            combinator,
                            Combinator::None,
                            "only the leftmost compound has no combinator"
                        );
                        Relation::of(combinator)
                    }
                };
                let last = components.len() - 1;
                components[last].bits = components[last].bits.with_relation(relation);
                if relation == Relation::Leftmost && index + 1 == count {
                    components[last].bits = components[last].bits.with(LAST_IN_LIST, true);
                }
                self.components.extend(components);
            }
            if leading != Combinator::None {
                let bits = ComponentBits::new(ComponentKind::RelativeAnchor)
                    .with(IMPLICIT, true)
                    .with_relation(Relation::Leftmost)
                    .with(LAST_IN_LIST, index + 1 == count);
                self.components.push(Component { bits, data: 0 });
            }
        }
        Some(start)
    }

    fn push_rule(&mut self, start: ListStart) -> RuleOrdinal {
        self.rule_lists.push(start);
        RuleOrdinal(index_u32(self.rule_lists.len() - 1))
    }

    /// Pushes the selectors of a style rule from another arena, after the selectors their `&` names,
    /// and answers the rule's ordinal here, or none for an empty list.
    fn copy_rule(&mut self, list: SelectorListView<'_>) -> Option<RuleOrdinal> {
        let nesting = match list.nesting() {
            None | Some(Nesting::Root) => NestingTarget::Root,
            Some(Nesting::Scope) => NestingTarget::Scope,
            Some(Nesting::Unbound) => NestingTarget::Unbound,
            Some(Nesting::Rule(parent)) => self.copy_rule(parent).map_or(NestingTarget::Root, NestingTarget::Rule),
        };
        let start = self.push_list(&list.to_tree(), nesting)?;
        Some(self.push_rule(start))
    }

    fn qualified_data(&mut self, name: &QualifiedName) -> u32 {
        self.lease(name.name);
        match name.namespace_type {
            NamespaceType::Named => {
                self.lease(name.namespace);
                pool_push(
                    &mut self.qualified,
                    QualifiedRare {
                        prefix: name.namespace,
                        local: name.name,
                    },
                )
            }
            _ => name.name.0,
        }
    }

    /// The component of `simple`, whose nested lists it pushes first.
    fn pseudo_class_component(&mut self, pseudo_class: &tree::PseudoClassSelector) -> (ComponentBits, u32) {
        let bits = ComponentBits::new(ComponentKind::PseudoClass)
            .with_pseudo(pseudo_class.pseudo_class as u8)
            .with(FORGIVING, pseudo_class.is_forgiving);
        let list = |builder: &mut Self| {
            builder
                .push_list(&pseudo_class.argument_selector_list, builder.nesting)
                .map_or(NONE, |start| start.0)
        };
        match pseudo_class.pseudo_class.metadata().parameter_type {
            PseudoClassParameterType::None => (bits, NONE),
            PseudoClassParameterType::AnPlusB | PseudoClassParameterType::AnPlusBOf => {
                let of = list(self);
                let AnPlusBPattern {
                    step_size: a,
                    offset: b,
                } = pseudo_class.an_plus_b_pattern;
                match (i16::try_from(a), i16::try_from(b), of) {
                    (Ok(a), Ok(b), NONE) if (u32::from(a as u16) << 16 | u32::from(b as u16)) != NONE => (
                        bits.with(INLINE, true),
                        (u32::from(a as u16) << 16) | u32::from(b as u16),
                    ),
                    _ => (bits, pool_push(&mut self.nths, NthRare { a, b, of })),
                }
            }
            PseudoClassParameterType::Ident => match &pseudo_class.identifier {
                None => (bits, NONE),
                Some(identifier) => {
                    // NB: `:dir()` compares its keyword ASCII case-insensitively, as a match
                    //     would otherwise fold it through the atom table each time.
                    let atom = match pseudo_class.pseudo_class {
                        PseudoClassType::Dir => {
                            crate::css::style::process_atoms().ascii_lowercase(pseudo_class.identifier_identity)
                        }
                        _ => pseudo_class.identifier_identity,
                    };
                    self.lease(atom);
                    let text = self.push_text(identifier);
                    let rare = ArgumentRare::Ident {
                        text,
                        atom,
                        direction: pseudo_class.direction,
                    };
                    (bits, pool_push(&mut self.arguments, rare))
                }
            },
            PseudoClassParameterType::LanguageRanges => {
                let ranges = self.push_text_list(
                    pseudo_class
                        .languages
                        .iter()
                        .map(|range| (range.value.units(), range.is_string)),
                );
                (bits, pool_push(&mut self.arguments, ArgumentRare::Languages { ranges }))
            }
            PseudoClassParameterType::LevelList => {
                let start = index_u32(self.numbers.len());
                self.numbers.extend_from_slice(&pseudo_class.levels);
                let levels = PoolRange {
                    start,
                    length: index_u32(pseudo_class.levels.len()),
                };
                (bits, pool_push(&mut self.arguments, ArgumentRare::Levels { levels }))
            }
            PseudoClassParameterType::CompoundSelector
            | PseudoClassParameterType::ForgivingSelectorList
            | PseudoClassParameterType::RelativeSelectorList
            | PseudoClassParameterType::SelectorList => (bits, list(self)),
        }
    }

    fn pseudo_element_component(&mut self, pseudo_element: &tree::PseudoElementSelector) -> (ComponentBits, u32) {
        let bits = ComponentBits::new(ComponentKind::PseudoElement).with_pseudo(pseudo_element.pseudo_element as u8);
        let serialized_name = pseudo_element.serialized_name.as_ref().map(|name| self.push_text(name));
        let value = match &pseudo_element.value {
            tree::PseudoElementValue::None => PseudoElementRareValue::None,
            tree::PseudoElementValue::CompoundSelector(selector) => PseudoElementRareValue::CompoundSelector(
                self.push_list(std::slice::from_ref(selector), self.nesting)
                    .expect("a one-selector list is not empty"),
            ),
            tree::PseudoElementValue::Identifiers(identifiers) => PseudoElementRareValue::Identifiers(
                self.push_text_list(identifiers.iter().map(|identifier| (identifier.units(), false))),
            ),
            tree::PseudoElementValue::TransitionName(argument) => {
                let name = argument.name.as_ref().map(|name| match name {
                    tree::TransitionNameSelector::Universal => None,
                    tree::TransitionNameSelector::Name(name) => Some(self.push_text(name)),
                });
                let classes = self.push_text_list(argument.classes.iter().map(|class| (class.units(), false)));
                PseudoElementRareValue::TransitionName { name, classes }
            }
        };
        let identities_start = index_u32(self.atoms.len());
        for &atom in &pseudo_element.identifier_identities {
            self.lease(atom);
            self.atoms.push(atom);
        }
        let identifier_identities = PoolRange {
            start: identities_start,
            length: index_u32(pseudo_element.identifier_identities.len()),
        };
        if serialized_name.is_none() && value == PseudoElementRareValue::None && identifier_identities.length == 0 {
            (bits, NONE)
        } else {
            let rare = PseudoElementRare {
                serialized_name,
                value,
                identifier_identities,
            };
            (bits, pool_push(&mut self.pseudo_elements, rare))
        }
    }

    fn component(&mut self, simple: &tree::SimpleSelector) -> Component {
        let (bits, data) = match simple {
            tree::SimpleSelector::Universal(name) => (
                ComponentBits::new(ComponentKind::Universal)
                    .with_namespace(name.namespace_type)
                    .with(ASCII_LOWERCASE, name.name_is_ascii_lowercase),
                self.qualified_data(name),
            ),
            tree::SimpleSelector::TagName(name) => (
                ComponentBits::new(ComponentKind::Tag)
                    .with_namespace(name.namespace_type)
                    .with(ASCII_LOWERCASE, name.name_is_ascii_lowercase),
                self.qualified_data(name),
            ),
            tree::SimpleSelector::Id(name) => {
                self.lease(name.name);
                (
                    ComponentBits::new(ComponentKind::Id).with(ASCII_LOWERCASE, name.is_ascii_lowercase),
                    name.name.0,
                )
            }
            tree::SimpleSelector::Class(name) => {
                self.lease(name.name);
                (
                    ComponentBits::new(ComponentKind::Class).with(ASCII_LOWERCASE, name.is_ascii_lowercase),
                    name.name.0,
                )
            }
            tree::SimpleSelector::Attribute(attribute) => {
                let name = &attribute.qualified_name;
                self.lease(name.name);
                self.lease(name.namespace);
                self.lease(attribute.value_identity);
                let text = self.push_text(&attribute.value);
                let rare = AttributeRare {
                    name: QualifiedRare {
                        prefix: name.namespace,
                        local: name.name,
                    },
                    value: attribute.value_identity,
                    text,
                };
                // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
                let legacy_case_insensitive =
                    matches!(name.namespace_type, NamespaceType::Default | NamespaceType::None)
                        && crate::css::style::process_atoms().text(name.name).is_some_and(|text| {
                            crate::css::selector::is_ascii_case_insensitive_html_attribute(&text.to_utf16())
                        });
                (
                    ComponentBits::new(ComponentKind::Attribute)
                        .with_namespace(name.namespace_type)
                        .with_pseudo(attribute.match_type as u8 | ((attribute.case_type as u8) << 3))
                        .with(ASCII_LOWERCASE, name.name_is_ascii_lowercase)
                        .with(LEGACY_CASE_INSENSITIVE, legacy_case_insensitive),
                    pool_push(&mut self.attributes, rare),
                )
            }
            tree::SimpleSelector::PseudoClass(pseudo_class) => self.pseudo_class_component(pseudo_class),
            tree::SimpleSelector::PseudoElement(pseudo_element) => self.pseudo_element_component(pseudo_element),
            tree::SimpleSelector::Nesting => (
                ComponentBits::new(ComponentKind::Nesting).with_pseudo(self.nesting.kind()),
                match self.nesting {
                    NestingTarget::Rule(rule) => rule.0,
                    NestingTarget::Root | NestingTarget::Scope | NestingTarget::Unbound => NONE,
                },
            ),
            tree::SimpleSelector::Invalid(text) => {
                let range = self.push_text(text);
                (
                    ComponentBits::new(ComponentKind::Invalid),
                    pool_push(&mut self.text_ranges, range),
                )
            }
            tree::SimpleSelector::Compound { .. } => unreachable!("a compound start is not among its simple selectors"),
        };
        Component { bits, data }
    }

    fn finish(mut self) -> SelectorArena {
        let namespace_prefixes = PoolRange {
            start: index_u32(self.atoms.len()),
            length: index_u32(self.namespace_prefixes.len()),
        };
        // NB: `[*|x]` and `[ns|x]` test names qualified by a namespace, which what reads the arena,
        //     as the host deciding which attributes to publish, looks up by their qualified atoms.
        let atoms = crate::css::style::process_atoms();
        let mut qualified_names = Vec::new();
        for component in &self.components {
            if component.bits.kind() != ComponentKind::Attribute {
                continue;
            }
            let qualifier = match component.bits.namespace() {
                NamespaceType::Any => Some(StyleAtomID::NONE),
                NamespaceType::Named => {
                    let prefix = self.attributes[component.data as usize].name.prefix;
                    self.namespace_prefixes
                        .as_chunks::<2>()
                        .0
                        .iter()
                        .find(|declared| declared[0] == prefix)
                        .map(|declared| declared[1])
                }
                _ => None,
            };
            let Some(namespace) = qualifier else {
                continue;
            };
            let local = self.attributes[component.data as usize].name.local;
            for name in [local, atoms.ascii_lowercase(local)] {
                qualified_names.push(crate::css::style::AtomLease::acquire_qualified(atoms, namespace, name));
            }
        }
        self.atoms.extend_from_slice(&self.namespace_prefixes);
        let namespace_atoms = self
            .default_namespace
            .into_iter()
            .chain(self.namespace_prefixes)
            .chain(qualified_names.iter().map(crate::css::style::AtomLease::atom));
        let mut arena = SelectorArena {
            components: self.components.into_boxed_slice(),
            qualified: self.qualified.into_boxed_slice(),
            attributes: self.attributes.into_boxed_slice(),
            nths: self.nths.into_boxed_slice(),
            arguments: self.arguments.into_boxed_slice(),
            pseudo_elements: self.pseudo_elements.into_boxed_slice(),
            atoms: self.atoms.into_boxed_slice(),
            numbers: self.numbers.into_boxed_slice(),
            text_ranges: self.text_ranges.into_boxed_slice(),
            text: self.text.into_boxed_slice(),
            rule_lists: self.rule_lists.into_boxed_slice(),
            _leases: crate::css::style::process_atoms().lease(self.leased.into_iter().chain(namespace_atoms)),
            default_namespace: self.default_namespace,
            namespace_prefixes,
            serial: crate::render_state::process_style().next_selector_arena_serial(),
            compounds: Box::default(),
            compound_starts: Box::default(),
            compound_ranks: Box::default(),
        };
        arena.mark_selector_facts();
        arena
    }
}

impl SelectorArena {
    /// Write what matching asks of each selector as a whole on its first component, once, as the arena freezes.
    fn mark_selector_facts(&mut self) {
        fn names_the_scope(selector: Selector<'_>) -> bool {
            fn simple(simple: &SimpleSelector<'_>) -> bool {
                match simple {
                    SimpleSelector::PseudoClass(pseudo_class) => {
                        pseudo_class.pseudo_class == PseudoClassType::Scope
                            || pseudo_class.argument_selector_list.iter().any(names_the_scope)
                    }
                    SimpleSelector::Nesting(Nesting::Scope) => true,
                    SimpleSelector::Nesting(Nesting::Rule(parent)) => parent.iter().any(names_the_scope),
                    _ => false,
                }
            }
            selector
                .compounds()
                .any(|compound| compound.simple_selectors.iter().any(|candidate| simple(&candidate)))
        }
        let escapes_its_namespace = |selector: Selector<'_>| {
            selector.compounds().any(|compound| {
                compound.simple_selectors.iter().any(|simple| {
                    matches!(simple, SimpleSelector::PseudoElement(pseudo_element)
                        if matches!(pseudo_element.pseudo_element, PseudoElementType::Slotted | PseudoElementType::Part))
                })
            })
        };
        let search_text_filters: Vec<usize> = (0..self.components.len())
            .filter(|&at| {
                self.components[at].bits.kind() == ComponentKind::PseudoClass
                    && matches!(self.decode(at), SimpleSelector::PseudoClass(pseudo_class)
                        if search_text_match_filter_of_pseudo_class(&pseudo_class) == Some(SearchTextMatchFilter::NotCurrent))
            })
            .collect();
        for at in search_text_filters {
            self.components[at].bits.0 |= SEARCH_TEXT_FILTER;
        }
        // NB: Every component is of one selector, which its last component, the one ending the leftmost compound,
        //     ends.
        let mut facts = Vec::new();
        let mut start = 0;
        for index in 0..self.components.len() {
            if self.components[index].bits.relation() != Relation::Leftmost {
                continue;
            }
            let selector = Selector {
                arena: self,
                at: ComponentOffset(index_u32(start)),
            };
            let subject_end = (start..=index)
                .find(|&at| self.components[at].bits.relation() != Relation::SubSelector)
                .unwrap_or(index);
            let describes_its_subject =
                !(start..=subject_end).any(|at| self.components[at].bits.kind() == ComponentKind::PseudoElement);
            let flags = (u32::from(names_the_scope(selector)) * NAMES_THE_SCOPE)
                | (u32::from(escapes_its_namespace(selector)) * ESCAPES_ITS_NAMESPACE)
                | (u32::from(describes_its_subject) * DESCRIBES_ITS_SUBJECT);
            if flags != 0 {
                facts.push((start, flags));
            }
            start = index + 1;
        }
        for (start, flags) in facts {
            self.components[start].bits.0 |= flags;
        }
        self.mark_compound_lengths();
        self.note_compound_facts();
    }

    /// Write the length of each compound shorter than 16 components on its first component.
    fn mark_compound_lengths(&mut self) {
        let mut start = 0;
        for index in 0..self.components.len() {
            if self.components[index].bits.relation() == Relation::SubSelector {
                continue;
            }
            let length = index + 1 - start;
            if length < 16 {
                debug_assert_eq!(self.components[start].bits.0 & COMPOUND_LENGTH_BITS, 0);
                self.components[start].bits.0 |= (length as u32) << COMPOUND_LENGTH_SHIFT;
            }
            start = index + 1;
        }
    }

    /// Note what matching asks of each compound as a whole, and where each compound starts.
    fn note_compound_facts(&mut self) {
        let mut compounds = Vec::new();
        let mut shapes: crate::css::style::fast_hash::FastMap<Vec<(u32, u32)>, u32> = Default::default();
        let words = self.components.len().div_ceil(64);
        let mut starts = vec![0u64; words];
        let mut start = 0;
        while start < self.components.len() {
            let end = self.compound_end(start);
            starts[start / 64] |= 1 << (start % 64);
            compounds.push(CompoundFacts {
                shape: self.compound_shape(start..end, &mut shapes),
                bloom: self.compound_bloom(start..end),
            });
            start = end;
        }
        let mut ranks = Vec::with_capacity(words);
        let mut before = 0;
        for &word in &starts {
            ranks.push(before);
            before += word.count_ones();
        }
        self.compounds = compounds.into_boxed_slice();
        self.compound_starts = starts.into_boxed_slice();
        self.compound_ranks = ranks.into_boxed_slice();
    }

    /// The dispatch bloom bits of what the compound of `components` tests, as [`CompoundFacts::bloom`] says.
    fn compound_bloom(&self, components: std::ops::Range<usize>) -> u64 {
        use crate::css::style::index::{DispatchKey, dispatch_bloom_bit};
        self.components[components]
            .iter()
            .fold(0, |bloom, component| match component.bits.kind() {
                ComponentKind::Id => bloom | dispatch_bloom_bit(DispatchKey::Id(StyleAtomID(component.data))),
                ComponentKind::Class => bloom | dispatch_bloom_bit(DispatchKey::Class(StyleAtomID(component.data))),
                // NB: A type written in ASCII lowercase is carried as written by every element matching it. A
                //     type qualified by a prefix holds its name in the rare table.
                ComponentKind::Tag
                    if component.bits.has(ASCII_LOWERCASE) && component.bits.namespace() != NamespaceType::Named =>
                {
                    bloom | dispatch_bloom_bit(DispatchKey::TagName(StyleAtomID(component.data)))
                }
                // NB: An attribute written in ASCII lowercase in no namespace is carried under that name by every
                //     element matching it, as one of the name forms of the attribute it holds.
                ComponentKind::Attribute
                    if component.bits.has(ASCII_LOWERCASE)
                        && matches!(component.bits.namespace(), NamespaceType::Default | NamespaceType::None) =>
                {
                    let name = self.attributes[component.data as usize].name.local;
                    bloom | dispatch_bloom_bit(DispatchKey::AttributeName(name))
                }
                ComponentKind::PseudoClass => {
                    match crate::css::style::component_matching::state_tested_by(pseudo_class_from_ffi(
                        component.bits.pseudo(),
                    )) {
                        Some(fact) => bloom | dispatch_bloom_bit(DispatchKey::State(fact)),
                        None => bloom,
                    }
                }
                _ => bloom,
            })
    }

    /// The number of the compound's own tests among those of the arena, as [`CompoundFacts::shape`] says.
    fn compound_shape(
        &self,
        components: std::ops::Range<usize>,
        shapes: &mut crate::css::style::fast_hash::FastMap<Vec<(u32, u32)>, u32>,
    ) -> u32 {
        // NB: Only what a compound's own tests read counts toward its shape: not the combinator its last component
        //     carries, nor the facts the arena notes of a selector or a compound.
        const SHAPE_BITS: u32 = !(RELATION_BITS
            | LAST_IN_LIST
            | NAMES_THE_SCOPE
            | ESCAPES_ITS_NAMESPACE
            | DESCRIBES_ITS_SUBJECT
            | COMPOUND_LENGTH_BITS);
        let reads_its_selector = components.clone().any(|at| match self.components[at].bits.kind() {
            ComponentKind::Nesting
            | ComponentKind::Invalid
            | ComponentKind::RelativeAnchor
            | ComponentKind::PseudoElement => true,
            // NB: What `:host` matches depends on the tree of the sheet the selector's rule decides in.
            ComponentKind::PseudoClass => matches!(
                self.decode(at),
                SimpleSelector::PseudoClass(pseudo_class)
                    if matches!(pseudo_class.pseudo_class, PseudoClassType::Scope | PseudoClassType::Host)
            ),
            _ => false,
        });
        if reads_its_selector {
            return CompoundFacts::NO_SHAPE;
        }
        let key: Vec<(u32, u32)> = self.components[components]
            .iter()
            .map(|component| (component.bits.0 & SHAPE_BITS, component.data))
            .collect();
        let next = index_u32(shapes.len());
        *shapes.entry(key).or_insert(next)
    }
}

impl Selector<'_> {
    /// The selector as a parse tree again, for a transformation that builds another selector from
    /// it. The tree names the arena's atoms, which the arena keeps live while it is packed again.
    pub(crate) fn to_tree(self) -> Arc<tree::CompiledSelector> {
        let compounds: Box<[tree::CompoundSelector]> = self
            .compound_selectors()
            .iter()
            .map(|compound| tree::CompoundSelector {
                combinator: compound.combinator,
                is_implicit_universal_anchor: compound.is_implicit_universal_anchor,
                simple_selectors: compound
                    .simple_selectors
                    .iter()
                    .map(|simple| simple_to_tree(&simple))
                    .collect(),
            })
            .collect();
        tree::CompiledSelector::new(compounds)
    }
}

impl SelectorListView<'_> {
    pub(crate) fn to_tree(self) -> tree::SelectorList {
        self.iter().map(Selector::to_tree).collect()
    }
}

fn simple_to_tree(simple: &SimpleSelector<'_>) -> tree::SimpleSelector {
    use crate::css::selector::SelectorString;
    match *simple {
        SimpleSelector::Universal(name) => tree::SimpleSelector::Universal(Box::new(name)),
        SimpleSelector::TagName(name) => tree::SimpleSelector::TagName(Box::new(name)),
        SimpleSelector::Id(name) => tree::SimpleSelector::Id(name),
        SimpleSelector::Class(name) => tree::SimpleSelector::Class(name),
        SimpleSelector::Attribute(attribute) => tree::SimpleSelector::Attribute(Box::new(tree::AttributeSelector {
            match_type: attribute.match_type,
            qualified_name: attribute.qualified_name,
            value: attribute.value.into(),
            value_identity: attribute.value_identity,
            case_type: attribute.case_type,
        })),
        SimpleSelector::PseudoClass(pseudo_class) => {
            tree::SimpleSelector::PseudoClass(Box::new(tree::PseudoClassSelector {
                pseudo_class: pseudo_class.pseudo_class,
                an_plus_b_pattern: pseudo_class.an_plus_b_pattern,
                argument_selector_list: pseudo_class.argument_selector_list.to_tree(),
                languages: pseudo_class
                    .languages
                    .iter()
                    .map(|range| tree::LanguageRange {
                        value: range.value.into(),
                        is_string: range.is_string,
                    })
                    .collect(),
                direction: pseudo_class.direction,
                identifier: pseudo_class.identifier.map(SelectorString::from),
                identifier_identity: pseudo_class.identifier_identity,
                levels: pseudo_class.levels.into(),
                is_forgiving: pseudo_class.is_forgiving,
            }))
        }
        SimpleSelector::PseudoElement(pseudo_element) => {
            tree::SimpleSelector::PseudoElement(Box::new(tree::PseudoElementSelector {
                pseudo_element: pseudo_element.pseudo_element,
                serialized_name: pseudo_element.serialized_name.map(SelectorString::from),
                value: match pseudo_element.value {
                    PseudoElementValue::None => tree::PseudoElementValue::None,
                    PseudoElementValue::CompoundSelector(selector) => {
                        tree::PseudoElementValue::CompoundSelector(selector.to_tree())
                    }
                    PseudoElementValue::Identifiers(identifiers) => {
                        tree::PseudoElementValue::Identifiers(identifiers.iter().map(SelectorString::from).collect())
                    }
                    PseudoElementValue::TransitionName(argument) => {
                        tree::PseudoElementValue::TransitionName(Box::new(tree::TransitionNameAndClassSelector {
                            name: argument.name.map(|name| match name {
                                TransitionNameSelector::Universal => tree::TransitionNameSelector::Universal,
                                TransitionNameSelector::Name(name) => tree::TransitionNameSelector::Name(name.into()),
                            }),
                            classes: argument.classes.iter().map(SelectorString::from).collect(),
                        }))
                    }
                },
                identifier_identities: pseudo_element.identifier_identities.into(),
            }))
        }
        SimpleSelector::Nesting(_) => tree::SimpleSelector::Nesting,
        SimpleSelector::Invalid(text) => tree::SimpleSelector::Invalid(text.into()),
    }
}

/// The selectors of a list, each found past the last by the lengths of its compounds.
pub struct SelectorsOf<'a> {
    arena: &'a SelectorArena,
    /// Where the next selector starts, or `usize::MAX` past the last.
    at: usize,
}

impl<'a> Iterator for SelectorsOf<'a> {
    type Item = Selector<'a>;

    #[inline]
    fn next(&mut self) -> Option<Selector<'a>> {
        if self.at == usize::MAX {
            return None;
        }
        let components = &self.arena.components;
        let first = self.at;
        let mut at = first;
        loop {
            let end = self.arena.compound_end(at);
            let bits = components[end - 1].bits;
            at = end;
            if bits.relation() == Relation::Leftmost {
                self.at = match bits.has(LAST_IN_LIST) {
                    true => usize::MAX,
                    false => at,
                };
                return Some(Selector {
                    arena: self.arena,
                    at: ComponentOffset(index_u32(first)),
                });
            }
        }
    }
}

impl<'a> IntoIterator for SelectorListView<'a> {
    type Item = Selector<'a>;
    type IntoIter = SelectorsOf<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.arena.selectors_of(self.start)
    }
}

impl<'a> IntoIterator for &SelectorListView<'a> {
    type Item = Selector<'a>;
    type IntoIter = SelectorsOf<'a>;

    fn into_iter(self) -> Self::IntoIter {
        (*self).into_iter()
    }
}

impl PartialEq for SelectorArena {
    fn eq(&self, other: &Self) -> bool {
        self.components == other.components
            && self.qualified == other.qualified
            && self.attributes == other.attributes
            && self.nths == other.nths
            && self.arguments == other.arguments
            && self.pseudo_elements == other.pseudo_elements
            && self.atoms == other.atoms
            && self.numbers == other.numbers
            && self.text_ranges == other.text_ranges
            && self.text == other.text
            && self.rule_lists == other.rule_lists
    }
}

impl PartialEq for SelectorListRef {
    fn eq(&self, other: &Self) -> bool {
        self.start == other.start
            && (Arc::ptr_eq(&self.arena, &other.arena) || frozen(&self.arena) == frozen(&other.arena))
    }
}
