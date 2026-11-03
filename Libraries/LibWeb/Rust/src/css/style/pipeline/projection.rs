/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The document as the engine reads it: each element's local facts and the links between elements,
//! projected from the writes the host makes, in the order it makes them.
//!
//! One row holds everything style reads of one element, addressed by its element slot, so reading
//! an element's facts and following a link out of it are one lookup each. Rows live in a
//! [`CowColumn`]: a fork shares every chunk of rows and copies one only when it writes there, and
//! the variable-length facts of a row are `Arc` slices, which a copied chunk shares in turn. The
//! catalogs keyed by atom (attribute name forms and the texts of values and languages) belong to
//! the document rather than to an element and are shared the same way.
//!
//! A row's facts and its links have lifetimes of their own. An element's links are reset when its
//! identity is minted and when it retires, as the tree changes; its facts exist from the first one
//! published until the element is forgotten at the end of the transaction it departed in, so that
//! invalidation can still read the facts of an element that left.
//!
//! Facts are written as their inputs arrive, ahead of the transaction they join, but style reads
//! them as the last transaction committed them: the projection publishes its rows at each
//! transaction barrier, and the facts readers see are those of that committed generation. An
//! element's links are read the same way; the shadow relations are read as written, as the host's
//! shadow tree changes between barriers are. While a transaction is
//! taken, the generation committed before it stays at hand too: the side of the transaction from
//! before it, which invalidation compares the elements' facts and relations against.

mod fact_lists;
pub(in crate::css::style) use fact_lists::{FactListsIdentity, FactListsTable};
mod tree_reads;
mod writer;

use super::super::element_host_facts::HostFactSet;
use super::super::element_host_facts::HostFactSetID;
use super::super::element_host_facts::HostFactSets;
use super::super::fast_hash::FastMap as HashMap;
use super::super::fast_hash::FastSet as HashSet;
use super::super::index::AttributeFact;
use super::super::index::AttributeNameForms;
use super::super::index::DispatchKey;
use super::super::index::LocalFeatureKey;
use super::super::index::StateSet;
use super::super::index::StyleAtomID;
use super::super::index::dispatch_bloom_bit;
use super::super::input_types::StateFact;
use super::super::input_types::TreeRelations;
use super::super::selector::Incomplete;
use super::super::selector::MatchScratch;
use super::super::selector::NthPosition;
use super::super::selector::PositionalIndexPolicy;
use super::super::selector::RelativeAnswerKey;
use super::super::selector::SiblingAnswerKey;
use super::super::selector::SiblingAnswerTable;
use super::super::selector::SiblingSequenceSource;
use super::super::selector_evaluation::ElementFeatures;
use super::super::selector_evaluation::SelectorBindings;
use super::super::selector_evaluation::SelectorEvaluator;
use super::super::selector_evaluation::SelectorSubject;
use super::super::selector_evaluation::SelectorTree;
use super::super::selector_evaluation::StructuralRead;
use super::super::transaction_view::TransactionFactSide;
use super::super::tree::StyleNodeID;
use super::super::tree::TreeScopeID;
use crate::cow_column::ColumnSnapshot;
use crate::cow_column::CowColumn;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use fact_lists::FactLists;
use fact_lists::FactListsEdit;
use smallvec::SmallVec;
use std::sync::Arc;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
pub(in crate::css::style) use writer::ProjectionPublication;
pub(in crate::css::style) use writer::ProjectionWriter;

/// How many rows a chunk of the projection holds, and so how many rows the first write after a fork
/// copies.
const ROWS_PER_CHUNK: usize = 64;

/// A set of elements whose rows were written, each listed once, with a bit per element slot saying
/// which.
#[derive(Clone, Default)]
struct WrittenRows {
    nodes: Vec<StyleNodeID>,
    bits: Vec<u64>,
}

impl WrittenRows {
    /// Add an element, saying whether it was not in the set yet.
    fn insert(&mut self, node: StyleNodeID, slot: usize) -> bool {
        let (word, bit) = (slot / 64, 1_u64 << (slot % 64));
        if word >= self.bits.len() {
            self.bits.resize(word + 1, 0);
        }
        if self.bits[word] & bit != 0 {
            return false;
        }
        self.bits[word] |= bit;
        self.nodes.push(node);
        true
    }

    fn contains(&self, slot: usize) -> bool {
        self.bits
            .get(slot / 64)
            .is_some_and(|word| word & (1_u64 << (slot % 64)) != 0)
    }

    /// Take the elements out, leaving the set empty.
    fn take(&mut self) -> Vec<StyleNodeID> {
        let nodes = std::mem::take(&mut self.nodes);
        for node in &nodes {
            let slot = node.element_slot();
            self.bits[slot / 64] &= !(1_u64 << (slot % 64));
        }
        nodes
    }

    /// Empty the set, keeping `nodes`, taken from it, as its allocation.
    fn reuse(&mut self, mut nodes: Vec<StyleNodeID>) {
        debug_assert!(self.nodes.is_empty());
        nodes.clear();
        self.nodes = nodes;
    }
}

/// The elements written since the last barrier, in the order of their writes, once per write, of which the first
/// `checked` were found to leave their rows as the committed generation has them, until written again.
#[derive(Default)]
struct PendingChangeScan {
    writes: Vec<StyleNodeID>,
    checked: std::sync::atomic::AtomicUsize,
}

impl Clone for PendingChangeScan {
    fn clone(&self) -> Self {
        Self {
            writes: self.writes.clone(),
            checked: self.checked.load(std::sync::atomic::Ordering::Relaxed).into(),
        }
    }
}

impl PendingChangeScan {
    fn clear(&mut self) {
        self.writes.clear();
        *self.checked.get_mut() = 0;
    }

    fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.writes)
    }
}

/// A key of an element, which a compound can require of the elements it matches: a name its row holds, or the
/// element-backed pseudo-element of its shadow host it stands for.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub(in crate::css::style) enum CarriedKey {
    Tag(StyleAtomID),
    Class(StyleAtomID),
    Id(StyleAtomID),
    BackedPseudoElement(u8),
}

/// The keys a [`Projection`] notes the elements of: those that carried one, or carry one, and whose keys moved.
#[derive(Default)]
pub(in crate::css::style) struct WatchedKeys {
    keys: HashSet<CarriedKey>,
    /// A Bloom summary of the names among the keys, which rules out most elements without a lookup.
    names_bloom: u64,
    watches_pseudo_elements: bool,
}

impl WatchedKeys {
    pub(in crate::css::style) fn new(keys: impl IntoIterator<Item = CarriedKey>) -> Self {
        let keys: HashSet<CarriedKey> = keys.into_iter().collect();
        let mut watched = Self {
            names_bloom: 0,
            watches_pseudo_elements: false,
            keys: HashSet::default(),
        };
        for &key in &keys {
            match key {
                CarriedKey::Tag(tag) => watched.names_bloom |= dispatch_bloom_bit(DispatchKey::TagName(tag)),
                CarriedKey::Class(class) => watched.names_bloom |= dispatch_bloom_bit(DispatchKey::Class(class)),
                CarriedKey::Id(id) => watched.names_bloom |= dispatch_bloom_bit(DispatchKey::Id(id)),
                CarriedKey::BackedPseudoElement(_) => watched.watches_pseudo_elements = true,
            }
        }
        watched.keys = keys;
        watched
    }

    /// Whether an element with `facts` carries a key, without reading more of it than its names where its Bloom
    /// summary says it may.
    fn carried_by(&self, facts: &ElementFacts, host_fact_sets: &HostFactSets) -> bool {
        (facts.dispatch_bloom & self.names_bloom != 0 && facts.named_keys().any(|key| self.keys.contains(&key)))
            || (self.watches_pseudo_elements
                && backed_pseudo_element_key(facts, host_fact_sets).is_some_and(|key| self.keys.contains(&key)))
    }
}

fn backed_pseudo_element_key(facts: &ElementFacts, host_fact_sets: &HostFactSets) -> Option<CarriedKey> {
    host_fact_sets
        .get(facts.host_facts)
        .associated_pseudo_kind()
        .map(CarriedKey::BackedPseudoElement)
}

/// Whether the two say the same of the keys their elements carry.
fn carry_the_same_keys(was: &ElementFacts, is: &ElementFacts, host_fact_sets: &HostFactSets) -> bool {
    was.tag == is.tag
        && was.folded_tag() == is.folded_tag()
        && was.id() == is.id()
        && was.classes() == is.classes()
        && backed_pseudo_element_key(was, host_fact_sets) == backed_pseudo_element_key(is, host_fact_sets)
}

/// A list of atoms or pairs a row holds, shared by every copy of the row. An empty one holds no
/// allocation.
#[derive(Clone, PartialEq)]
struct SharedList<T>(Option<Arc<[T]>>);

impl<T> Default for SharedList<T> {
    fn default() -> Self {
        Self(None)
    }
}

impl<T: Clone> SharedList<T> {
    fn new(values: &[T]) -> Self {
        Self((!values.is_empty()).then(|| values.into()))
    }

    fn as_slice(&self) -> &[T] {
        self.0.as_deref().unwrap_or_default()
    }
}

/// What one element publishes of itself, which its selectors test.
#[derive(Clone, Default, PartialEq)]
pub(in crate::css::style) struct ElementFacts {
    tag: StyleAtomID,
    /// The `ElementFacts::RESIDENT` and the other flags.
    flags: u8,
    /// The element's heading level, 1 to 9, or zero where it is not a heading.
    heading_level: u8,
    /// The identity of the set of facts the host publishes of the element beside the ones its selectors test, among
    /// the document's sets: what its box and its style adjustments read of it, and the element-backed pseudo-element
    /// of its shadow host it stands for.
    host_facts: HostFactSetID,
    states: StateSet,
    /// The element's id, namespace, language, directionality, tree scope and assigned slot, and its
    /// classes, attributes, parts and custom states, with its folded tag and part exposure, interned
    /// by content.
    lists: FactLists,
    /// A Bloom summary of the dispatch keys the element carries, document-root status aside: those
    /// its lists summarize, and its own. Kept beside the facts, as an ancestor walk reads it for
    /// every ancestor and the lists may be a cache miss away.
    dispatch_bloom: u64,
}

const _: () = assert!(size_of::<ElementFacts>() == 32);

impl ElementFacts {
    /// Whether anything was published about the element, or it is a shadow root standing in the
    /// tree. An element without facts is one the projection cannot answer for, which is not the same
    /// as an element that has none.
    const RESIDENT: u8 = 1 << 0;
    /// Whether a text or comment child keeps the element from being `:empty`.
    const HAS_TEXT_CONTENT: u8 = 1 << 1;
    /// Whether the element is a `<slot>`. A slot inside a shadow tree is not itself a flattened
    /// slottable, so `::slotted()` never names one even when its assignment carries a chain onward.
    const IS_SLOT: u8 = 1 << 2;
    /// Whether the row's identity is live, which belongs to its links rather than its facts and is
    /// kept here where the row has room for it.
    const LIVE: u8 = 1 << 3;
    /// Whether the element stands in a tree: it was inserted and has not left. An identity is
    /// minted as its node arrives, before its relations are published. Like `LIVE`, of its links.
    const CONNECTED: u8 = 1 << 4;

    fn flag(&self, flag: u8) -> bool {
        self.flags & flag != 0
    }

    fn set_flag(&mut self, flag: u8, value: bool) {
        match value {
            true => self.flags |= flag,
            false => self.flags &= !flag,
        }
    }

    fn resident(&self) -> bool {
        self.flag(Self::RESIDENT)
    }

    /// Whether the two say the same of their elements, leaving aside what they keep of their links: whether the
    /// identity is live and stands in a tree, its tree scope and its assigned slot.
    fn says_the_same_as(&self, other: &Self) -> bool {
        const LINK_FLAGS: u8 = ElementFacts::LIVE | ElementFacts::CONNECTED;
        self.tag == other.tag
            && self.flags & !LINK_FLAGS == other.flags & !LINK_FLAGS
            && self.heading_level == other.heading_level
            && self.host_facts == other.host_facts
            && self.states == other.states
            && self.lists.says_the_same_as(&other.lists)
    }

    /// Summarizes the dispatch keys the element carries again: those of its lists, which the lists summarize where they
    /// are made, and its own few.
    fn refresh_dispatch_bloom(&mut self) {
        let mut bloom = self.lists.bloom()
            | dispatch_bloom_bit(DispatchKey::Universal)
            | dispatch_bloom_bit(DispatchKey::TagName(self.tag));
        if self.heading_level() != 0 {
            bloom |= dispatch_bloom_bit(DispatchKey::Heading);
        }
        for fact in self.states.facts() {
            bloom |= dispatch_bloom_bit(DispatchKey::State(fact));
        }
        self.dispatch_bloom = bloom;
    }

    /// The facts the element is left with once they are forgotten: none, but what they keep of its links, and the host
    /// facts, which the element keeps until its identity is minted again, for what reads the departed element.
    fn forgotten(&self) -> Self {
        Self {
            flags: self.flags & (Self::LIVE | Self::CONNECTED),
            host_facts: self.host_facts,
            ..Self::default()
        }
    }

    #[must_use]
    pub(in crate::css::style) fn tag(&self) -> StyleAtomID {
        self.tag
    }

    #[must_use]
    pub(in crate::css::style) fn namespace(&self) -> StyleAtomID {
        self.lists.namespace()
    }

    #[must_use]
    pub(in crate::css::style) fn id(&self) -> StyleAtomID {
        self.lists.id()
    }

    #[must_use]
    pub(in crate::css::style) fn states(&self) -> StateSet {
        self.states
    }

    #[must_use]
    pub(in crate::css::style) fn language(&self) -> StyleAtomID {
        self.lists.language()
    }

    #[must_use]
    pub(in crate::css::style) fn directionality(&self) -> StyleAtomID {
        self.lists.directionality()
    }

    #[must_use]
    pub(in crate::css::style) fn part_exposure(&self) -> StyleAtomID {
        self.lists.part_exposure()
    }

    /// The ASCII-lowercase folding of the element's tag, where it differs from it.
    #[must_use]
    pub(in crate::css::style) fn folded_tag(&self) -> StyleAtomID {
        self.lists.folded_tag()
    }

    /// What is equal between two elements only if their lists are the same ones, and so everything
    /// they hold is: for as long as both stand in rows.
    #[must_use]
    pub(in crate::css::style) fn lists_identity(&self) -> FactListsIdentity {
        self.lists.identity()
    }

    #[must_use]
    pub(in crate::css::style) fn heading_level(&self) -> u8 {
        self.heading_level
    }

    #[must_use]
    pub(in crate::css::style) fn is_slot(&self) -> bool {
        self.flag(Self::IS_SLOT)
    }

    #[must_use]
    pub(in crate::css::style) fn has_text_content(&self) -> bool {
        self.flag(Self::HAS_TEXT_CONTENT)
    }

    #[must_use]
    pub(in crate::css::style) fn classes(&self) -> &[StyleAtomID] {
        self.lists.classes()
    }

    #[must_use]
    pub(in crate::css::style) fn attributes(&self) -> &[AttributeFact] {
        self.lists.attributes()
    }

    #[must_use]
    pub(in crate::css::style) fn parts(&self) -> &[StyleAtomID] {
        self.lists.parts()
    }

    #[must_use]
    pub(in crate::css::style) fn custom_states(&self) -> &[StyleAtomID] {
        self.lists.custom_states()
    }

    /// The keys of the element that name something of its own.
    fn named_keys(&self) -> impl Iterator<Item = CarriedKey> + '_ {
        let (folded_tag, id) = (self.folded_tag(), self.id());
        [
            Some(CarriedKey::Tag(self.tag)),
            (!folded_tag.is_none()).then_some(CarriedKey::Tag(folded_tag)),
            (!id.is_none()).then_some(CarriedKey::Id(id)),
        ]
        .into_iter()
        .flatten()
        .chain(self.classes().iter().copied().map(CarriedKey::Class))
    }

    /// The Bloom summary of the dispatch keys the element carries.
    #[must_use]
    pub(in crate::css::style) fn dispatch_bloom(&self, is_document_root: bool) -> u64 {
        self.dispatch_bloom
            | match is_document_root {
                true => dispatch_bloom_bit(DispatchKey::Root),
                false => 0,
            }
    }
}

/// How one element stands in the tree. Its tree scope and assigned slot are held in its fact lists,
/// and whether it is live and connected in its facts' flags, so that a row is 48 bytes.
#[derive(Clone, Default, PartialEq)]
struct Links {
    parent: Option<StyleNodeID>,
    first_element_child: Option<StyleNodeID>,
    previous_element_sibling: Option<StyleNodeID>,
    next_element_sibling: Option<StyleNodeID>,
}

const _: () = assert!(size_of::<Links>() == 16);

/// The shadow relations of an element, which few elements have.
#[derive(Clone, Default, PartialEq)]
struct ShadowLinks {
    /// A shadow host's shadow root.
    shadow_root: Option<StyleNodeID>,
    /// A shadow root's host.
    host: Option<StyleNodeID>,
    /// A slot's assigned nodes, text nodes included, in the order the DOM assigned them.
    assigned_nodes: SharedList<StyleNodeID>,
    /// Every part name the element is addressable by, each with the host of the level that name
    /// reaches.
    part_hosts: SharedList<(StyleAtomID, StyleNodeID)>,
}

#[derive(Clone, Default, PartialEq)]
struct Row {
    facts: ElementFacts,
    links: Links,
}

const _: () = assert!(size_of::<Row>() == 48);

/// The shadow relations of the elements by slot, in a column of their own that is made only as far as the last element
/// holding any: most documents have none, and most elements of those that do hold none either.
type ShadowColumn = CowColumn<Option<Arc<ShadowLinks>>, ROWS_PER_CHUNK>;
type ShadowSnapshot = ColumnSnapshot<Option<Arc<ShadowLinks>>, ROWS_PER_CHUNK>;

fn shadow_links_heap_size(shadow: Option<&Arc<ShadowLinks>>, sizer: &mut crate::heap_size::HeapSizer) -> usize {
    shadow.map_or(0, |shadow| {
        if !sizer.first_sight(Arc::as_ptr(shadow)) {
            return 0;
        }
        crate::heap_size::ARC_HEADER_BYTES
            + size_of::<ShadowLinks>()
            + shadow.assigned_nodes.heap_size(sizer)
            + shadow.part_hosts.heap_size(sizer)
    })
}

impl Row {
    /// Whether the row's element identity is live: minted and not retired.
    fn live(&self) -> bool {
        self.facts.flag(ElementFacts::LIVE)
    }

    /// Whether the row's element stands in a tree.
    fn connected(&self) -> bool {
        self.facts.flag(ElementFacts::CONNECTED)
    }
}

/// What a document's atoms spell, for the tests and substitutions that compare text.
#[derive(Clone, Default)]
struct Catalogs {
    name_forms: Catalog<AttributeNameForms>,
    /// The local name an `attr()` reads an attribute in no namespace by.
    substitution_names: Catalog<Box<[u16]>>,
    /// What attribute-value atoms spell: the host's own string, which the document's attribute
    /// holds as well, shared by reference.
    value_texts: Catalog<RetainedUtf16FlyString>,
    language_texts: Catalog<Box<[u16]>>,
}

/// A catalog the writer adds to between publications, which every publication shares: a large base and the small part
/// added since the base was last folded, so that an addition after a publication copies only the small part. The part
/// is folded into the base once it grows past an eighth of it.
#[derive(Clone)]
struct Catalog<T> {
    base: Arc<HashMap<StyleAtomID, T>>,
    recent: Arc<HashMap<StyleAtomID, T>>,
    /// How many entries additions copied, for a test.
    #[cfg(test)]
    copied: usize,
}

impl<T> Default for Catalog<T> {
    fn default() -> Self {
        Self {
            base: Arc::default(),
            recent: Arc::default(),
            #[cfg(test)]
            copied: 0,
        }
    }
}

impl<T: Clone> Catalog<T> {
    fn get(&self, atom: &StyleAtomID) -> Option<&T> {
        self.recent.get(atom).or_else(|| self.base.get(atom))
    }

    fn contains_key(&self, atom: &StyleAtomID) -> bool {
        self.get(atom).is_some()
    }

    /// Every entry, once, the recent spelling of an atom standing for the one in the base.
    fn iter(&self) -> impl Iterator<Item = (&StyleAtomID, &T)> {
        self.recent
            .iter()
            .chain(self.base.iter().filter(|(atom, _)| !self.recent.contains_key(atom)))
    }

    /// Notes, for a test, how many entries writing `map` now copies.
    #[cfg(test)]
    fn note_copy(copied: &mut usize, map: &Arc<HashMap<StyleAtomID, T>>) {
        if Arc::strong_count(map) > 1 {
            *copied += map.len();
        }
    }

    fn insert(&mut self, atom: StyleAtomID, value: T) {
        #[cfg(test)]
        Self::note_copy(&mut self.copied, &self.recent);
        Arc::make_mut(&mut self.recent).insert(atom, value);
        if self.recent.len() > self.base.len() / 8 + 64 {
            #[cfg(test)]
            Self::note_copy(&mut self.copied, &self.base);
            let recent = std::mem::take(&mut self.recent);
            let base = Arc::make_mut(&mut self.base);
            match Arc::try_unwrap(recent) {
                Ok(recent) => base.extend(recent),
                Err(recent) => base.extend(recent.iter().map(|(atom, value)| (*atom, value.clone()))),
            }
        }
    }

    /// Drops whatever the catalog holds of `atoms`.
    fn forget(&mut self, atoms: &[StyleAtomID]) {
        for map in [&mut self.base, &mut self.recent] {
            if atoms.iter().any(|atom| map.contains_key(atom)) {
                let map = Arc::make_mut(map);
                for atom in atoms {
                    map.remove(atom);
                }
            }
        }
    }

    /// The bytes of the catalog's maps that `sizer` reaches for the first time.
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        [&self.base, &self.recent]
            .into_iter()
            .map(|map| sizer.arc_shallow(map) + crate::heap_size::map_bytes(map))
            .sum()
    }

    fn values(&self) -> impl Iterator<Item = &T> {
        self.iter().map(|(_, value)| value)
    }
}

/// The elements that match `:hover`, which the render owner places as the pointer moves rather than the host writes:
/// a small overlay over the rows, holding the hover chain as placed, and for each generation style reads the hovered
/// elements' facts with the hover fact set. A fork carries it whole, as it is the size of one chain.
#[derive(Clone, Default)]
struct HoverOverlay {
    /// The chain as last placed, which the next transaction takes.
    placed: SmallVec<[StyleNodeID; 16]>,
    committed: HoveredFacts,
    before: HoveredFacts,
    /// The chain as the latest geometry read sealed it, beside the sealed rows.
    sealed: Option<SmallVec<[StyleNodeID; 16]>>,
}

/// The facts of the hovered elements of one generation, the hover fact set, which stand in for their rows' facts.
#[derive(Clone, Default)]
struct HoveredFacts {
    facts: Vec<(StyleNodeID, ElementFacts)>,
    /// A bit per element slot modulo 64, which rules out most elements without a search.
    slots: u64,
}

impl HoveredFacts {
    /// The hovered elements of `chain` that `generation` holds facts of, with the hover fact set.
    fn of(chain: &[StyleNodeID], generation: &ColumnSnapshot<Row, ROWS_PER_CHUNK>) -> Self {
        let mut hovered = Self::default();
        for &node in chain {
            let Some(row) = generation.get(node.element_slot()).filter(|row| row.facts.resident()) else {
                continue;
            };
            let mut facts = row.facts.clone();
            facts.states.insert(StateFact::Hover);
            facts.refresh_dispatch_bloom();
            hovered.slots |= 1 << (node.element_slot() % 64);
            hovered.facts.push((node, facts));
        }
        hovered
    }

    #[inline]
    fn get(&self, node: StyleNodeID) -> Option<&ElementFacts> {
        if self.slots & (1 << (node.element_slot() % 64)) == 0 {
            return None;
        }
        self.facts
            .iter()
            .find_map(|(hovered, facts)| (*hovered == node).then_some(facts))
    }

    fn contains(&self, node: StyleNodeID) -> bool {
        self.get(node).is_some()
    }
}

/// The generations a [`Projection`] reads its facts, links and shadow relations from, held so that
/// no later generation takes their place in memory while a memo compares against them.
#[derive(Clone)]
pub(in crate::css::style) struct ProjectionGeneration {
    rows: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    committed: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    shadows: ShadowSnapshot,
}

impl ProjectionGeneration {
    /// The element `node` inherits from in this generation, as [`Projection::inheritance_parent`] answers for an
    /// element.
    pub(in crate::css::style) fn element_inheritance_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        let row = self.committed.get(node.element_slot())?;
        if let Some(slot) = row.facts.lists.assigned_slot() {
            return Some(slot);
        }
        let parent = row.links.parent?;
        let host = self
            .shadows
            .get(parent.element_slot())?
            .as_deref()
            .and_then(|shadow| shadow.host);
        Some(host.unwrap_or(parent))
    }
}

/// What the selectors matched against an element read of its children's places among their siblings, and of itself
/// whether it is empty: the changes to its children that can move what they matched.
#[derive(Clone, Copy, PartialEq, Eq, Default, Debug)]
pub(in crate::css::style) struct StructuralFlags(u8);

impl StructuralFlags {
    /// A selector read a child's earlier siblings, across a `+` or a `~`, or rejected a child on what they carry.
    pub(in crate::css::style) const CHILDREN_READ_SIBLINGS: Self = Self(1 << 0);
    /// A selector read a child's previous sibling, across a `+`.
    pub(in crate::css::style) const CHILDREN_READ_PREVIOUS_SIBLING: Self = Self(1 << 1);
    /// A selector read where a child stands among its siblings.
    pub(in crate::css::style) const CHILDREN_READ_POSITIONS: Self = Self(1 << 2);
    /// A selector read whether the element is `:empty`.
    pub(in crate::css::style) const READ_EMPTINESS: Self = Self(1 << 3);
    /// The record of a child, or of a pseudo-element of it, computes from its place among its siblings.
    pub(in crate::css::style) const CHILD_READS_SIBLING_POSITION: Self = Self(1 << 4);
    /// A selector read where a child stands counted from the end.
    pub(in crate::css::style) const CHILDREN_READ_POSITIONS_FROM_END: Self = Self(1 << 5);
    /// A selector read where a child stands among those of its type.
    pub(in crate::css::style) const CHILDREN_READ_POSITIONS_OF_TYPE: Self = Self(1 << 6);
    /// A selector read more of where a child stands than whether it stands first, in the way it counts.
    pub(in crate::css::style) const CHILDREN_READ_POSITIONS_BEYOND_EDGES: Self = Self(1 << 7);
    const ALL: Self = Self(0xff);

    #[must_use]
    const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    #[must_use]
    pub(in crate::css::style) fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

/// The structural flags of the elements by slot, which selectors matched on any thread set, and which are never
/// cleared while the element's records may hold what its children matched: a flag set for nothing only costs a
/// change to the element's children the work it does without the flag.
#[derive(Default)]
struct StructuralFlagsColumn(Box<[AtomicU8]>);

impl StructuralFlagsColumn {
    /// The column for `slot_count` slots, holding what this one holds.
    fn grown_to(&self, slot_count: usize) -> Self {
        let length = slot_count.max(self.0.len().saturating_mul(2)).max(64);
        Self(
            (0..length)
                .map(|slot| AtomicU8::new(self.0.get(slot).map_or(0, |flags| flags.load(Ordering::Relaxed))))
                .collect(),
        )
    }
}

/// The projection of a document's elements that the style engine reads: the rows as the host last published them, which
/// only the host's [`ProjectionWriter`] writes, with the generations transactions take of them.
#[derive(Clone, Default)]
pub(in crate::css::style) struct Projection {
    /// The rows as the host last published them.
    rows: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    /// The elements' shadow relations as the host last published them, which are read as published.
    shadows: ShadowSnapshot,
    /// The rows as the last transaction published them, whose facts style reads. It shares every
    /// chunk `rows` has not written since.
    committed: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    /// The rows as the transaction being taken found them, until it is released.
    before: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    /// One past the greatest element slot written.
    slot_count: usize,
    catalogs: Catalogs,
    /// The distinct sets of host facts the rows name, as the host last published them.
    host_fact_sets: Arc<HostFactSets>,
    /// A slotted text node's slot. Text nodes have no rows, and a slotted one is rare.
    text_assigned_slots: Arc<HashMap<StyleNodeID, StyleNodeID>>,
    /// The elements whose facts say they are disabled, as the host last published them.
    disabled: Arc<HashSet<StyleNodeID>>,
    /// The elements whose rows the host's publications wrote since the last barrier.
    written: WrittenRows,
    /// Each write since the last barrier, which whether a written row changes the committed
    /// generation is asked of at most once, as long as it is not written again.
    pending_change_scan: PendingChangeScan,
    /// The elements whose rows were written since the last barrier or geometry read seal, whichever
    /// came last: those whose changes the pending transaction has not yet been asked about.
    written_since_seal: WrittenRows,
    /// The rows as the latest geometry read sealed them, where it sealed a change the next
    /// transaction has yet to take: the style change event a geometry read established without
    /// settling style. The transaction that flushes the seal takes these rows alone, and what was
    /// written after the seal waits for the transaction after it.
    sealed: Option<ColumnSnapshot<Row, ROWS_PER_CHUNK>>,
    /// Whether the transaction being taken is the one that flushes the seal.
    flushing_seal: bool,
    /// What the transaction being taken changed, element by element, until it is released.
    changes: Vec<(StyleNodeID, ElementChange)>,
    /// The elements the render owner's hover places in the hover state, beside the rows.
    hover: HoverOverlay,
    /// The keys the transactions note the elements of, as the hover seal last said.
    watched_keys: Option<Arc<WatchedKeys>>,
    /// The elements the transactions since the hover seal last asked moved the watched keys of.
    watched_key_writes: WrittenRows,
    /// The elements whose links were written since the last transaction was released: those the
    /// tree has to settle at the next barrier, and those it settled at the barrier of the
    /// transaction being taken.
    links_written: WrittenRows,
    /// Of those, the elements whose own relations were written, rather than only their first element
    /// child: an element whose children arrive before it does stands in no tree yet, and has not left
    /// one either.
    relations_written: WrittenRows,
    /// How many elements' links were written since the last transaction was released, which the
    /// document's host reads: each may deepen the tree by a level once the tree settles it.
    link_rows_written: Arc<AtomicU32>,
    /// What the selectors matched against each element read of its structure, which matching on any thread sets.
    /// A fork shares it, where what one side sets only costs the other the work a flag lets it skip.
    structural_flags: Arc<StructuralFlagsColumn>,
    /// The host's writer of a test, whose writes are published to the projection as they are made.
    #[cfg(test)]
    test_writer: Box<ProjectionWriter>,
}

/// Everything an element arrives with that its selectors test, which its row takes in one write.
pub(in crate::css::style) struct ArrivalFacts<'a> {
    pub tag: StyleAtomID,
    /// The ASCII-lowercase folding of the tag, or the tag itself where folding leaves it as it is.
    pub folded_tag: StyleAtomID,
    pub namespace: StyleAtomID,
    pub id: StyleAtomID,
    pub language: StyleAtomID,
    pub directionality: StyleAtomID,
    pub heading_level: u8,
    pub is_slot: bool,
    pub has_text_content: bool,
    pub states: StateSet,
    pub classes: &'a [StyleAtomID],
    pub attributes: &'a [AttributeFact],
    pub custom_states: &'a [StyleAtomID],
}

/// What a transaction changed of one element: how the projection's side from before the
/// transaction and the generation it committed differ there.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::css::style) enum ElementChange {
    /// The element's relations, before and now, none on a side where it stood in no tree.
    Relations(Option<TreeRelations>, Option<TreeRelations>),
    /// The element arrived with facts, which its arrival says all of.
    ArrivingFacts,
    /// One local fact of an element that stood where it was.
    Feature(LocalFeatureKey),
    /// The element's id, before and now.
    Id(StyleAtomID, StyleAtomID),
    State(StateFact),
}

impl<T> SharedList<T> {
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.0.as_ref().map_or(0, |list| sizer.arc_shallow(list))
    }
}

impl Row {
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.facts.lists.heap_size(sizer)
    }
}

impl WrittenRows {
    fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.nodes) + crate::heap_size::vec_bytes(&self.bits)
    }
}

impl Projection {
    /// Counts the rows of every generation the projection holds, each chunk once, and its
    /// catalogs and change sets, with the live elements it holds.
    pub(in crate::css::style) fn report_memory(&self, report: &mut crate::css::style::memory_report::MemoryReport) {
        use crate::heap_size::map_bytes;
        let owner = crate::css::style::memory_report::MemoryOwner::Projection;
        let sizer = &mut report.sizer;
        let mut bytes = self.rows.heap_size(sizer, Row::heap_size)
            + self
                .shadows
                .heap_size(sizer, |shadow, sizer| shadow_links_heap_size(shadow.as_ref(), sizer));
        // NB: The generations a transaction or a geometry read holds share the chunks the rows
        //     were not written in since, and hold the others only until they are released.
        let generations = self.committed.heap_size(sizer, Row::heap_size)
            + self.before.heap_size(sizer, Row::heap_size)
            + self
                .sealed
                .as_ref()
                .map_or(0, |sealed| sealed.heap_size(sizer, Row::heap_size));
        report.add(
            crate::css::style::memory_report::MemoryOwner::ProjectionGenerations,
            generations,
        );
        let sizer = &mut report.sizer;
        if sizer.arc_shallow(&self.host_fact_sets) != 0 {
            bytes += crate::heap_size::ARC_HEADER_BYTES + size_of::<HostFactSets>() + self.host_fact_sets.heap_size();
        }
        let catalogs = &self.catalogs;
        bytes += catalogs.name_forms.heap_size(sizer);
        // NB: The value texts are the host's strings, which its attributes hold too.
        bytes += catalogs.value_texts.heap_size(sizer);
        for texts in [&catalogs.substitution_names, &catalogs.language_texts] {
            bytes += texts.heap_size(sizer);
            bytes += texts.values().map(|text| size_of_val::<[u16]>(text)).sum::<usize>();
        }
        bytes += sizer.arc_shallow(&self.text_assigned_slots) + map_bytes(&self.text_assigned_slots);
        bytes += sizer.arc_shallow(&self.disabled) + crate::heap_size::set_bytes(&self.disabled);
        if let Some(watched_keys) = &self.watched_keys {
            bytes += sizer.arc_shallow(watched_keys) + crate::heap_size::set_bytes(&watched_keys.keys);
        }
        if sizer.arc_shallow(&self.structural_flags) != 0 {
            bytes += crate::heap_size::ARC_HEADER_BYTES
                + size_of::<StructuralFlagsColumn>()
                + size_of_val::<[AtomicU8]>(&self.structural_flags.0);
        }
        bytes += [
            &self.written,
            &self.written_since_seal,
            &self.links_written,
            &self.relations_written,
            &self.watched_key_writes,
        ]
        .iter()
        .map(|rows| rows.heap_size())
        .sum::<usize>();
        bytes += self.pending_change_scan.heap_size();
        bytes += crate::heap_size::vec_bytes(&self.changes);
        report.add(owner, bytes);
        report.elements += (0..self.slot_count)
            .filter(|&slot| self.rows.get(slot).is_some_and(Row::live))
            .count();
    }
}

impl Projection {
    fn row(&self, node: StyleNodeID) -> Option<&Row> {
        self.rows.get(node.element_slot())
    }

    /// Note that the row of `node`, at `slot`, is written in the transaction to come.
    fn note_written(&mut self, node: StyleNodeID, slot: usize) {
        self.written.insert(node, slot);
        self.written_since_seal.insert(node, slot);
    }

    /// The links of `node` as the last transaction committed them.
    /// Whether the rows stand as they did in `generation`, without taking a generation to compare.
    #[must_use]
    pub(in crate::css::style) fn is_at_generation(&self, generation: &ProjectionGeneration) -> bool {
        generation.rows.is_same_generation_as(&self.rows)
            && generation.committed.is_same_generation_as(&self.committed)
            && generation.shadows.is_same_generation_as(&self.shadows)
    }

    /// The generations of the rows, as the host last published them and as the last transaction
    /// committed them, and of the shadow relations: what a read of the facts, links and shadow
    /// relations reads, which reads the same where the generations are the same.
    pub(in crate::css::style) fn generation(&self) -> ProjectionGeneration {
        ProjectionGeneration {
            rows: self.rows.clone(),
            committed: self.committed.clone(),
            shadows: self.shadows.clone(),
        }
    }

    fn links(&self, node: StyleNodeID) -> Option<&Links> {
        Some(&self.committed.get(node.element_slot())?.links)
    }

    fn shadow(&self, node: StyleNodeID) -> Option<&ShadowLinks> {
        self.shadows.get(node.element_slot())?.as_deref()
    }

    // -- Identity lifecycle --------------------------------------------------------------------

    /// Take a transaction at its barrier: what was committed before it becomes its side from before
    /// it, and the facts written so far are the ones style reads. A published generation shares
    /// every chunk of rows with the written one until the written one writes there.
    ///
    /// The transaction that flushes a geometry read's seal publishes the sealed rows instead, and
    /// the elements written after the seal stay written for the transaction after it.
    pub(in crate::css::style) fn commit(&mut self) {
        let committed = match self.flushing_seal {
            true => self.sealed.clone().expect("a flushed seal holds its rows"),
            false => self.rows.clone(),
        };
        self.before = std::mem::replace(&mut self.committed, committed);
        let chain = match self.flushing_seal {
            true => self
                .hover
                .sealed
                .as_deref()
                .expect("a flushed seal holds its hover chain"),
            false => &self.hover.placed,
        };
        let hovered = HoveredFacts::of(chain, &self.committed);
        self.hover.before = std::mem::replace(&mut self.hover.committed, hovered);
        let mut written = self.written.take();
        written.sort_unstable();
        self.pending_change_scan.clear();
        self.changes.clear();
        for pass in ChangePass::ALL {
            for &node in &written {
                let slot = node.element_slot();
                let before = self.before.get(slot);
                let after = self.committed.get(slot);
                let hovered = (self.hover.before.contains(node), self.hover.committed.contains(node));
                note_changes(pass, node, (before, after), hovered, &mut self.changes);
            }
        }
        self.note_watched_key_writes(&written);
        self.written.reuse(written);
        if self.flushing_seal {
            for index in 0..self.written_since_seal.nodes.len() {
                let node = self.written_since_seal.nodes[index];
                self.written.insert(node, node.element_slot());
                self.pending_change_scan.writes.push(node);
            }
        } else {
            self.forget_written_since_seal();
        }
    }

    /// Note those of the elements `written`, in the transaction being taken, that carried a watched key or carry one,
    /// and whose keys moved.
    fn note_watched_key_writes(&mut self, written: &[StyleNodeID]) {
        let Some(watched) = self.watched_keys.as_ref().filter(|watched| !watched.keys.is_empty()) else {
            return;
        };
        let sets = &*self.host_fact_sets;
        let carries = |row: Option<&Row>| row.is_some_and(|row| watched.carried_by(&row.facts, sets));
        let empty = ElementFacts::default();
        for &node in written {
            let slot = node.element_slot();
            let (before, after) = (self.before.get(slot), self.committed.get(slot));
            if (carries(before) || carries(after))
                && !carry_the_same_keys(
                    before.map_or(&empty, |row| &row.facts),
                    after.map_or(&empty, |row| &row.facts),
                    sets,
                )
            {
                self.watched_key_writes.insert(node, slot);
            }
        }
    }

    /// Have the transactions from here on note the elements of `keys`, in place of those they noted till now.
    pub(in crate::css::style) fn watch_keys(&mut self, keys: Arc<WatchedKeys>) {
        self.watched_keys = Some(keys);
    }

    /// Whether the transactions noted the elements of `keys` all along since the hover seal said to.
    #[must_use]
    pub(in crate::css::style) fn watches_keys(&self, keys: &Arc<WatchedKeys>) -> bool {
        self.watched_keys
            .as_ref()
            .is_some_and(|watched| Arc::ptr_eq(watched, keys))
    }

    /// The elements noted since this was last asked, each once.
    pub(in crate::css::style) fn take_watched_key_writes(&mut self) -> Vec<StyleNodeID> {
        self.watched_key_writes.take()
    }

    /// Hand back the list [`Self::take_watched_key_writes`] answered, as the next one's allocation.
    pub(in crate::css::style) fn reuse_watched_key_writes(&mut self, writes: Vec<StyleNodeID>) {
        self.watched_key_writes.reuse(writes);
    }

    /// The elements that carry one of `keys` as the last transaction committed them.
    pub(in crate::css::style) fn committed_carriers_of<'a>(
        &'a self,
        keys: &'a WatchedKeys,
    ) -> impl Iterator<Item = StyleNodeID> + 'a {
        let sets = &*self.host_fact_sets;
        (1..self.slot_count).filter_map(move |slot| {
            let row = self.committed.get(slot)?;
            keys.carried_by(&row.facts, sets)
                .then(|| StyleNodeID::element(u32::try_from(slot).expect("element slot exceeds u32")))
        })
    }

    fn forget_written_since_seal(&mut self) {
        let written = self.written_since_seal.take();
        self.written_since_seal.reuse(written);
    }

    /// Seal what was written so far for a geometry read that found none of it could move geometry:
    /// [`Self::any_change_since_seal`] asks only about rows written after this.
    /// The transaction a later style change event flushes the seal with takes the rows as they
    /// are now, where they hold a change at all.
    pub(in crate::css::style) fn seal_for_geometry_read(&mut self) {
        debug_assert!(!self.flushing_seal);
        self.forget_written_since_seal();
        self.sealed = self.has_pending_changes().then(|| self.rows.clone());
        self.hover.sealed = self.sealed.is_some().then(|| self.hover.placed.clone());
    }

    /// Whether a geometry read sealed a change that no transaction has taken yet.
    #[must_use]
    pub(in crate::css::style) fn has_sealed_changes(&self) -> bool {
        !self.flushing_seal && self.sealed.is_some()
    }

    /// Have the next transaction take the sealed rows alone.
    pub(in crate::css::style) fn begin_flushing_seal(&mut self) {
        assert!(self.has_sealed_changes(), "flushing a seal that holds no change");
        self.flushing_seal = true;
    }

    /// Finish the transaction that flushed the seal: what was written after it is what the next
    /// transaction takes.
    pub(in crate::css::style) fn end_flushing_seal(&mut self) {
        assert!(self.flushing_seal, "ending a seal flush that did not begin");
        self.flushing_seal = false;
        self.sealed = None;
        self.hover.sealed = None;
    }

    /// Drop the seal, for a transaction that takes everything written, sealed or not.
    pub(in crate::css::style) fn drop_seal(&mut self) {
        if !self.flushing_seal {
            self.sealed = None;
            self.hover.sealed = None;
        }
    }

    /// Whether the rows written since the last barrier change the committed generation, as
    /// [`Self::changes`] would say a transaction taken now does.
    #[must_use]
    pub(in crate::css::style) fn has_pending_changes(&self) -> bool {
        // NB: A row found to leave the committed generation as it was does so until it is written again, which lists
        //     it again, so each write is asked about at most once, and a row found to change it is asked first next.
        use std::sync::atomic::Ordering;
        let scan = &self.pending_change_scan;
        let mut changes = Vec::new();
        let start = scan.checked.load(Ordering::Relaxed);
        let pending = scan.writes[start..]
            .iter()
            .position(|&node| self.has_pending_change_noted_in(node, &mut changes));
        scan.checked
            .store(start + pending.unwrap_or(scan.writes.len() - start), Ordering::Relaxed);
        debug_assert_eq!(
            pending.is_some(),
            self.written.nodes.iter().any(|&node| self.has_pending_change(node)),
            "a written row that changes the committed generation is among the writes not yet found to change nothing"
        );
        pending.is_some()
    }

    /// Whether the row of `node` as written changes the committed generation.
    #[must_use]
    pub(in crate::css::style) fn has_pending_change(&self, node: StyleNodeID) -> bool {
        self.has_pending_change_noted_in(node, &mut Vec::new())
    }

    /// [`Self::has_pending_change`], noting the changes it finds in `changes`, an empty list.
    fn has_pending_change_noted_in(&self, node: StyleNodeID, changes: &mut Vec<(StyleNodeID, ElementChange)>) -> bool {
        let slot = node.element_slot();
        if node.element_index().is_none() || !self.written.contains(slot) {
            return false;
        }
        let (committed, now) = (self.committed.get(slot), self.rows.get(slot));
        let hovered = (self.hover.committed.contains(node), self.hover.placed.contains(&node));
        ChangePass::ALL.into_iter().any(|pass| {
            note_changes(pass, node, (committed, now), hovered, changes);
            !changes.is_empty()
        })
    }

    /// The elements whose rows as written change the committed generation.
    pub(in crate::css::style) fn pending_change_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.written
            .nodes
            .iter()
            .copied()
            .filter(|&node| self.has_pending_change(node))
    }

    /// Whether `node` arrives in the transaction to come: it stood in no tree when the last one
    /// was committed, and stands in one as written.
    #[must_use]
    pub(in crate::css::style) fn arrival_is_pending(&self, node: StyleNodeID) -> bool {
        node.element_index().is_some()
            && self.committed_relations(node).is_none()
            && self.written_relations(node).is_some()
    }

    /// Whether `test` holds for a change the rows written since the last barrier or seal make to
    /// the committed generation, each said as [`Self::changes`] says a taken transaction's.
    pub(in crate::css::style) fn any_change_since_seal(
        &self,
        mut test: impl FnMut(StyleNodeID, ElementChange) -> bool,
    ) -> bool {
        let mut changes = Vec::new();
        self.written_since_seal.nodes.iter().any(|&node| {
            let slot = node.element_slot();
            let (committed, now) = (self.committed.get(slot), self.rows.get(slot));
            let hovered = (self.hover.committed.contains(node), self.hover.placed.contains(&node));
            changes.clear();
            for pass in ChangePass::ALL {
                note_changes(pass, node, (committed, now), hovered, &mut changes);
            }
            changes.iter().any(|&(node, change)| test(node, change))
        })
    }

    /// Release the transaction taken last: its side from before it is dropped, and the links written for it are
    /// settled.
    pub(in crate::css::style) fn release(&mut self) {
        self.before = ColumnSnapshot::default();
        self.hover.before = HoveredFacts::default();
        self.changes.clear();
        let links_written = self.links_written.take();
        self.links_written.reuse(links_written);
        let relations_written = self.relations_written.take();
        self.relations_written.reuse(relations_written);
        self.link_rows_written.store(0, Ordering::Relaxed);
    }

    /// Keep `link_rows_written`, which the document's host reads, at how many elements' links
    /// were written and not released yet, rather than a count of the projection's own. Nothing is
    /// written yet.
    pub(in crate::css::style) fn share_link_rows_written(&mut self, link_rows_written: Arc<AtomicU32>) {
        debug_assert!(!self.has_written_links());
        self.link_rows_written = link_rows_written;
    }

    /// Give a fork a count of written link rows of its own in place of the one it shares with the
    /// host, holding what it holds now.
    pub(in crate::css::style) fn detach_link_rows_written_for_fork(&mut self) {
        let written = self.link_rows_written.load(Ordering::Relaxed);
        self.link_rows_written = Arc::new(AtomicU32::new(written));
    }

    /// Whether any element's links were written since the last transaction was released.
    #[must_use]
    pub(in crate::css::style) fn has_written_links(&self) -> bool {
        !self.links_written.nodes.is_empty()
    }

    /// The elements whose links were written since the last transaction was released, each once,
    /// in the order they were first written.
    pub(in crate::css::style) fn links_written(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.links_written.nodes.iter().copied()
    }

    /// The elements whose relations were written since the last transaction was released, each
    /// once, in the order they were first written.
    pub(in crate::css::style) fn relations_written(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.relations_written.nodes.iter().copied()
    }

    /// Whether the relations of `node` were written since the last transaction was released.
    #[must_use]
    pub(in crate::css::style) fn relations_were_written(&self, node: StyleNodeID) -> bool {
        node.element_index().is_some() && self.relations_written.contains(node.element_slot())
    }

    /// What the transaction being taken changed of the elements' facts and relations: their
    /// relations first, then their facts, then their states, each in element order.
    #[must_use]
    pub(in crate::css::style) fn changes(&self) -> &[(StyleNodeID, ElementChange)] {
        &self.changes
    }

    // -- Hover ---------------------------------------------------------------------------------

    /// Place the hover on `chain`, the hovered element first: each element entering or leaving it is written, as its
    /// hover state moves.
    pub(in crate::css::style) fn place_hover(&mut self, chain: &[StyleNodeID]) {
        let placed = std::mem::replace(&mut self.hover.placed, SmallVec::from_slice(chain));
        for &node in placed.iter().filter(|node| !chain.contains(node)) {
            self.note_hover_written(node);
        }
        for &node in chain.iter().filter(|node| !placed.contains(node)) {
            self.note_hover_written(node);
        }
    }

    fn note_hover_written(&mut self, node: StyleNodeID) {
        self.note_written(node, node.element_slot());
        self.pending_change_scan.writes.push(node);
    }

    /// Whether the hover as last placed puts `node` in the hover state.
    #[cfg(test)]
    #[must_use]
    pub(in crate::css::style) fn hover_is_placed_on(&self, node: StyleNodeID) -> bool {
        self.hover.placed.contains(&node)
    }

    /// Let go of `node`, whose identity retires: an element that leaves the tree takes its hover state along.
    pub(in crate::css::style) fn retire_hovered(&mut self, node: StyleNodeID) {
        self.hover.placed.retain(|hovered| *hovered != node);
    }

    // -- Adoption ------------------------------------------------------------------------------

    /// Take what the host published at a barrier as the rows written, for the next transaction to take, answering
    /// what the host's writes counted.
    pub(in crate::css::style) fn adopt(
        &mut self,
        publication: ProjectionPublication,
    ) -> SmallVec<[(super::super::instrumentation::Counter, u64); 4]> {
        let ProjectionPublication {
            rows,
            shadows,
            slot_count,
            catalogs,
            host_fact_sets,
            text_assigned_slots,
            disabled,
            written,
            links_written,
            relations_written,
            counts,
        } = publication;
        self.rows = rows;
        self.shadows = shadows;
        self.slot_count = slot_count;
        if self.structural_flags.0.len() < slot_count {
            self.structural_flags = Arc::new(self.structural_flags.grown_to(slot_count));
        }
        self.catalogs = catalogs;
        self.host_fact_sets = host_fact_sets;
        self.text_assigned_slots = text_assigned_slots;
        self.disabled = disabled;
        for node in written {
            self.note_written(node, node.element_slot());
            self.pending_change_scan.writes.push(node);
        }
        for node in links_written {
            self.note_links_written(node);
        }
        for node in relations_written {
            self.relations_written.insert(node, node.element_slot());
        }
        counts
    }

    /// Write through the test's own writer, publishing what it wrote to the projection at once, as a host would at
    /// its next barrier.
    #[cfg(test)]
    pub(in crate::css::style) fn write_for_test<R>(
        &mut self,
        write: impl FnOnce(&mut ProjectionWriter) -> R,
    ) -> (R, SmallVec<[(super::super::instrumentation::Counter, u64); 4]>) {
        let answer = write(&mut self.test_writer);
        let publication = self.test_writer.publish();
        (answer, self.adopt(publication))
    }

    /// Note that the links of `node` are written in the transaction to come.
    fn note_links_written(&mut self, node: StyleNodeID) {
        if self.links_written.insert(node, node.element_slot()) {
            self.link_rows_written.store(
                u32::try_from(self.links_written.nodes.len()).unwrap_or(u32::MAX),
                Ordering::Relaxed,
            );
        }
    }

    // -- Reads ---------------------------------------------------------------------------------

    /// The elements whose facts as written say they are disabled.
    #[must_use]
    pub(in crate::css::style) fn written_disabled(&self) -> &HashSet<StyleNodeID> {
        &self.disabled
    }

    /// One past the greatest element slot the projection holds a row for.
    #[must_use]
    pub(in crate::css::style) fn element_slot_count(&self) -> usize {
        self.slot_count
    }

    #[must_use]
    pub(in crate::css::style) fn is_live(&self, node: StyleNodeID) -> bool {
        self.row(node).is_some_and(Row::live)
    }

    /// The element's facts as the last transaction committed them, or none where nothing was
    /// published of it by then.
    #[must_use]
    pub(in crate::css::style) fn facts(&self, node: StyleNodeID) -> Option<&ElementFacts> {
        self.facts_on(TransactionFactSide::After, node)
    }

    /// The facts the host published of `node` beside those its selectors test, as published: an element keeps them from
    /// its arrival until its identity is minted again, and a text node holds none.
    #[must_use]
    pub(in crate::css::style) fn host_facts(&self, node: StyleNodeID) -> HostFactSet {
        let id = match node.element_index() {
            Some(_) => self
                .row(node)
                .map_or_else(HostFactSetID::default, |row| row.facts.host_facts),
            None => HostFactSetID::default(),
        };
        self.host_fact_sets.get(id)
    }

    /// The element facts the style computation's adjustments read, as `bridge::element_adjustment_fact` names them.
    #[must_use]
    pub(in crate::css::style) fn adjustment_facts(&self, node: StyleNodeID) -> u32 {
        self.host_facts(node).adjustment_facts
    }

    /// The element facts a layout row built for the element records, as `bridge::element_construction_fact` names
    /// them.
    #[must_use]
    pub(in crate::css::style) fn construction_facts(&self, node: StyleNodeID) -> u32 {
        self.host_facts(node).construction_facts
    }

    /// Which principal box the element asks for.
    #[must_use]
    pub(in crate::css::style) fn box_kind(&self, node: StyleNodeID) -> super::super::bridge::ElementBoxKind {
        self.host_facts(node).box_kind()
    }

    /// The element-backed pseudo-element kind of its shadow host the element stands for, as published.
    #[must_use]
    pub(in crate::css::style) fn associated_pseudo_kind(&self, node: StyleNodeID) -> Option<u8> {
        self.host_facts(node).associated_pseudo_kind()
    }

    /// The element-backed pseudo-element kind of its shadow host the element `facts` are of stands for, on whichever
    /// generation they are read: the published sets hold every set an earlier generation names.
    #[must_use]
    pub(in crate::css::style) fn backing_pseudo_kind(&self, facts: &ElementFacts) -> Option<u8> {
        self.host_fact_sets.get(facts.host_facts).associated_pseudo_kind()
    }

    /// The element's facts as written, the ones the next transaction is to take included, or none
    /// where nothing was published of it.
    #[must_use]
    pub(in crate::css::style) fn written_facts(&self, node: StyleNodeID) -> Option<&ElementFacts> {
        self.row(node).map(|row| &row.facts).filter(|facts| facts.resident())
    }

    /// The element's facts as the transaction being taken left them, or as it found them.
    #[must_use]
    pub(in crate::css::style) fn facts_on(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<&ElementFacts> {
        let (generation, hovered) = match side {
            TransactionFactSide::Before => (&self.before, &self.hover.before),
            TransactionFactSide::After => (&self.committed, &self.hover.committed),
        };
        if let Some(facts) = hovered.get(node) {
            return Some(facts);
        }
        generation
            .get(node.element_slot())
            .map(|row| &row.facts)
            .filter(|facts| facts.resident())
    }

    /// The element's committed facts, read with the catalogs that spell them.
    #[must_use]
    pub(in crate::css::style) fn row_of(&self, node: StyleNodeID) -> Option<ProjectionRow<'_>> {
        self.row_on(TransactionFactSide::After, node)
    }

    /// The element's facts on one side of the transaction being taken, read with the catalogs that
    /// spell them.
    #[must_use]
    pub(in crate::css::style) fn row_on(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<ProjectionRow<'_>> {
        Some(ProjectionRow {
            facts: self.facts_on(side, node)?,
            projection: self,
        })
    }

    /// How the element stands in the tree as written, the writes the next transaction is to take
    /// included, or none where it stands in no tree.
    #[must_use]
    pub(in crate::css::style) fn written_relations(&self, node: StyleNodeID) -> Option<TreeRelations> {
        self.row(node).and_then(relations_of)
    }

    /// The element's first element child as written.
    #[must_use]
    pub(in crate::css::style) fn written_first_element_child(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.row(node)?.links.first_element_child
    }

    /// How the element stands in the tree as the last transaction committed it, or none where it
    /// stood in no tree then.
    #[must_use]
    pub(in crate::css::style) fn committed_relations(&self, node: StyleNodeID) -> Option<TreeRelations> {
        self.committed.get(node.element_slot()).and_then(relations_of)
    }

    /// The links of `node` as the transaction being taken left them, or as it found them.
    fn links_on(&self, side: TransactionFactSide, node: StyleNodeID) -> Option<&Links> {
        match side {
            TransactionFactSide::Before => Some(&self.before.get(node.element_slot())?.links),
            TransactionFactSide::After => self.links(node),
        }
    }

    /// Whether the element stands in a tree as the transaction being taken left it, or did when it
    /// found it.
    #[must_use]
    pub(in crate::css::style) fn is_connected_on(&self, side: TransactionFactSide, node: StyleNodeID) -> bool {
        let row = match side {
            TransactionFactSide::Before => self.before.get(node.element_slot()),
            TransactionFactSide::After => self.committed.get(node.element_slot()),
        };
        row.and_then(relations_of).is_some()
    }

    /// Whether the children of `parent` stood in a tree before the update: where it did, or for a shadow root, which
    /// holds no relations of its own, where its host did.
    #[must_use]
    pub(in crate::css::style) fn children_were_connected(&self, parent: StyleNodeID) -> bool {
        self.is_connected_on(TransactionFactSide::Before, parent)
            || self
                .host_of(parent)
                .is_some_and(|host| self.is_connected_on(TransactionFactSide::Before, host))
    }

    #[must_use]
    pub(in crate::css::style) fn parent_on(&self, side: TransactionFactSide, node: StyleNodeID) -> Option<StyleNodeID> {
        self.links_on(side, node)?.parent
    }

    #[must_use]
    pub(in crate::css::style) fn first_element_child_on(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<StyleNodeID> {
        self.links_on(side, node)?.first_element_child
    }

    #[must_use]
    pub(in crate::css::style) fn previous_element_sibling_on(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<StyleNodeID> {
        self.links_on(side, node)?.previous_element_sibling
    }

    #[must_use]
    pub(in crate::css::style) fn next_element_sibling_on(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> Option<StyleNodeID> {
        self.links_on(side, node)?.next_element_sibling
    }

    pub(in crate::css::style) fn children_on(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
    ) -> impl Iterator<Item = StyleNodeID> + '_ {
        std::iter::successors(self.first_element_child_on(side, node), move |&child| {
            self.next_element_sibling_on(side, child)
        })
    }

    /// Visit every dispatch probe the element carries: each dispatch key, an attribute's under every
    /// name a selector can reach it by, with the value of the attribute that supplied it.
    pub(in crate::css::style) fn for_each_dispatch_probe(
        &self,
        facts: &ElementFacts,
        is_document_root: bool,
        visit: impl FnMut(DispatchKey, Option<StyleAtomID>),
    ) {
        for_each_dispatch_probe(facts, &self.catalogs.name_forms, is_document_root, visit);
    }

    /// Add the atoms the rows hold to `atoms`, with the other forms of each attribute name one
    /// carries, and return how many rows were visited.
    pub(in crate::css::style) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        let mut insert = |atom: StyleAtomID| {
            if !atom.is_none() {
                atoms.insert(atom);
            }
        };
        for slot in 0..self.slot_count {
            let Some(row) = self.rows.get(slot) else {
                continue;
            };
            visited += 1;
            let facts = &row.facts;
            for atom in [
                facts.tag,
                facts.folded_tag(),
                facts.id(),
                facts.language(),
                facts.namespace(),
                facts.part_exposure(),
                facts.directionality(),
            ] {
                insert(atom);
            }
            for &atom in facts.custom_states().iter().chain(facts.parts()).chain(facts.classes()) {
                insert(atom);
            }
            if let Some(shadow) = self.shadows.get(slot).and_then(Option::as_deref) {
                for &(atom, _) in shadow.part_hosts.as_slice() {
                    insert(atom);
                }
            }
            for attribute in facts.attributes() {
                insert(attribute.name);
                insert(attribute.value);
                let forms = self.attribute_name_forms(attribute.name);
                for form in [forms.local, forms.folded_name, forms.folded_local] {
                    insert(form);
                }
            }
        }
        visited
    }

    /// Add to `atoms` the other forms of every attribute name in it, which stay live with it.
    pub(in crate::css::style) fn extend_live_attribute_name_forms(&self, atoms: &mut HashSet<StyleAtomID>) {
        for (name, forms) in self.catalogs.name_forms.iter() {
            if atoms.contains(name) {
                atoms.extend(
                    [forms.local, forms.folded_name, forms.folded_local]
                        .into_iter()
                        .filter(|atom| !atom.is_none()),
                );
            }
        }
    }

    /// The names an attribute called `name` answers to, each one standing for itself where it has
    /// no other form.
    #[must_use]
    pub(in crate::css::style) fn attribute_name_keys(&self, name: StyleAtomID) -> [StyleAtomID; 4] {
        attribute_name_keys(&self.catalogs.name_forms, name)
    }

    /// Whether the rules of trees other than the element's own can reach it: it hosts a shadow
    /// tree, is slotted, or is a part exposed outside its tree.
    #[must_use]
    pub(in crate::css::style) fn is_reached_from_other_trees(&self, node: StyleNodeID) -> bool {
        self.shadow_root_of(node).is_some()
            || self.assigned_slot_of(node).is_some()
            || !self.part_hosts_of(node).is_empty()
    }

    #[must_use]
    pub(in crate::css::style) fn attribute_name_forms(&self, name: StyleAtomID) -> AttributeNameForms {
        self.catalogs.name_forms.get(&name).copied().unwrap_or_default()
    }

    #[must_use]
    pub(in crate::css::style) fn attribute_substitution_name(&self, name: StyleAtomID) -> Option<&[u16]> {
        self.catalogs.substitution_names.get(&name).map(AsRef::as_ref)
    }

    #[must_use]
    pub(in crate::css::style) fn attribute_value_text(&self, value: StyleAtomID) -> Option<&RetainedUtf16FlyString> {
        self.catalogs.value_texts.get(&value)
    }

    /// The element's resolved language tag, empty when it has none.
    #[must_use]
    pub(in crate::css::style) fn language_tag_of(&self, node: StyleNodeID) -> &[u16] {
        self.facts(node)
            .and_then(|facts| self.catalogs.language_texts.get(&facts.language()))
            .map_or(&[], AsRef::as_ref)
    }

    /// What the selectors matched against `node` read of its structure. An element the column has no slot for yet
    /// is taken to have had all of it read.
    #[must_use]
    pub(in crate::css::style) fn structural_flags(&self, node: StyleNodeID) -> StructuralFlags {
        self.structural_flags
            .0
            .get(node.element_slot())
            .map_or(StructuralFlags::ALL, |flags| {
                StructuralFlags(flags.load(Ordering::Relaxed))
            })
    }

    /// Note that a selector read `read` of `node`, on `side` of the transaction being taken: of its place among its
    /// siblings on its parent, of whether it is empty on itself.
    pub(in crate::css::style) fn note_structural_read(
        &self,
        side: TransactionFactSide,
        node: StyleNodeID,
        read: StructuralRead,
    ) {
        let (flagged, flags) = match read {
            StructuralRead::PreviousSibling => (
                self.parent_on(side, node),
                StructuralFlags::CHILDREN_READ_SIBLINGS.union(StructuralFlags::CHILDREN_READ_PREVIOUS_SIBLING),
            ),
            StructuralRead::EarlierSiblings => (self.parent_on(side, node), StructuralFlags::CHILDREN_READ_SIBLINGS),
            StructuralRead::Position {
                from_end,
                of_type,
                beyond_edge,
            } => {
                let when = |read: bool, flag: StructuralFlags| match read {
                    true => flag,
                    false => StructuralFlags::default(),
                };
                (
                    self.parent_on(side, node),
                    StructuralFlags::CHILDREN_READ_POSITIONS
                        .union(when(from_end, StructuralFlags::CHILDREN_READ_POSITIONS_FROM_END))
                        .union(when(of_type, StructuralFlags::CHILDREN_READ_POSITIONS_OF_TYPE))
                        .union(when(beyond_edge, StructuralFlags::CHILDREN_READ_POSITIONS_BEYOND_EDGES)),
                )
            }
            StructuralRead::Emptiness => (Some(node), StructuralFlags::READ_EMPTINESS),
            StructuralRead::SiblingPositionValue => (
                self.parent_on(side, node),
                StructuralFlags::CHILD_READS_SIBLING_POSITION,
            ),
        };
        // NB: A row lies below the slot count, which the column always covers.
        let Some(cell) = flagged.and_then(|flagged| self.structural_flags.0.get(flagged.element_slot())) else {
            return;
        };
        if cell.load(Ordering::Relaxed) & flags.0 != flags.0 {
            cell.fetch_or(flags.0, Ordering::Relaxed);
        }
    }

    /// Forget what the selectors read of the structure of `node`, which arrived where all its children are matched
    /// again.
    pub(in crate::css::style) fn forget_structural_flags(&self, node: StyleNodeID) {
        if let Some(cell) = self.structural_flags.0.get(node.element_slot()) {
            cell.store(0, Ordering::Relaxed);
        }
    }

    #[must_use]
    pub(in crate::css::style) fn parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.links(node)?.parent
    }

    #[must_use]
    pub(in crate::css::style) fn first_element_child(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.links(node)?.first_element_child
    }

    #[must_use]
    pub(in crate::css::style) fn previous_element_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.links(node)?.previous_element_sibling
    }

    #[must_use]
    pub(in crate::css::style) fn next_element_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.links(node)?.next_element_sibling
    }

    #[must_use]
    pub(in crate::css::style) fn tree_scope(&self, node: StyleNodeID) -> TreeScopeID {
        self.committed
            .get(node.element_slot())
            .map_or(TreeScopeID::DOCUMENT, |row| row.facts.lists.tree_scope())
    }

    #[must_use]
    pub(in crate::css::style) fn shadow_root_of(&self, host: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow(host)?.shadow_root
    }

    #[must_use]
    pub(in crate::css::style) fn host_of(&self, shadow_root: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow(shadow_root)?.host
    }

    #[must_use]
    pub(in crate::css::style) fn assigned_slot_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(_) => self.text_assigned_slots.get(&node).copied(),
            None => self.committed.get(node.element_slot())?.facts.lists.assigned_slot(),
        }
    }

    #[must_use]
    pub(in crate::css::style) fn assigned_nodes_of(&self, slot: StyleNodeID) -> &[StyleNodeID] {
        self.shadow(slot).map_or(&[], |shadow| shadow.assigned_nodes.as_slice())
    }

    #[must_use]
    pub(in crate::css::style) fn part_hosts_of(&self, node: StyleNodeID) -> &[(StyleAtomID, StyleNodeID)] {
        self.shadow(node).map_or(&[], |shadow| shadow.part_hosts.as_slice())
    }

    /// The shadow host of the tree `node` is in, if it is in one: the host of the root of its
    /// parent chain.
    #[must_use]
    pub(in crate::css::style) fn shadow_host_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        let mut root = node;
        while let Some(parent) = self.parent(root) {
            root = parent;
        }
        self.host_of(root)
    }

    /// The shadow host of the tree `node` stands in as written, the writes the next transaction is
    /// to take included.
    #[must_use]
    pub(in crate::css::style) fn written_shadow_host_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        let mut root = node;
        while let Some(parent) = self.row(root).and_then(|row| row.links.parent) {
            root = parent;
        }
        self.host_of(root)
    }

    /// The element whose computed values this element inherits: its slot, the host of the shadow
    /// root it is a child of, or its parent.
    #[must_use]
    pub(in crate::css::style) fn inheritance_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if let Some(slot) = self.assigned_slot_of(node) {
            return Some(slot);
        }
        let parent = self.parent(node)?;
        Some(self.host_of(parent).unwrap_or(parent))
    }

    /// The parent an element has in the flat tree: its slot, the host of the shadow root it is a
    /// child of, or its parent, unless that parent hosts a shadow tree or is a slot with assigned
    /// nodes, which leave the element out of the flat tree.
    #[must_use]
    pub(in crate::css::style) fn flat_tree_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if let Some(slot) = self.assigned_slot_of(node) {
            return Some(slot);
        }
        let parent = self.parent(node)?;
        if let Some(host) = self.host_of(parent) {
            return Some(host);
        }
        if self.shadow_root_of(parent).is_some() || !self.assigned_nodes_of(parent).is_empty() {
            return None;
        }
        Some(parent)
    }

    /// The children an element has in the flat tree: those of its shadow root where it hosts one,
    /// a slot's assigned nodes, text nodes included, where it has any, and its own otherwise.
    pub(in crate::css::style) fn flat_tree_children(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = StyleNodeID> + '_ {
        let assigned = match self.shadow_root_of(node) {
            Some(_) => &[][..],
            None => self.assigned_nodes_of(node),
        };
        let dom_parent = match assigned.is_empty() {
            true => Some(self.shadow_root_of(node).unwrap_or(node)),
            false => None,
        };
        dom_parent
            .into_iter()
            .flat_map(|parent| self.children(parent))
            .chain(assigned.iter().copied())
    }

    pub(in crate::css::style) fn children(&self, node: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        std::iter::successors(self.first_element_child(node), |&child| {
            self.next_element_sibling(child)
        })
    }

    pub(in crate::css::style) fn ancestors(&self, node: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        std::iter::successors(self.parent(node), |&ancestor| self.parent(ancestor))
    }

    /// `root` and its descendants, in tree order.
    pub(in crate::css::style) fn preorder(&self, root: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        let tree = ProjectionTree {
            projection: self,
            side: TransactionFactSide::After,
        };
        std::iter::successors(Some(root), move |&node| tree.next_in_subtree(node, root))
    }

    /// The elements whose [`Self::inheritance_parent`] is `node`: a shadow host's shadow root
    /// children, a slot's assigned elements, and the children assigned to no slot.
    pub(in crate::css::style) fn style_children(&self, node: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        let shadow_root_children = self
            .shadow_root_of(node)
            .into_iter()
            .flat_map(|shadow_root| self.children(shadow_root));
        let assigned_elements = self.assigned_nodes_of(node).iter().copied().filter(move |&assigned| {
            assigned.element_index().is_some() && self.assigned_slot_of(assigned) == Some(node)
        });
        let own_children = self
            .children(node)
            .filter(move |&child| self.inheritance_parent(child) == Some(node));
        shadow_root_children.chain(assigned_elements).chain(own_children)
    }

    /// `root` and the elements a style traversal reaches from it through [`Self::style_children`],
    /// each after its inheritance parent.
    pub(in crate::css::style) fn style_preorder(&self, root: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        let mut stack = vec![root];
        std::iter::from_fn(move || {
            let node = stack.pop()?;
            let first = stack.len();
            stack.extend(self.style_children(node));
            stack[first..].reverse();
            Some(node)
        })
    }

    /// The projection as a selector is matched against it on one side of the
    /// transaction being taken.
    #[must_use]
    pub(in crate::css::style) fn subject_on(&self, side: TransactionFactSide) -> ProjectionSubject<'_> {
        ProjectionSubject {
            projection: self,
            side,
            part_exposure_scope: None,
            match_workspace: None,
            owned_match_workspace: None,
            positional_index_policy: PositionalIndexPolicy::All,
            last_structural_read: std::cell::Cell::new(None),
        }
    }

    /// An evaluator of selectors against the projection.
    #[must_use]
    pub(in crate::css::style) fn evaluator(&self) -> ProjectionEvaluator<'_> {
        self.evaluator_on(TransactionFactSide::After)
    }

    /// An evaluator of selectors against the projection on one side of the transaction
    /// being taken.
    #[must_use]
    pub(in crate::css::style) fn evaluator_on(&self, side: TransactionFactSide) -> ProjectionEvaluator<'_> {
        SelectorEvaluator {
            subject: self.subject_on(side),
            bindings: SelectorBindings::default(),
        }
    }
}

/// The relations of the element a row holds, or none where it stands in no tree.
/// The writes a test makes of a projection directly, each published to it at once through the test's own writer, as
/// a host's are at its next barrier.
#[cfg(test)]
macro_rules! writes_for_test {
    ($($name:ident($($argument:ident: $type:ty),*);)+) => {
        impl Projection {
            $(
                pub(in crate::css::style) fn $name(&mut self, $($argument: $type),*) {
                    self.write_for_test(|writer| writer.$name($($argument),*));
                }
            )+
        }
    };
}

#[cfg(test)]
writes_for_test! {
    mint(node: StyleNodeID);
    retire(node: StyleNodeID);
    forget(node: StyleNodeID);
    set_tag(node: StyleNodeID, tag: StyleAtomID);
    set_folded_tag(node: StyleNodeID, folded: StyleAtomID);
    set_language(node: StyleNodeID, language: StyleAtomID);
    set_directionality(node: StyleNodeID, directionality: StyleAtomID);
    set_part_exposure(node: StyleNodeID, exposure: StyleAtomID);
    set_custom_states(node: StyleNodeID, states: &[StyleAtomID]);
    set_language_text(language: StyleAtomID, text: &[u16]);
    ensure_facts(node: StyleNodeID);
    write_relations(node: StyleNodeID, relations: Option<TreeRelations>);
    set_namespace(node: StyleNodeID, namespace: StyleAtomID);
    set_id(node: StyleNodeID, id: StyleAtomID);
    set_class(node: StyleNodeID, class: StyleAtomID, present: bool);
    set_attribute(node: StyleNodeID, name: StyleAtomID, value: StyleAtomID, present: bool);
    set_parts(node: StyleNodeID, parts: &[StyleAtomID]);
    set_attribute_value_text(value: StyleAtomID, text: RetainedUtf16FlyString);
    note_attribute_name_forms(name: StyleAtomID, forms: AttributeNameForms);
    forget_atoms(atoms: &[StyleAtomID]);
    set_parent(node: StyleNodeID, parent: Option<StyleNodeID>);
    set_first_element_child(node: StyleNodeID, child: Option<StyleNodeID>);
    set_previous_element_sibling(node: StyleNodeID, sibling: Option<StyleNodeID>);
    set_next_element_sibling(node: StyleNodeID, sibling: Option<StyleNodeID>);
    set_connected(node: StyleNodeID, connected: bool);
    set_assigned_slot(node: StyleNodeID, slot: Option<StyleNodeID>);
    set_shadow_root(host: StyleNodeID, shadow_root: StyleNodeID);
    set_assigned_nodes(slot: StyleNodeID, nodes: &[StyleNodeID]);
    retire_texts(nodes: [StyleNodeID; 1]);
}

fn relations_of(row: &Row) -> Option<TreeRelations> {
    (row.live() && row.connected()).then_some(TreeRelations {
        parent: row.links.parent,
        previous_element_sibling: row.links.previous_element_sibling,
        next_element_sibling: row.links.next_element_sibling,
        tree_scope: row.facts.lists.tree_scope(),
        assigned_slot: row.facts.lists.assigned_slot(),
    })
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ChangePass {
    Relations,
    Facts,
    States,
}

impl ChangePass {
    const ALL: [Self; 3] = [Self::Relations, Self::Facts, Self::States];
}

/// Visit what is in one of `a` and `b`, sorted lists, and not in the other.
fn for_each_difference<T: Ord + Copy>(a: &[T], b: &[T], mut visit: impl FnMut(T)) {
    let (mut a, mut b) = (a.iter().peekable(), b.iter().peekable());
    loop {
        match (a.peek(), b.peek()) {
            (None, None) => return,
            (Some(&&x), None) => {
                visit(x);
                a.next();
            }
            (None, Some(&&y)) => {
                visit(y);
                b.next();
            }
            (Some(&&x), Some(&&y)) => match x.cmp(&y) {
                std::cmp::Ordering::Less => {
                    visit(x);
                    a.next();
                }
                std::cmp::Ordering::Greater => {
                    visit(y);
                    b.next();
                }
                std::cmp::Ordering::Equal => {
                    a.next();
                    b.next();
                }
            },
        }
    }
}

/// Note what one pass over the elements of a transaction finds changed of `node` between its row
/// before the transaction and its row now.
fn note_changes(
    pass: ChangePass,
    node: StyleNodeID,
    (before, after): (Option<&Row>, Option<&Row>),
    (was_hovered, is_hovered): (bool, bool),
    changes: &mut Vec<(StyleNodeID, ElementChange)>,
) {
    let (old, new) = (before.and_then(relations_of), after.and_then(relations_of));
    let empty = ElementFacts::default();
    let (was, is) = (
        before.map_or(&empty, |row| &row.facts),
        after.map_or(&empty, |row| &row.facts),
    );
    let mut note = |change| changes.push((node, change));
    match pass {
        ChangePass::Relations => {
            if old != new {
                note(ElementChange::Relations(old, new));
            }
        }
        // NB: An element that leaves says every fact it had by leaving, and the host forgets them as it publishes that.
        ChangePass::Facts | ChangePass::States if old.is_some() && new.is_none() => {}
        // NB: Every fact of an element arriving is said by its arrival.
        ChangePass::Facts if old.is_none() && new.is_some() => {
            if !was.says_the_same_as(is) {
                note(ElementChange::ArrivingFacts);
            }
        }
        ChangePass::Facts => {
            if was.tag != is.tag {
                note(ElementChange::Feature(LocalFeatureKey::TagName));
            }
            if was.folded_tag() != is.folded_tag() {
                note(ElementChange::Feature(LocalFeatureKey::FoldedTagName));
            }
            if was.id() != is.id() {
                note(ElementChange::Id(was.id(), is.id()));
            }
            for_each_difference(was.classes(), is.classes(), |class| {
                note(ElementChange::Feature(LocalFeatureKey::Class(class)));
            });
            let mut parts: SmallVec<[StyleAtomID; 4]> = SmallVec::from_slice(was.parts());
            parts.sort_unstable();
            let mut now: SmallVec<[StyleAtomID; 4]> = SmallVec::from_slice(is.parts());
            now.sort_unstable();
            for_each_difference(&parts, &now, |part| {
                note(ElementChange::Feature(LocalFeatureKey::Part(part)));
            });
            parts.clear();
            parts.extend_from_slice(was.custom_states());
            parts.sort_unstable();
            now.clear();
            now.extend_from_slice(is.custom_states());
            now.sort_unstable();
            for_each_difference(&parts, &now, |state| {
                note(ElementChange::Feature(LocalFeatureKey::CustomState(state)));
            });
            if was.has_text_content() != is.has_text_content() {
                note(ElementChange::Feature(LocalFeatureKey::Emptiness));
            }
            // An attribute whose value moved is one change, as one that came or went is.
            let names = |attributes: &[AttributeFact]| -> SmallVec<[StyleAtomID; 8]> {
                attributes.iter().map(|attribute| attribute.name).collect()
            };
            let (old_names, new_names) = (names(was.attributes()), names(is.attributes()));
            for_each_difference(&old_names, &new_names, |name| {
                note(ElementChange::Feature(LocalFeatureKey::Attribute(name)));
            });
            for attribute in was.attributes() {
                if let Ok(index) = new_names.binary_search(&attribute.name)
                    && is.attributes()[index].value != attribute.value
                {
                    note(ElementChange::Feature(LocalFeatureKey::Attribute(attribute.name)));
                }
            }
            if was.language() != is.language() {
                note(ElementChange::Feature(LocalFeatureKey::Language));
            }
            if was.directionality() != is.directionality() {
                note(ElementChange::Feature(LocalFeatureKey::Directionality));
            }
            if was.heading_level() != is.heading_level() {
                note(ElementChange::Feature(LocalFeatureKey::HeadingLevel));
            }
            if was.part_exposure() != is.part_exposure() {
                note(ElementChange::Feature(LocalFeatureKey::PartExposure));
            }
        }
        ChangePass::States => {
            if old.is_none() && new.is_some() {
                return;
            }
            let hovered = |hovered: bool| match hovered {
                true => 1_u64 << StateFact::Hover as u32,
                false => 0,
            };
            // NB: An element that leaves the tree leaves the hover chain with it, which is no change of its state.
            let hover_moved = new.is_some() && was_hovered != is_hovered;
            let (was_states, is_states) = (
                was.states.0 | hovered(was_hovered && hover_moved),
                is.states.0 | hovered(is_hovered && hover_moved),
            );
            for fact in StateSet(was_states ^ is_states).facts() {
                note(ElementChange::State(fact));
            }
        }
    }
}

/// The names an attribute called `name` answers to: itself, its local name, and the folding of
/// each, the foldings standing for the names they fold where they are the same.
fn attribute_name_keys(name_forms: &Catalog<AttributeNameForms>, name: StyleAtomID) -> [StyleAtomID; 4] {
    let forms = name_forms.get(&name).copied().unwrap_or_default();
    let folded_name = match forms.folded_name.is_none() {
        true => name,
        false => forms.folded_name,
    };
    let folded_local = match forms.folded_local.is_none() {
        true => forms.local,
        false => forms.folded_local,
    };
    [name, forms.local, folded_name, folded_local]
}

/// The Bloom summary of the dispatch keys `facts` carry, document-root status aside.
/// The Bloom summary of the dispatch keys the lists `lists` would carry.
fn list_bloom_of(lists: &FactListsEdit, name_forms: &Catalog<AttributeNameForms>) -> u64 {
    let mut bloom = 0;
    if !lists.folded_tag.is_none() {
        bloom |= dispatch_bloom_bit(DispatchKey::TagName(lists.folded_tag));
    }
    if !lists.id.is_none() {
        bloom |= dispatch_bloom_bit(DispatchKey::Id(lists.id));
    }
    for &class in lists.classes.iter() {
        bloom |= dispatch_bloom_bit(DispatchKey::Class(class));
    }
    if !lists.directionality.is_none() {
        bloom |= dispatch_bloom_bit(DispatchKey::Directionality(lists.directionality));
    }
    for &state in lists.custom_states.iter() {
        bloom |= dispatch_bloom_bit(DispatchKey::CustomState(state));
    }
    for &part in lists.parts.iter() {
        bloom |= dispatch_bloom_bit(DispatchKey::Part(part));
    }
    for attribute in lists.attributes.iter() {
        for name in attribute_name_keys(name_forms, attribute.name) {
            if !name.is_none() {
                bloom |= dispatch_bloom_bit(DispatchKey::AttributeName(name));
            }
        }
    }
    bloom
}

/// See [`Projection::for_each_dispatch_probe`].
fn for_each_dispatch_probe(
    facts: &ElementFacts,
    name_forms: &Catalog<AttributeNameForms>,
    is_document_root: bool,
    mut visit: impl FnMut(DispatchKey, Option<StyleAtomID>),
) {
    visit(DispatchKey::Universal, None);
    for fact in facts.states.facts() {
        visit(DispatchKey::State(fact), None);
    }
    if is_document_root {
        visit(DispatchKey::Root, None);
    }
    if facts.heading_level() != 0 {
        visit(DispatchKey::Heading, None);
    }
    visit(DispatchKey::TagName(facts.tag), None);
    let folded_tag = facts.folded_tag();
    if !folded_tag.is_none() {
        visit(DispatchKey::TagName(folded_tag), None);
    }
    if !facts.id().is_none() {
        visit(DispatchKey::Id(facts.id()), None);
    }
    for &class in facts.classes() {
        visit(DispatchKey::Class(class), None);
    }
    if !facts.directionality().is_none() {
        visit(DispatchKey::Directionality(facts.directionality()), None);
    }
    for &state in facts.custom_states() {
        visit(DispatchKey::CustomState(state), None);
    }
    for &part in facts.parts() {
        visit(DispatchKey::Part(part), None);
    }
    for attribute in facts.attributes() {
        // Every name a selector can reach this attribute by, since a compound dispatches under one
        // of them and which one depends on how the selector wrote it.
        let keys = attribute_name_keys(name_forms, attribute.name);
        for (index, &name) in keys.iter().enumerate() {
            if !name.is_none() && !keys[..index].contains(&name) {
                visit(DispatchKey::AttributeName(name), Some(attribute.value));
            }
        }
    }
}

impl<'a> ProjectionRow<'a> {
    #[must_use]
    pub(in crate::css::style) fn tag(self) -> StyleAtomID {
        self.facts.tag
    }

    #[must_use]
    pub(in crate::css::style) fn folded_tag(self) -> StyleAtomID {
        self.facts.folded_tag()
    }

    /// The element-backed pseudo-element kind of its shadow host the element stands for.
    #[must_use]
    pub(in crate::css::style) fn backing_pseudo_kind(self) -> Option<u8> {
        self.projection.backing_pseudo_kind(self.facts)
    }

    /// The keys of the element, which a compound can require of it.
    pub(in crate::css::style) fn carried_keys(self) -> impl Iterator<Item = CarriedKey> + 'a {
        self.facts
            .named_keys()
            .chain(backed_pseudo_element_key(self.facts, &self.projection.host_fact_sets))
    }

    #[must_use]
    pub(in crate::css::style) fn id(self) -> StyleAtomID {
        self.facts.id()
    }

    #[must_use]
    pub(in crate::css::style) fn states(self) -> StateSet {
        self.facts.states
    }

    #[must_use]
    pub(in crate::css::style) fn classes(self) -> &'a [StyleAtomID] {
        self.facts.classes()
    }

    #[must_use]
    pub(in crate::css::style) fn attributes(self) -> &'a [AttributeFact] {
        self.facts.attributes()
    }

    #[must_use]
    pub(in crate::css::style) fn custom_states(self) -> &'a [StyleAtomID] {
        self.facts.custom_states()
    }

    #[must_use]
    pub(in crate::css::style) fn parts(self) -> &'a [StyleAtomID] {
        self.facts.parts()
    }

    /// See [`Projection::attribute_name_keys`].
    #[must_use]
    pub(in crate::css::style) fn attribute_name_keys(self, name: StyleAtomID) -> [StyleAtomID; 4] {
        self.projection.attribute_name_keys(name)
    }
}

/// Matches selectors against the projection.
pub(in crate::css::style) type ProjectionEvaluator<'a> = SelectorEvaluator<ProjectionSubject<'a>>;

impl<'a> ProjectionEvaluator<'a> {
    /// Evaluate for a part exposed in `scope`, for an evaluator reused across the rules of several trees, or for no part
    /// for none.
    pub(in crate::css::style) fn set_part_exposure_scope(&mut self, scope: Option<TreeScopeID>) {
        self.subject.part_exposure_scope = scope;
    }

    /// Keep sibling positions, and the answers of positional tests, in `workspace`.
    #[must_use]
    pub(in crate::css::style) fn with_match_workspace(mut self, workspace: &'a mut MatchScratch) -> Self {
        self.subject.match_workspace = Some(workspace);
        self
    }

    /// Keep sibling positions, and the answers of positional tests, in a workspace of the
    /// evaluator's own for `side`, for as long as it lives. The projection must not change
    /// meanwhile, as the positions are not taken again.
    #[must_use]
    pub(in crate::css::style) fn with_owned_match_workspace(mut self) -> Self {
        self.subject.owned_match_workspace = Some(MatchScratch::default());
        self
    }

    /// How many walks of a `:has()` argument the workspace the evaluator owns remembers the answers of.
    pub(in crate::css::style) fn relative_walks(&self) -> u64 {
        self.subject
            .owned_match_workspace
            .as_ref()
            .map_or(0, MatchScratch::relative_walks)
    }

    /// Answer only stepped positional tests from the workspace's sibling index, and memoize no
    /// positional answer. See [`PositionalIndexPolicy::SteppedOnly`].
    #[must_use]
    pub(in crate::css::style) fn indexing_stepped_positions_only(mut self) -> Self {
        self.subject.positional_index_policy = PositionalIndexPolicy::SteppedOnly;
        self
    }
}

/// The projection's links, as a selector walks them.
///
/// On the side of a transaction from before it, an element's parent and siblings are the ones it
/// had, but its shadow relations, its slot and the parent that scoping and `:scope` walk are the
/// ones it has: an element whose tree or slot moved restyles whole, so what they reach is the same
/// before as now.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct ProjectionTree<'a> {
    projection: &'a Projection,
    side: TransactionFactSide,
}

impl SelectorTree for ProjectionTree<'_> {
    type Node = StyleNodeID;

    #[inline]
    fn backs_pseudo_element(self, node: StyleNodeID, kind: u16) -> bool {
        self.projection
            .facts_on(self.side, node)
            .is_some_and(|facts| self.projection.backing_pseudo_kind(facts).map(u16::from) == Some(kind))
    }

    #[inline]
    fn parent(self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.parent_on(self.side, node)
    }

    #[inline]
    fn previous_sibling(self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.previous_element_sibling_on(self.side, node)
    }

    #[inline]
    fn next_sibling(self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.next_element_sibling_on(self.side, node)
    }

    #[inline]
    fn first_child(self, parent: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.first_element_child_on(self.side, parent)
    }

    #[inline]
    fn live_parent(self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.parent(node)
    }

    #[inline]
    fn shadow_root_of(self, host: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.shadow_root_of(host)
    }

    #[inline]
    fn host_of(self, shadow_root: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.host_of(shadow_root)
    }

    #[inline]
    fn assigned_slot_of(self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.assigned_slot_of(node)
    }

    #[inline]
    fn shadow_host_of(self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.projection.shadow_host_of(node)
    }
}

/// One element's facts, as the tests of a selector read them.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct ProjectionRow<'a> {
    pub(in crate::css::style) facts: &'a ElementFacts,
    projection: &'a Projection,
}

impl ElementFeatures for ProjectionRow<'_> {
    type Attribute = AttributeFact;

    #[inline]
    fn local_name_is(&self, name: StyleAtomID) -> bool {
        self.facts.tag == name
    }

    #[inline]
    fn namespace_is(&self, namespace: StyleAtomID) -> bool {
        self.facts.namespace() == namespace
    }

    #[inline]
    fn has_id(&self, id: StyleAtomID) -> bool {
        self.facts.id() == id
    }

    #[inline]
    fn has_class(&self, class: StyleAtomID) -> bool {
        self.facts.classes().binary_search(&class).is_ok()
    }

    /// `[*|x]` names the attribute called `x` in each namespace the element carries it in, and they
    /// all publish the same any-namespace atom as their local name.
    #[inline]
    fn attributes_named(&self, name: StyleAtomID, any_namespace: bool) -> impl Iterator<Item = AttributeFact> + '_ {
        self.facts
            .attributes()
            .iter()
            .filter(move |attribute| match any_namespace {
                false => attribute.name == name,
                true => self.projection.attribute_name_forms(attribute.name).local == name,
            })
            .copied()
    }
}

/// The projection's elements, as a selector is matched against them: their committed
/// facts, their links, and the workspace the evaluation keeps sibling positions in, if any.
pub(in crate::css::style) struct ProjectionSubject<'a> {
    projection: &'a Projection,
    /// Which side of the transaction being taken the elements' facts and relations are read on.
    side: TransactionFactSide,
    /// The outer tree scope asking about a part exposed from a shadow tree.
    part_exposure_scope: Option<TreeScopeID>,
    match_workspace: Option<&'a mut MatchScratch>,
    /// The workspace the evaluation keeps sibling positions in where none is lent to it.
    owned_match_workspace: Option<MatchScratch>,
    positional_index_policy: PositionalIndexPolicy,
    /// The structural read the evaluation noted last, which the candidates of one element note again
    /// one after another.
    last_structural_read: std::cell::Cell<Option<(StyleNodeID, StructuralRead)>>,
}

impl ProjectionSubject<'_> {
    fn workspace_mut(&mut self) -> Option<&mut MatchScratch> {
        match &mut self.match_workspace {
            Some(workspace) => Some(&mut **workspace),
            None => self.owned_match_workspace.as_mut(),
        }
    }

    /// The distinct hosts the element's part names reach, or the host of the tree it stands in,
    /// which is the only level it has where it exports no part.
    fn part_exposure_hosts_of(&self, node: StyleNodeID) -> SmallVec<[StyleNodeID; 1]> {
        let pairs = self.projection.part_hosts_of(node);
        if pairs.is_empty() {
            return self.projection.shadow_host_of(node).into_iter().collect();
        }
        let mut hosts = SmallVec::new();
        for &(_, host) in pairs {
            if !hosts.contains(&host) {
                hosts.push(host);
            }
        }
        hosts
    }
}

impl<'a> SelectorSubject for ProjectionSubject<'a> {
    type Node = StyleNodeID;
    type Tree = ProjectionTree<'a>;
    type Row = ProjectionRow<'a>;
    type Attribute = AttributeFact;
    type Features<'s>
        = ProjectionRow<'a>
    where
        Self: 's;
    type Incomplete = Incomplete;

    #[inline]
    fn tree(&self) -> ProjectionTree<'a> {
        ProjectionTree {
            projection: self.projection,
            side: self.side,
        }
    }

    #[inline]
    fn row(&mut self, node: StyleNodeID) -> Result<ProjectionRow<'a>, Incomplete> {
        let facts = self
            .projection
            .facts_on(self.side, node)
            .ok_or(Incomplete::MissingFacts(node))?;
        Ok(ProjectionRow {
            facts,
            projection: self.projection,
        })
    }

    #[inline]
    fn features(&self, row: ProjectionRow<'a>) -> ProjectionRow<'a> {
        row
    }

    #[inline]
    fn same_type(&self, row: ProjectionRow<'a>, other: ProjectionRow<'a>) -> bool {
        row.facts.tag == other.facts.tag && row.facts.namespace() == other.facts.namespace()
    }

    #[inline]
    fn attribute_value_atom(&self, attribute: AttributeFact) -> StyleAtomID {
        attribute.value
    }

    #[inline]
    fn attribute_value_text(&self, attribute: AttributeFact) -> Option<TokenizerInput<'_>> {
        self.projection
            .attribute_value_text(attribute.value)
            .map(TokenizerInput::from)
    }

    #[inline]
    fn has_state(&self, row: ProjectionRow<'a>, fact: StateFact) -> bool {
        row.facts.states.contains(fact)
    }

    #[inline]
    fn language_tag(&self, row: ProjectionRow<'a>) -> TokenizerInput<'_> {
        let tag = self
            .projection
            .catalogs
            .language_texts
            .get(&row.facts.language())
            .map_or(&[][..], AsRef::as_ref);
        TokenizerInput::Utf16(tag)
    }

    #[inline]
    fn directionality_is(&self, row: ProjectionRow<'a>, direction: StyleAtomID) -> bool {
        row.facts.directionality() == direction
    }

    #[inline]
    fn has_custom_state(&self, row: ProjectionRow<'a>, state: StyleAtomID) -> bool {
        row.facts.custom_states().contains(&state)
    }

    #[inline]
    fn heading_level(&self, row: ProjectionRow<'a>) -> u8 {
        row.facts.heading_level()
    }

    /// Element children are rows and the links answer for them. A text or comment child is not,
    /// so the element publishes whether it holds one.
    #[inline]
    fn is_empty(&mut self, node: StyleNodeID) -> Result<bool, Incomplete> {
        let row = self.row(node)?;
        Ok(self.projection.first_element_child_on(self.side, node).is_none() && !row.facts.has_text_content())
    }

    #[inline]
    fn is_root(&self, node: StyleNodeID) -> bool {
        self.projection.parent_on(self.side, node).is_none()
    }

    #[inline]
    fn is_node(&self, node: StyleNodeID, named: StyleNodeID) -> bool {
        node == named
    }

    #[inline]
    fn has_part(&self, row: ProjectionRow<'a>, part: StyleAtomID) -> bool {
        row.facts.parts().contains(&part)
    }

    fn part_exposure_hosts(&self, node: StyleNodeID) -> SmallVec<[StyleNodeID; 1]> {
        self.part_exposure_hosts_of(node)
    }

    fn part_hosts_in_exposure_scope(&self, node: StyleNodeID) -> Option<SmallVec<[StyleNodeID; 1]>> {
        let scope = self.part_exposure_scope?;
        let mut hosts = self.part_exposure_hosts_of(node);
        hosts.retain(|host| self.projection.tree_scope(*host) == scope);
        Some(hosts)
    }

    fn exposes_part_to(&mut self, node: StyleNodeID, part: StyleAtomID, host: StyleNodeID) -> Result<bool, Incomplete> {
        let row = self.row(node)?;
        let pairs = self.projection.part_hosts_of(node);
        Ok(match pairs.is_empty() {
            // With no pairing recorded the element is addressable only under the names it carries,
            // and all of them reach the host of the tree it stands in.
            true => row.facts.parts().contains(&part),
            false => pairs
                .iter()
                .any(|&(exposed, exposed_to)| exposed == part && exposed_to == host),
        })
    }

    #[inline]
    fn note_structural_read(&self, node: StyleNodeID, read: StructuralRead) {
        if self.last_structural_read.get() == Some((node, read)) {
            return;
        }
        self.last_structural_read.set(Some((node, read)));
        self.projection.note_structural_read(self.side, node, read);
    }

    const FILTERS_BY_REQUIRED_DISPATCH_BLOOM: bool = true;

    /// A row facts cannot answer for yet is never prejudged.
    #[inline]
    fn may_match_required(&self, required: u64, node: StyleNodeID) -> bool {
        self.projection
            .facts_on(self.side, node)
            .is_none_or(|facts| facts.dispatch_bloom(false) & required == required)
    }

    /// NB: Only the workspace the evaluator owns keeps relative answers: an evaluator owning one
    ///     lives while the projection holds still, which is what an answer reads, as the sibling
    ///     positions it keeps do. A lent workspace may outlive a change of facts.
    fn relative_answer_of(
        &mut self,
        list: super::super::selector_evaluation::SelectorListKey,
        anchor: StyleNodeID,
        bindings: &SelectorBindings<StyleNodeID>,
    ) -> Option<bool> {
        let workspace = self.owned_match_workspace.as_ref()?;
        workspace.relative_answer(&RelativeAnswerKey::of_list(list, anchor, bindings))
    }

    fn record_relative_answer_of(
        &mut self,
        list: super::super::selector_evaluation::SelectorListKey,
        anchor: StyleNodeID,
        bindings: &SelectorBindings<StyleNodeID>,
        answer: bool,
    ) {
        if let Some(workspace) = self.owned_match_workspace.as_mut() {
            workspace.insert_relative_answer(RelativeAnswerKey::of_list(list, anchor, bindings), answer);
        }
    }

    const REMEMBERS_SIBLING_ANSWERS: bool = true;

    type WalkAnswers = SiblingAnswerTable;

    /// NB: Only the workspace the evaluator owns keeps sibling answers, as it keeps relative ones.
    fn walk_answers(
        &mut self,
        selector: super::super::selector_evaluation::SelectorKey,
        bindings: &SelectorBindings<StyleNodeID>,
    ) -> Option<SiblingAnswerTable> {
        let key = SiblingAnswerKey {
            selector,
            scope_shadow_root: bindings.scope_shadow_root,
            scope_root_instance: bindings.scope_root_instance,
            part_exposure_scope: self.part_exposure_scope,
        };
        let workspace = self.owned_match_workspace.as_mut()?;
        Some(workspace.sibling_table(key))
    }

    fn walk_answer(&self, table: SiblingAnswerTable, node: StyleNodeID) -> Option<bool> {
        let workspace = self.owned_match_workspace.as_ref()?;
        workspace.sibling_answer(table, node)
    }

    fn record_walk_answers(&mut self, table: SiblingAnswerTable, nodes: &[StyleNodeID], answer: bool) {
        if let Some(workspace) = self.owned_match_workspace.as_mut() {
            for &node in nodes {
                workspace.insert_sibling_answer(table, node, answer);
            }
        }
    }

    #[inline]
    fn sibling_index(&mut self, position: NthPosition, node: StyleNodeID) -> Result<Option<i64>, Incomplete> {
        if position.step == 0 && self.positional_index_policy != PositionalIndexPolicy::All {
            return Ok(None);
        }
        let (projection, fact_side) = (self.projection, self.side);
        let Some(workspace) = self.workspace_mut() else {
            return Ok(None);
        };
        workspace.indexed_sibling_position(
            position,
            node,
            SiblingSequenceSource {
                parent: projection.parent_on(fact_side, node),
                children: |parent| projection.children_on(fact_side, parent).collect(),
                type_of: |sibling| {
                    let facts = projection
                        .facts_on(fact_side, sibling)
                        .ok_or(Incomplete::MissingFacts(sibling))?;
                    Ok((facts.tag, facts.namespace()))
                },
            },
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DIV: StyleAtomID = StyleAtomID(1);
    const THEME: StyleAtomID = StyleAtomID(10);
    const ITEM: StyleAtomID = StyleAtomID(11);
    const HREF: StyleAtomID = StyleAtomID(20);
    const HREF_VALUE: StyleAtomID = StyleAtomID(21);

    fn element(index: u32) -> StyleNodeID {
        StyleNodeID::element(index)
    }

    /// `root > [first.item, second.item[href], third]`, root carrying `.theme`.
    fn document() -> Projection {
        let mut projection = Projection::default();
        for index in 1..=4 {
            projection.mint(element(index));
            projection.set_connected(element(index), true);
            projection.set_tag(element(index), DIV);
        }
        projection.set_class(element(1), THEME, true);
        projection.set_first_element_child(element(1), Some(element(2)));
        for index in 2..=4 {
            projection.set_parent(element(index), Some(element(1)));
            projection.set_previous_element_sibling(element(index), (index > 2).then(|| element(index - 1)));
            projection.set_next_element_sibling(element(index), (index < 4).then(|| element(index + 1)));
        }
        projection.set_class(element(2), ITEM, true);
        projection.set_class(element(3), ITEM, true);
        projection.set_attribute(element(3), HREF, HREF_VALUE, true);
        projection.commit();
        projection
    }

    #[test]
    fn a_catalog_copies_only_what_was_added_since_its_base_was_folded() {
        let mut catalog: Catalog<u32> = Catalog::default();
        let mut published = Vec::new();
        for index in 1..=10_000_u32 {
            catalog.insert(StyleAtomID(index), index);
            published.push(catalog.clone());
            if published.len() > 2 {
                published.remove(0);
            }
        }
        assert_eq!(catalog.get(&StyleAtomID(1)), Some(&1));
        assert_eq!(catalog.get(&StyleAtomID(10_000)), Some(&10_000));
        assert_eq!(catalog.iter().count(), 10_000);
        // NB: Copying the whole catalog for each addition would copy about 50 million entries.
        assert!(
            catalog.copied < 10_000 * 10_000 / 8,
            "additions copied {} entries",
            catalog.copied
        );
        catalog.forget(&[StyleAtomID(1), StyleAtomID(10_000)]);
        assert!(!catalog.contains_key(&StyleAtomID(1)) && !catalog.contains_key(&StyleAtomID(10_000)));
    }

    #[test]
    fn live_atoms_are_those_the_rows_hold_with_their_attribute_name_forms() {
        let mut projection = Projection::default();
        let node = element(1);
        projection.mint(node);
        projection.set_tag(node, StyleAtomID(1));
        projection.set_folded_tag(node, StyleAtomID(2));
        projection.set_id(node, StyleAtomID(3));
        projection.set_language(node, StyleAtomID(4));
        projection.set_namespace(node, StyleAtomID(5));
        projection.set_part_exposure(node, StyleAtomID(6));
        projection.set_directionality(node, StyleAtomID(7));
        projection.set_custom_states(node, &[StyleAtomID(8)]);
        projection.set_parts(node, &[StyleAtomID(9)]);
        projection.set_class(node, StyleAtomID(10), true);
        let forms = AttributeNameForms {
            local: StyleAtomID(13),
            folded_name: StyleAtomID(14),
            folded_local: StyleAtomID(15),
        };
        projection.note_attribute_name_forms(StyleAtomID(11), forms);
        projection.note_attribute_name_forms(StyleAtomID(16), forms);
        projection.set_attribute(node, StyleAtomID(11), StyleAtomID(12), true);
        projection.commit();
        projection.release();

        let mut atoms = HashSet::default();
        assert_eq!(projection.collect_atoms(&mut atoms), 2);
        assert_eq!(atoms, (1..=15).map(StyleAtomID).collect());

        // A name another root keeps live keeps its forms live with it.
        let mut live: HashSet<StyleAtomID> = [StyleAtomID(16)].into_iter().collect();
        projection.extend_live_attribute_name_forms(&mut live);
        assert_eq!(live, [16, 13, 14, 15].into_iter().map(StyleAtomID).collect());

        projection.retire(node);
        projection.forget(node);
        projection.release();
        let mut atoms = HashSet::default();
        projection.collect_atoms(&mut atoms);
        assert!(atoms.is_empty());
    }

    #[test]
    fn the_tree_reads_links() {
        let projection = document();
        assert_eq!(
            projection.children(element(1)).collect::<Vec<_>>(),
            [element(2), element(3), element(4)]
        );
        assert_eq!(projection.preorder(element(1)).count(), 4);
        assert_eq!(projection.ancestors(element(4)).collect::<Vec<_>>(), [element(1)]);
    }

    #[test]
    fn an_element_without_facts_is_incomplete_rather_than_featureless() {
        let mut projection = document();
        projection.mint(element(5));
        projection.commit();
        assert!(projection.facts(element(5)).is_none());
        let mut subject = projection.subject_on(TransactionFactSide::After);
        assert!(matches!(subject.row(element(5)), Err(Incomplete::MissingFacts(_))));
        projection.ensure_facts(element(5));
        assert!(
            projection
                .subject_on(TransactionFactSide::After)
                .row(element(5))
                .is_err(),
            "facts are read as the last transaction committed them"
        );
        projection.commit();
        assert!(
            projection
                .subject_on(TransactionFactSide::After)
                .row(element(5))
                .is_ok()
        );
    }

    #[test]
    fn facts_are_kept_sorted_and_edits_that_change_nothing_are_free() {
        let mut projection = document();
        projection.set_class(element(2), StyleAtomID(5), true);
        projection.set_class(element(2), StyleAtomID(50), true);
        projection.set_class(element(2), ITEM, false);
        projection.commit();
        let facts = projection.facts(element(2)).unwrap();
        assert_eq!(facts.classes(), [StyleAtomID(5), StyleAtomID(50)]);
        projection.set_attribute(element(3), HREF, StyleAtomID(22), true);
        projection.set_attribute(element(3), StyleAtomID(15), StyleAtomID::NONE, true);
        projection.commit();
        let attributes = projection.facts(element(3)).unwrap().attributes();
        assert_eq!(
            attributes
                .iter()
                .map(|attribute| (attribute.name, attribute.value))
                .collect::<Vec<_>>(),
            [(StyleAtomID(15), StyleAtomID::NONE), (HREF, StyleAtomID(22))]
        );

        let mut fork = projection.clone();
        let version = fork.test_writer.rows_version();
        fork.set_class(element(2), StyleAtomID(5), true);
        fork.set_attribute(element(3), HREF, StyleAtomID(22), true);
        assert_eq!(
            fork.test_writer.rows_version(),
            version,
            "an edit that changes nothing copies no chunk"
        );
    }

    #[test]
    fn the_dispatch_bloom_follows_the_facts() {
        let mut projection = document();
        projection.note_attribute_name_forms(
            HREF,
            AttributeNameForms {
                local: StyleAtomID(22),
                folded_name: StyleAtomID::NONE,
                folded_local: StyleAtomID::NONE,
            },
        );
        projection.set_attribute(element(4), HREF, HREF_VALUE, true);
        projection.commit();
        let facts = projection.facts(element(4)).unwrap();
        let mut probes = Vec::new();
        projection.for_each_dispatch_probe(facts, true, |key, value| probes.push((key, value)));
        assert!(probes.contains(&(DispatchKey::Root, None)));
        assert!(probes.contains(&(DispatchKey::AttributeName(HREF), Some(HREF_VALUE))));
        assert!(probes.contains(&(DispatchKey::AttributeName(StyleAtomID(22)), Some(HREF_VALUE))));
        for (key, _) in probes {
            assert_ne!(facts.dispatch_bloom(true) & dispatch_bloom_bit(key), 0);
        }
        assert_eq!(facts.dispatch_bloom(false) & dispatch_bloom_bit(DispatchKey::Root), 0);

        projection.set_class(element(4), THEME, true);
        assert_eq!(
            projection.facts(element(4)).unwrap().dispatch_bloom(false) & dispatch_bloom_bit(DispatchKey::Class(THEME)),
            0,
            "an uncommitted write is not read"
        );
        projection.commit();
        assert_ne!(
            projection.facts(element(4)).unwrap().dispatch_bloom(false) & dispatch_bloom_bit(DispatchKey::Class(THEME)),
            0
        );
    }

    #[test]
    fn written_links_are_noted_until_the_transaction_is_released() {
        let mut projection = document();
        projection.release();
        assert!(!projection.has_written_links());

        // Facts alone write no links.
        projection.set_class(element(4), ITEM, true);
        assert!(!projection.has_written_links());

        let relations = projection.written_relations(element(4)).unwrap();

        projection.write_relations(element(4), None);
        projection.write_relations(
            element(3),
            Some(TreeRelations {
                next_element_sibling: None,
                ..projection.written_relations(element(3)).unwrap()
            }),
        );
        assert!(projection.relations_were_written(element(4)));
        assert!(projection.relations_were_written(element(3)));
        assert!(!projection.relations_were_written(element(2)));
        projection.set_first_element_child(element(2), None);
        assert!(
            !projection.relations_were_written(element(2)),
            "a first element child is no relation"
        );
        assert_eq!(
            projection.links_written().collect::<Vec<_>>(),
            [element(4), element(3), element(2)]
        );
        assert_eq!(
            projection.relations_written().collect::<Vec<_>>(),
            [element(4), element(3)]
        );
        assert_eq!(projection.written_relations(element(4)), None);
        assert_eq!(projection.committed_relations(element(4)), Some(relations));

        projection.commit();
        assert!(projection.has_written_links(), "the barrier settles what was written");
        assert_eq!(projection.committed_relations(element(4)), None);
        projection.release();
        assert!(!projection.has_written_links());
        assert!(!projection.relations_were_written(element(4)));
    }

    #[test]
    fn a_fork_shares_rows_until_either_side_writes() {
        let projection = document();
        let mut fork = projection.clone();
        fork.set_class(element(4), ITEM, true);
        fork.set_folded_tag(element(4), StyleAtomID(2));
        fork.set_folded_tag(element(3), DIV);
        fork.commit();
        assert!(projection.facts(element(4)).unwrap().classes().is_empty());
        assert_eq!(fork.facts(element(4)).unwrap().classes(), [ITEM]);
        assert_eq!(fork.facts(element(4)).unwrap().folded_tag(), StyleAtomID(2));
        assert_eq!(fork.facts(element(3)).unwrap().folded_tag(), StyleAtomID::NONE);
        // The class list of an untouched element is the same allocation on both sides.
        let ours = projection.facts(element(2)).unwrap().classes();
        let theirs = fork.facts(element(2)).unwrap().classes();
        assert!(!ours.is_empty() && std::ptr::eq(ours.as_ptr(), theirs.as_ptr()));
    }

    #[test]
    fn shadow_trees_and_slots_shape_the_style_order() {
        // host(1) -> shadow root(2) -> slot(3); host's light child 4 is assigned to the slot, its
        // light child 5 is not; text 1 is assigned too.
        let mut projection = Projection::default();
        for index in 1..=5 {
            projection.mint(element(index));
        }
        projection.set_shadow_root(element(1), element(2));
        projection.set_first_element_child(element(2), Some(element(3)));
        projection.set_parent(element(3), Some(element(2)));
        projection.set_first_element_child(element(1), Some(element(4)));
        projection.set_parent(element(4), Some(element(1)));
        projection.set_next_element_sibling(element(4), Some(element(5)));
        projection.set_parent(element(5), Some(element(1)));
        projection.set_previous_element_sibling(element(5), Some(element(4)));
        projection.set_assigned_slot(element(4), Some(element(3)));
        let text = StyleNodeID::text(1);
        projection.set_assigned_nodes(element(3), &[element(4), text]);
        projection.commit();

        assert_eq!(projection.host_of(element(2)), Some(element(1)));
        assert_eq!(projection.inheritance_parent(element(3)), Some(element(1)));
        assert_eq!(projection.inheritance_parent(element(4)), Some(element(3)));
        assert_eq!(projection.shadow_host_of(element(3)), Some(element(1)));
        assert_eq!(projection.assigned_slot_of(text), Some(element(3)));
        assert_eq!(
            projection.style_preorder(element(1)).collect::<Vec<_>>(),
            [element(1), element(3), element(4), element(5)]
        );

        projection.retire_texts([text]);
        assert_eq!(projection.assigned_nodes_of(element(3)), [element(4)]);
        projection.retire(element(4));
        assert!(projection.assigned_nodes_of(element(3)).is_empty());
        projection.retire(element(3));
        projection.retire(element(2));
        assert_eq!(projection.shadow_root_of(element(1)), None);

        // A new root for the host unpairs the old one.
        projection.mint(element(2));
        projection.mint(element(6));
        projection.set_shadow_root(element(1), element(2));
        projection.set_shadow_root(element(1), element(6));
        assert_eq!(projection.host_of(element(2)), None);
        assert_eq!(projection.host_of(element(6)), Some(element(1)));
    }

    #[test]
    fn forgotten_atoms_leave_the_catalogs() {
        let mut projection = document();
        let text: Vec<u16> = "https://example.com".encode_utf16().collect();
        projection.set_attribute_value_text(HREF_VALUE, RetainedUtf16FlyString::from_utf16(&text));
        projection.set_attribute_value_text(HREF_VALUE, RetainedUtf16FlyString::from_utf16(&[]));
        assert_eq!(
            projection
                .attribute_value_text(HREF_VALUE)
                .map(RetainedUtf16FlyString::to_utf16)
                .as_deref(),
            Some(&text[..])
        );
        projection.set_language(element(2), StyleAtomID(30));
        projection.set_language_text(StyleAtomID(30), &[u16::from(b'e'), u16::from(b'n')]);
        projection.commit();
        assert_eq!(projection.language_tag_of(element(2)).len(), 2);
        projection.forget_atoms(&[HREF_VALUE, StyleAtomID(30)]);
        assert_eq!(projection.attribute_value_text(HREF_VALUE), None);
        assert!(projection.language_tag_of(element(2)).is_empty());

        projection.forget(element(2));
        assert!(projection.facts(element(2)).is_some());
        projection.commit();
        assert!(projection.facts(element(2)).is_none());
        assert!(projection.is_live(element(2)));
    }
}
