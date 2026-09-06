/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Style feature atoms and the local facts selector evaluation reads.
//!
//! An atom is a document-local `u32` standing for a tag/namespace pair, an ID, a class, an
//! attribute name, an attribute value, a namespace, or a state fact. C++ assigns the identities
//! because it owns the authoritative string payloads; Rust stores only the `u32` in bytecode,
//! postings, and journal keys, and never a duplicate of the string. Interning a name on the C++
//! side is a hash lookup plus a reference count bump on a string that already exists, so a feature
//! comparison costs an integer compare and no conversion.
//!
//! Index keys are therefore semantic facts rather than strings. Substring and token attribute
//! operators drive from an attribute-name posting and run their exact value test in compound
//! bytecode instead of demanding a posting per substring, which is why a feature key names the
//! attribute and not the shape of the test applied to it. Only those tests need the value as text,
//! and only for the attributes an attached program actually mentions.

use super::AncestorDispatchShape;
use super::capacity::ShallowCapacityBytes;
use super::capacity::capacity_bytes;
use super::column::BitColumn;
use super::column::Column;
use super::column::EpochColumn;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::column::PagedValuePage;
use super::column::RemovablePagedColumnPage;
use super::column::advance_epoch;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::FastSet as HashSet;
use crate::css::style_value::RetainedStyleValueData;
use std::cell::Cell;
use std::cmp::Reverse;
use std::rc::Rc;

use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::memory::Tier;
use super::partial_view::Lookup;
use super::prefix::PrefixAutomaton;
use super::program::CustomDeclaration;
use super::program::DeclaredProperty;
use super::program::EntryID;
use super::program::RuleID;
use super::program::SelectorProgramID;
use super::selector::AttributeCase;
use super::selector::AttributeOperator;
use super::selector::AttributeTest;
use super::sorted_merge::SortedMergeEntry;
use super::sorted_merge::merge_sorted_by;
use super::transaction::ElementDeclarationKind;
use super::transaction::StateFact;
use super::tree::StyleNodeID;

define_id! {
    /// Document-local identity of an interned selector-mentioned feature. Obeys the same generation
    /// and non-reuse rules as [`super::tree::StyleNodeID`].
    default pub struct StyleAtomID(pub);
}

impl StyleAtomID {
    /// Reserved: no atom. Document-local atoms start at 1.
    pub const NONE: Self = Self(0);

    #[must_use]
    pub fn is_none(self) -> bool {
        self == Self::NONE
    }
}

fn attribute_value_equals(value: &[u16], literal: &[u16], insensitive: bool) -> bool {
    if !insensitive {
        return value == literal;
    }
    value.len() == literal.len()
        && value.iter().zip(literal).all(|(&left, &right)| {
            left == right
                || match (u8::try_from(left), u8::try_from(right)) {
                    (Ok(left), Ok(right)) => left.eq_ignore_ascii_case(&right),
                    _ => false,
                }
        })
}

fn attribute_value_starts_with(value: &[u16], literal: &[u16], insensitive: bool) -> bool {
    value.len() >= literal.len() && attribute_value_equals(&value[..literal.len()], literal, insensitive)
}

fn attribute_value_may_match(value: &[u16], literal: &[u16], operator: AttributeOperator, insensitive: bool) -> bool {
    match operator {
        AttributeOperator::Presence => true,
        AttributeOperator::Exact => attribute_value_equals(value, literal, insensitive),
        AttributeOperator::Includes => {
            !literal.is_empty()
                && value
                    .split(|unit| matches!(unit, 0x20 | 0x09 | 0x0A | 0x0C | 0x0D))
                    .any(|token| attribute_value_equals(token, literal, insensitive))
        }
        AttributeOperator::DashMatch => {
            attribute_value_equals(value, literal, insensitive)
                || (value.len() > literal.len()
                    && value[literal.len()] == u16::from(b'-')
                    && attribute_value_starts_with(value, literal, insensitive))
        }
        AttributeOperator::Prefix => !literal.is_empty() && attribute_value_starts_with(value, literal, insensitive),
        AttributeOperator::Suffix => {
            !literal.is_empty()
                && value.len() >= literal.len()
                && attribute_value_equals(&value[value.len() - literal.len()..], literal, insensitive)
        }
        AttributeOperator::Substring => {
            !literal.is_empty()
                && value.len() >= literal.len()
                && (0..=value.len() - literal.len())
                    .any(|start| attribute_value_equals(&value[start..start + literal.len()], literal, insensitive))
        }
    }
}

/// Which local fact of a style node is being described.
///
/// Attribute presence and attribute value share one key: changing an attribute changes both facts
/// at once, and splitting them would let a consumer handle one and miss the other.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum LocalFeatureKey {
    /// The element's qualified name, as a single interned tag/namespace atom.
    TagName,
    /// The ASCII-lowercase folding of that name, recorded only when it differs from it. A type
    /// selector dispatches on its own folded form, so an element whose local name is not already
    /// lowercase has to be reachable under both.
    FoldedTagName,
    /// The element's ID.
    Id,
    /// Membership of one class.
    Class(StyleAtomID),
    /// Exposure of one shadow part.
    Part(StyleAtomID),
    /// Membership of one custom state, which `:state()` tests.
    CustomState(StyleAtomID),
    /// Whether the element has no children. A text node arriving changes it and connects no
    /// element, so it cannot be carried by a tree delta.
    Emptiness,
    /// Presence and value of one attribute, named by its interned attribute-name atom.
    Attribute(StyleAtomID),
    /// The element's resolved language tag. `:lang()` compares extended language ranges, which a
    /// single atom cannot express, so the fact is the whole tag and every `:lang()` rule hears any
    /// change to it. What that costs is bounded by how rare `:lang()` is; what it buys is that the
    /// region stays exactly the elements whose tag moved.
    Language,
    /// The element's resolved directionality, which `:dir()` tests.
    Directionality,
    /// The element's computed heading level, which `:heading()` tests.
    HeadingLevel,
    /// The outermost host from whose scope a `::part()` rule can address this element.
    ///
    /// A part name says nothing on its own about how far out it reaches: `exportparts` forwards it
    /// through a host under a name that is often the same one, so the names an element carries can
    /// stay exactly as they were while the scopes able to address it change. The value is compared
    /// only against its own previous value, never against anything a selector names.
    PartExposure,
    /// Every fact an element announced while it was arriving, as one key.
    ///
    /// An element publishes its tag, its id, its classes, its attributes and its states as it
    /// connects, and journalling each of them separately is what a page of a few thousand elements
    /// spends its whole transaction budget on. They all describe the same event, so the journal
    /// keeps one entry per arriving element and routing reads the facts back off the element.
    ArrivingFacts,
}

/// The value of a local fact.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FeatureValue {
    /// The fact does not hold: no such class, no such attribute, no ID.
    Absent,
    /// The fact holds, but its payload is not internable, so an exact test reads the live DOM
    /// after cheaper atom and name checks have already passed.
    Present,
    /// The fact holds and its payload is not the one it held before. Distinct from [`Self::Present`]
    /// so that a value change is a change: a delta reporting presence on both sides cancels, which
    /// is right for a fact nothing can distinguish and wrong for one that moved.
    ChangedValue,
    /// The fact holds with an interned value.
    Atom(StyleAtomID),
    /// The fact holds with a numeric value.
    Number(u32),
}

impl FeatureValue {
    #[must_use]
    pub fn holds(self) -> bool {
        !matches!(self, Self::Absent)
    }
}

/// Element or document states, packed one bit per fact. One word covers every boolean
/// pseudo-class the parser can produce.
#[derive(Clone, Copy, Debug, Default, Hash, PartialEq, Eq)]
pub struct StateSet(pub u64);

impl StateSet {
    /// The facts this set holds, for routing an element that announced them all at once.
    pub fn facts(self) -> impl Iterator<Item = StateFact> {
        let valid_bits = 1_u64
            .checked_shl(u32::try_from(StateFact::ALL.len()).expect("state fact count exceeds u32"))
            .map_or(u64::MAX, |limit| limit - 1);
        let mut bits = self.0 & valid_bits;
        std::iter::from_fn(move || {
            if bits == 0 {
                return None;
            }
            let index = bits.trailing_zeros() as usize;
            bits &= bits - 1;
            Some(StateFact::ALL[index])
        })
    }

    #[must_use]
    pub fn contains(self, fact: StateFact) -> bool {
        self.0 & (1_u64 << fact as u32) != 0
    }

    pub fn insert(&mut self, fact: StateFact) {
        self.0 |= 1_u64 << fact as u32;
    }

    pub fn remove(&mut self, fact: StateFact) {
        self.0 &= !(1_u64 << fact as u32);
    }
}

/// One attribute of one style node.
///
/// `value` is the interned value when the value was worth interning. Derived name forms and value
/// text live once in the batch's shared atom catalog rather than once per element.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttributeFact {
    pub name: StyleAtomID,
    pub value: StyleAtomID,
    pub text_offset: u32,
    pub text_length: u32,
}

/// The other names one attribute-name atom answers to.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AttributeNameForms {
    pub local: StyleAtomID,
    pub folded_name: StyleAtomID,
    pub folded_local: StyleAtomID,
}

#[derive(Clone)]
struct PagedCopyColumn<T: Copy + Default> {
    values: PagedColumn<PagedValuePage<T>>,
    indices: Vec<usize>,
}

impl<T: Copy + Default> Default for PagedCopyColumn<T> {
    fn default() -> Self {
        Self {
            values: PagedColumn::default(),
            indices: Vec::new(),
        }
    }
}

impl<T: Copy + Default> PagedCopyColumn<T> {
    fn get(&self, index: usize) -> Option<T> {
        self.values.get(index)
    }

    fn insert(&mut self, index: usize, value: T) {
        let (previous, _) = self.values.insert(index, value);
        if previous.is_none() {
            self.indices.push(index);
        }
    }

    fn indexed_iter(&self) -> impl Iterator<Item = (usize, T)> + '_ {
        self.indices.iter().copied().map(|index| {
            (
                index,
                self.values.get(index).expect("paged column index must remain present"),
            )
        })
    }
}

impl<T: Copy + Default> ShallowCapacityBytes for PagedCopyColumn<T> {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.values.capacity_bytes() + self.indices.shallow_capacity_bytes()
    }
}

#[derive(Clone)]
struct PagedOwnedColumn<T: Clone + Default> {
    handles: PagedColumn<PagedValuePage<u32>>,
    values: Vec<T>,
    indices: Vec<usize>,
}

impl<T: Clone + Default> Default for PagedOwnedColumn<T> {
    fn default() -> Self {
        Self {
            handles: PagedColumn::default(),
            values: Vec::new(),
            indices: Vec::new(),
        }
    }
}

impl<T: Clone + Default> PagedOwnedColumn<T> {
    fn get(&self, index: usize) -> Option<&T> {
        let handle = self.handles.get(index)? as usize;
        self.values.get(handle)
    }

    fn get_mut(&mut self, index: usize) -> Option<&mut T> {
        let handle = self.handles.get(index)? as usize;
        self.values.get_mut(handle)
    }

    fn entry(&mut self, index: usize) -> &mut T {
        let handle = match self.handles.get(index) {
            Some(handle) => handle,
            None => {
                let handle = u32::try_from(self.values.len()).expect("paged owned column handle overflow");
                self.values.push(T::default());
                self.indices.push(index);
                self.handles.insert(index, handle);
                handle
            }
        };
        &mut self.values[handle as usize]
    }

    fn insert(&mut self, index: usize, value: T) {
        *self.entry(index) = value;
    }

    fn iter(&self) -> impl Iterator<Item = &T> {
        self.values.iter()
    }

    fn iter_mut(&mut self) -> impl Iterator<Item = &mut T> {
        self.values.iter_mut()
    }

    fn indexed_iter(&self) -> impl Iterator<Item = (usize, &T)> {
        self.indices.iter().copied().zip(self.values.iter())
    }

    fn indexed_iter_mut(&mut self) -> impl Iterator<Item = (usize, &mut T)> {
        self.indices.iter().copied().zip(self.values.iter_mut())
    }
}

impl<T: Clone + Default> ShallowCapacityBytes for PagedOwnedColumn<T> {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.handles.capacity_bytes() + self.values.shallow_capacity_bytes() + self.indices.shallow_capacity_bytes()
    }
}

#[derive(Clone, Default)]
struct AttributeCatalogs {
    name_forms: PagedCopyColumn<AttributeNameForms>,
    value_texts: PagedOwnedColumn<Option<Box<[u16]>>>,
    language_texts: PagedOwnedColumn<Option<Box<[u16]>>>,
}

const NO_ROW: u32 = u32::MAX;

#[derive(Clone, Copy, Default)]
struct PayloadHandle {
    offset: u32,
    length: u32,
}

impl PayloadHandle {
    fn appended_to<T>(payload: &mut Vec<T>, values: &[T]) -> Self
    where
        T: Clone,
    {
        let offset = u32::try_from(payload.len()).expect("fact payload offset overflow");
        payload.extend_from_slice(values);
        Self {
            offset,
            length: u32::try_from(values.len()).expect("fact payload length overflow"),
        }
    }

    fn slice<T>(self, payload: &[T]) -> &[T] {
        let start = self.offset as usize;
        &payload[start..start + self.length as usize]
    }
}

#[derive(Clone, Copy, Default)]
struct RareFacts {
    heading_level: u8,
    part_exposure: StyleAtomID,
    custom_states: PayloadHandle,
    parts: PayloadHandle,
}

#[derive(Clone, Copy)]
struct PrimaryFactSnapshot {
    tag: StyleAtomID,
    folded_tag: StyleAtomID,
    id: StyleAtomID,
    states: StateSet,
    directionality: StyleAtomID,
    language: StyleAtomID,
    has_text_content: bool,
    namespace: StyleAtomID,
    rare_facts: Option<RareFacts>,
    class_handle: PayloadHandle,
    attribute_handle: PayloadHandle,
}

const RARE_FACT_PAGE_SHIFT: usize = 6;
const RARE_FACT_PAGE_SIZE: usize = 1 << RARE_FACT_PAGE_SHIFT;

#[derive(Clone)]
struct RareFactPage {
    rows: [Option<RareFacts>; RARE_FACT_PAGE_SIZE],
}

impl Default for RareFactPage {
    fn default() -> Self {
        Self {
            rows: [None; RARE_FACT_PAGE_SIZE],
        }
    }
}

impl PagedColumnPage for RareFactPage {
    type Value = RareFacts;

    const SHIFT: usize = RARE_FACT_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<Self::Value> {
        self.rows[index]
    }

    fn insert(&mut self, index: usize, value: Self::Value) {
        self.rows[index] = Some(value);
    }
}

impl RemovablePagedColumnPage for RareFactPage {
    fn remove(&mut self, index: usize) -> Option<Self::Value> {
        self.rows[index].take()
    }
}

/// The next unique batch-row-space name; see `StyleNodeFacts::generation`.
fn next_batch_generation() -> u64 {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

/// Columnar local facts for style nodes.
///
/// `ElementFactStore` owns one instance as the required document projection. Bounded before-side
/// and selective-evaluation batches reuse the same physical row format without becoming another
/// source of truth.
///
/// Rows are addressed by element index so that following a tree relation into another row is an
/// array lookup rather than a search. That directness is the point: relation steps have to stay
/// inside the evaluator.
#[derive(Clone, Default)]
pub struct StyleNodeFacts {
    attribute_catalogs: Rc<AttributeCatalogs>,
    primary: bool,
    resident: BitColumn,
    rare_facts: PagedColumn<RareFactPage>,
    nodes: Vec<StyleNodeID>,
    row_by_element_index: Vec<u32>,
    /// Bloom summary of every dispatch key carried by a row, excluding document-root status.
    dispatch_bloom: Vec<u64>,
    /// Rows no longer reachable through the element mapping. The primary arrangement repoints an
    /// element to a freshly packed row when its facts move; the old row stays as garbage until
    /// its measured carrying cost makes one rebuild cheaper than retaining it.
    stale_rows: u32,
    live_rows: usize,
    /// Names this batch's row space. Consumers holding row indices across calls compare it:
    /// clearing a batch renumbers every row, so anything indexed by row is stale the moment the
    /// generation moves. Appending keeps the generation, since existing rows keep their places.
    /// Zero marks a default-constructed batch whose consumers must never trust held rows.
    generation: u64,

    tag: Vec<StyleAtomID>,
    /// The ASCII-lowercase folding of `tag`, or none when the name is already lowercase. Dispatch
    /// buckets rules under a type selector's folded form, so a candidate has to probe both.
    folded_tag: Vec<StyleAtomID>,
    id: Vec<StyleAtomID>,
    states: Vec<StateSet>,
    /// Values the parameterized state operators test: the element's resolved directionality, its
    /// resolved language, and its heading level where it has one.
    directionality: Vec<StyleAtomID>,
    language: Vec<StyleAtomID>,
    /// Whether the element holds a text or comment child that keeps it from being `:empty`.
    has_text_content: Vec<bool>,
    /// The element's resolved language tag as a range into `text`. `:lang()` compares extended
    /// language ranges against it, and a range is not a name, so the atom cannot answer the test.
    language_text: Vec<(u32, u32)>,
    /// The element's namespace URI, keyed by its text. A type or universal selector constrains it
    /// whenever it was written with a prefix or its sheet declared a default namespace.
    namespace: Vec<StyleAtomID>,
    heading_level: Vec<u8>,
    custom_state_handles: Vec<PayloadHandle>,
    custom_states: Vec<StyleAtomID>,
    /// The part names the element is exposed under. `::part()` dispatches on one, so a row that did
    /// not carry them would make every such rule unreachable rather than merely slower.
    part_handles: Vec<PayloadHandle>,
    parts: Vec<StyleAtomID>,
    /// The host a `::part()` rule addresses the element from, which `exportparts` moves outwards.
    /// The outer compound of such a rule describes that host and not the one the part sits under.
    part_exposure: Vec<StyleAtomID>,

    class_handles: Vec<PayloadHandle>,
    classes: Vec<StyleAtomID>,
    attribute_handles: Vec<PayloadHandle>,
    attributes: Vec<AttributeFact>,

    /// Batch-local UTF-16 for language tags and non-interned attribute values.
    text: Vec<u16>,
}

pub(super) struct MatchingFactBatch {
    facts: Rc<StyleNodeFacts>,
    charged_bytes: u64,
}

impl MatchingFactBatch {
    fn owned(facts: StyleNodeFacts) -> Self {
        let charged_bytes = facts.capacity_bytes();
        Self {
            facts: Rc::new(facts),
            charged_bytes,
        }
    }

    fn primary_view(facts: Rc<StyleNodeFacts>) -> Self {
        Self {
            facts,
            charged_bytes: 0,
        }
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        self.charged_bytes
    }
}

impl std::ops::Deref for MatchingFactBatch {
    type Target = StyleNodeFacts;

    fn deref(&self) -> &Self::Target {
        &self.facts
    }
}

impl From<StyleNodeFacts> for MatchingFactBatch {
    fn from(facts: StyleNodeFacts) -> Self {
        Self::owned(facts)
    }
}

impl StyleNodeFacts {
    #[must_use]
    pub fn new() -> Self {
        Self {
            generation: next_batch_generation(),
            ..Self::default()
        }
    }

    fn new_primary() -> Self {
        Self {
            primary: true,
            generation: next_batch_generation(),
            ..Self::default()
        }
    }

    /// Append one row. Rows must be appended in the order the batch reports them.
    pub fn push_row(
        &mut self,
        node: StyleNodeID,
        tag: StyleAtomID,
        id: StyleAtomID,
        states: StateSet,
        classes: &[StyleAtomID],
        attributes: &[AttributeFact],
    ) {
        assert!(!self.primary, "cannot append a batch row to primary facts");
        let row = u32::try_from(self.nodes.len()).expect("fact batch row space exhausted");
        self.nodes.push(node);
        self.tag.push(tag);
        self.folded_tag.push(StyleAtomID::NONE);
        self.id.push(id);
        self.states.push(states);
        self.directionality.push(StyleAtomID::NONE);
        self.language.push(StyleAtomID::NONE);
        self.has_text_content.push(false);
        self.language_text.push((0, 0));
        self.namespace.push(StyleAtomID::NONE);
        self.heading_level.push(0);
        self.custom_state_handles.push(PayloadHandle::default());
        self.part_handles.push(PayloadHandle::default());
        self.part_exposure.push(StyleAtomID::NONE);
        self.class_handles
            .push(PayloadHandle::appended_to(&mut self.classes, classes));
        self.attribute_handles
            .push(PayloadHandle::appended_to(&mut self.attributes, attributes));
        self.dispatch_bloom.push(0);
        self.dispatch_bloom[row as usize] = self.compute_dispatch_bloom_of(row);

        self.map_row(node, row);
    }

    /// Append one row from another packed fact arrangement without materializing an owned row.
    fn push_row_from(&mut self, node: StyleNodeID, source: &Self, source_row: u32) {
        assert!(!self.primary, "cannot append a batch row to primary facts");
        let row = u32::try_from(self.nodes.len()).expect("fact batch row space exhausted");
        self.nodes.push(node);
        self.tag.push(source.tag_of(source_row));
        self.folded_tag.push(source.folded_tag_of(source_row));
        self.id.push(source.id_of(source_row));
        self.states.push(source.states_of(source_row));
        self.directionality.push(source.directionality_of(source_row));
        self.language.push(source.language_of(source_row));
        self.has_text_content.push(source.has_text_content_of(source_row));
        let language_text = source.language_tag_of(source_row);
        let language_offset = u32::try_from(self.text.len()).expect("fact text space exhausted");
        self.text.extend_from_slice(language_text);
        self.language_text.push((
            language_offset,
            u32::try_from(language_text.len()).expect("fact text space exhausted"),
        ));
        self.namespace.push(source.namespace_of(source_row));
        self.heading_level.push(source.heading_level_of(source_row));
        self.custom_state_handles.push(PayloadHandle::appended_to(
            &mut self.custom_states,
            source.custom_states_of(source_row),
        ));
        self.part_handles
            .push(PayloadHandle::appended_to(&mut self.parts, source.parts_of(source_row)));
        self.part_exposure.push(source.part_exposure_of(source_row));
        self.class_handles.push(PayloadHandle::appended_to(
            &mut self.classes,
            source.classes_of(source_row),
        ));
        let attribute_start = u32::try_from(self.attributes.len()).expect("attribute payload offset overflow");
        for &attribute in source.attributes_of(source_row) {
            let (text_offset, text_length) = match source.local_text_of(attribute) {
                Some(text) => self.push_text(text),
                None => (u32::MAX, 0),
            };
            self.attributes.push(AttributeFact {
                text_offset,
                text_length,
                ..attribute
            });
        }
        self.attribute_handles.push(PayloadHandle {
            offset: attribute_start,
            length: u32::try_from(self.attributes.len()).expect("attribute payload overflow") - attribute_start,
        });
        self.dispatch_bloom.push(0);
        self.dispatch_bloom[row as usize] = self.compute_dispatch_bloom_of(row);

        self.map_row(node, row);
    }

    fn primary_snapshot(&self, row: u32) -> PrimaryFactSnapshot {
        assert!(self.primary);
        PrimaryFactSnapshot {
            tag: self.tag_of(row),
            folded_tag: self.folded_tag_of(row),
            id: self.id_of(row),
            states: self.states_of(row),
            directionality: self.directionality_of(row),
            language: self.language_of(row),
            has_text_content: self.has_text_content_of(row),
            namespace: self.namespace_of(row),
            rare_facts: self.rare_facts.get(row as usize),
            class_handle: self.class_handles[row as usize],
            attribute_handle: self.attribute_handles[row as usize],
        }
    }

    fn push_row_from_primary_snapshot(&mut self, node: StyleNodeID, source: &Self, snapshot: PrimaryFactSnapshot) {
        assert!(!self.primary);
        assert!(source.primary);
        let rare_facts = snapshot.rare_facts.unwrap_or_default();
        self.push_row(
            node,
            snapshot.tag,
            snapshot.id,
            snapshot.states,
            snapshot.class_handle.slice(&source.classes),
            snapshot.attribute_handle.slice(&source.attributes),
        );
        self.set_row_folded_tag(snapshot.folded_tag);
        self.set_row_namespace(snapshot.namespace);
        self.set_row_part_exposure(rare_facts.part_exposure);
        self.set_row_has_text_content(snapshot.has_text_content);
        let row = u32::try_from(self.row_count() - 1).expect("fact batch row space exhausted");
        self.set_row_parameters(
            row,
            snapshot.directionality,
            snapshot.language,
            rare_facts.heading_level,
            rare_facts.custom_states.slice(&source.custom_states),
            rare_facts.parts.slice(&source.parts),
        );
    }

    /// Append UTF-16 text whose value has no atom and return its batch-local range.
    pub fn push_text(&mut self, text: &[u16]) -> (u32, u32) {
        let offset = u32::try_from(self.text.len()).expect("text payload overflow");
        self.text.extend_from_slice(text);
        (offset, u32::try_from(text.len()).expect("text payload overflow"))
    }

    fn map_row(&mut self, node: StyleNodeID, row: u32) {
        debug_assert!(!self.primary);
        let Some(index) = node.element_index() else {
            return;
        };
        let index = index as usize;
        if index >= self.row_by_element_index.len() {
            self.row_by_element_index.resize(index + 1, NO_ROW);
        }
        if self.row_by_element_index[index] != NO_ROW {
            self.stale_rows += 1;
        } else {
            self.live_rows += 1;
        }
        self.row_by_element_index[index] = row;
    }

    #[must_use]
    pub fn row_count(&self) -> usize {
        if self.primary { self.tag.len() } else { self.nodes.len() }
    }

    #[must_use]
    pub fn node_at(&self, row: u32) -> StyleNodeID {
        if self.primary {
            assert!(self.resident.contains(row as usize));
            StyleNodeID::element(row)
        } else {
            self.nodes[row as usize]
        }
    }

    #[must_use]
    pub fn has_row(&self, row: u32) -> bool {
        let row = row as usize;
        if self.primary {
            self.resident.contains(row)
        } else {
            row < self.nodes.len() && self.row_of(self.nodes[row]) == u32::try_from(row).ok()
        }
    }

    /// The row holding `node`'s facts, or `None` when the batch does not cover it. A miss is not a
    /// negative answer: it means the evaluator has to widen or ask for a different batch.
    /// Detach one element's row from the mapping, leaving the packed row as garbage.
    pub fn forget_row(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index() else {
            return;
        };
        if self.primary {
            if !self.resident.contains(index as usize) {
                return;
            }
            self.resident.set(index as usize, false);
            self.live_rows -= 1;
            self.rare_facts.remove(index as usize);
            self.class_handles[index as usize] = PayloadHandle::default();
            self.attribute_handles[index as usize] = PayloadHandle::default();
            return;
        }
        if let Some(slot) = self.row_by_element_index.get_mut(index as usize)
            && *slot != NO_ROW
        {
            *slot = NO_ROW;
            self.stale_rows += 1;
            self.live_rows -= 1;
        }
    }

    #[must_use]
    pub fn stale_rows(&self) -> u32 {
        if self.primary { 0 } else { self.stale_rows }
    }

    #[must_use]
    pub fn generation(&self) -> u64 {
        self.generation
    }

    #[must_use]
    pub fn row_of(&self, node: StyleNodeID) -> Option<u32> {
        let index = node.element_index()? as usize;
        if self.primary {
            return self
                .resident
                .contains(index)
                .then(|| u32::try_from(index).expect("element fact row exceeds u32"));
        }
        match self.row_by_element_index.get(index) {
            Some(&NO_ROW) | None => None,
            Some(&row) => Some(row),
        }
    }

    fn live_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        (0..self.row_count()).filter_map(|row| {
            let node = if self.primary {
                if row == 0 || !self.resident.contains(row) {
                    return None;
                }
                StyleNodeID::element(u32::try_from(row).expect("element fact row exceeds u32"))
            } else {
                self.nodes[row]
            };
            (self.row_of(node) == u32::try_from(row).ok()).then_some(node)
        })
    }

    #[must_use]
    pub fn live_row_count(&self) -> usize {
        self.live_rows
    }

    fn set_primary_row(&mut self, node: StyleNodeID, facts: &StagedFactRow) -> u64 {
        assert!(self.primary, "cannot index a primary row in a batch");
        let row = node.element_index().expect("fact row must be an element") as usize;
        let was_resident = self.resident.contains(row);
        self.live_rows += usize::from(!was_resident);
        let old_rare_facts = was_resident.then(|| self.rare_facts.get(row)).flatten();
        let old_class_handle = was_resident.then(|| self.class_handles[row]);
        let old_attribute_handle = was_resident.then(|| self.attribute_handles[row]);
        let mut stale_payload_bytes = 0_u64;
        if row >= self.tag.len() {
            let length = row.checked_add(1).expect("element fact row space exhausted");
            self.tag.resize(length, StyleAtomID::NONE);
            self.folded_tag.resize(length, StyleAtomID::NONE);
            self.id.resize(length, StyleAtomID::NONE);
            self.states.resize(length, StateSet::default());
            self.directionality.resize(length, StyleAtomID::NONE);
            self.language.resize(length, StyleAtomID::NONE);
            self.has_text_content.resize(length, false);
            self.language_text.resize(length, (0, 0));
            self.namespace.resize(length, StyleAtomID::NONE);
            self.class_handles.resize(length, PayloadHandle::default());
            self.attribute_handles.resize(length, PayloadHandle::default());
            self.dispatch_bloom.resize(length, 0);
        }

        self.tag[row] = facts.tag;
        self.folded_tag[row] = facts.folded_tag;
        self.id[row] = facts.id;
        self.states[row] = facts.states;
        self.directionality[row] = facts.directionality;
        self.language[row] = facts.language;
        self.has_text_content[row] = facts.has_text_content;
        self.language_text[row] = (0, 0);
        self.namespace[row] = facts.namespace;
        if facts.heading_level != 0
            || !facts.custom_states.is_empty()
            || !facts.parts.is_empty()
            || !facts.part_exposure.is_none()
        {
            let custom_states = old_rare_facts
                .filter(|old| old.custom_states.slice(&self.custom_states) == facts.custom_states)
                .map_or_else(
                    || {
                        stale_payload_bytes += old_rare_facts.map_or(0, |old| {
                            size_of_val(old.custom_states.slice(&self.custom_states)) as u64
                        });
                        PayloadHandle::appended_to(&mut self.custom_states, &facts.custom_states)
                    },
                    |old| old.custom_states,
                );
            let parts = old_rare_facts
                .filter(|old| old.parts.slice(&self.parts) == facts.parts)
                .map_or_else(
                    || {
                        stale_payload_bytes +=
                            old_rare_facts.map_or(0, |old| size_of_val(old.parts.slice(&self.parts)) as u64);
                        PayloadHandle::appended_to(&mut self.parts, &facts.parts)
                    },
                    |old| old.parts,
                );
            self.rare_facts.insert(
                row,
                RareFacts {
                    heading_level: facts.heading_level,
                    part_exposure: facts.part_exposure,
                    custom_states,
                    parts,
                },
            );
        } else {
            stale_payload_bytes += old_rare_facts.map_or(0, |old| {
                (size_of_val(old.custom_states.slice(&self.custom_states)) + size_of_val(old.parts.slice(&self.parts)))
                    as u64
            });
            self.rare_facts.remove(row);
        }
        self.class_handles[row] = old_class_handle
            .filter(|old| old.slice(&self.classes) == facts.classes)
            .unwrap_or_else(|| {
                stale_payload_bytes += old_class_handle.map_or(0, |old| size_of_val(old.slice(&self.classes)) as u64);
                PayloadHandle::appended_to(&mut self.classes, &facts.classes)
            });
        let attributes_are_unchanged = old_attribute_handle.is_some_and(|old| {
            old.slice(&self.attributes)
                .iter()
                .map(|attribute| (attribute.name, attribute.value))
                .eq(facts.attributes.iter().copied())
        });
        if !attributes_are_unchanged {
            stale_payload_bytes +=
                old_attribute_handle.map_or(0, |old| size_of_val(old.slice(&self.attributes)) as u64);
            let offset = u32::try_from(self.attributes.len()).expect("fact payload offset overflow");
            self.attributes
                .extend(facts.attributes.iter().map(|&(name, value)| AttributeFact {
                    name,
                    value,
                    text_offset: u32::MAX,
                    text_length: 0,
                }));
            self.attribute_handles[row] = PayloadHandle {
                offset,
                length: u32::try_from(facts.attributes.len()).expect("fact payload length overflow"),
            };
        }
        self.resident.set(row, true);
        self.dispatch_bloom[row] = self.compute_dispatch_bloom_of(row as u32);
        stale_payload_bytes
    }

    /// Record the folded name of the row just pushed. Only a name that is not already lowercase
    /// has one.
    pub fn set_row_folded_tag(&mut self, folded: StyleAtomID) {
        let row = self.folded_tag.len() - 1;
        self.folded_tag[row] = folded;
        if !folded.is_none() {
            self.dispatch_bloom[row] |= dispatch_bloom_bit(DispatchKey::TagName(folded));
        }
    }

    /// Set the namespace of the row just pushed.
    pub fn set_row_namespace(&mut self, namespace: StyleAtomID) {
        let row = self.namespace.len() - 1;
        self.namespace[row] = namespace;
    }

    pub fn set_row_part_exposure(&mut self, exposure: StyleAtomID) {
        let row = self.part_exposure.len() - 1;
        self.part_exposure[row] = exposure;
    }

    #[must_use]
    pub fn tag_of(&self, row: u32) -> StyleAtomID {
        self.tag[row as usize]
    }

    #[must_use]
    pub fn folded_tag_of(&self, row: u32) -> StyleAtomID {
        self.folded_tag[row as usize]
    }

    #[must_use]
    pub fn id_of(&self, row: u32) -> StyleAtomID {
        self.id[row as usize]
    }

    #[must_use]
    pub fn states_of(&self, row: u32) -> StateSet {
        self.states[row as usize]
    }

    /// Set the values the parameterized state operators read for the row just pushed.
    pub fn set_row_parameters(
        &mut self,
        row: u32,
        directionality: StyleAtomID,
        language: StyleAtomID,
        heading_level: u8,
        custom_states: &[StyleAtomID],
        parts: &[StyleAtomID],
    ) {
        self.directionality[row as usize] = directionality;
        self.language[row as usize] = language;
        self.heading_level[row as usize] = heading_level;
        // Custom states are appended for the last row only, which is the order rows are built in.
        self.custom_state_handles[row as usize] = PayloadHandle::appended_to(&mut self.custom_states, custom_states);
        self.part_handles[row as usize] = PayloadHandle::appended_to(&mut self.parts, parts);
        let mut bloom = self.dispatch_bloom[row as usize];
        if !directionality.is_none() {
            bloom |= dispatch_bloom_bit(DispatchKey::Directionality(directionality));
        }
        if heading_level != 0 {
            bloom |= dispatch_bloom_bit(DispatchKey::Heading);
        }
        for &state in custom_states {
            bloom |= dispatch_bloom_bit(DispatchKey::CustomState(state));
        }
        for &part in parts {
            bloom |= dispatch_bloom_bit(DispatchKey::Part(part));
        }
        self.dispatch_bloom[row as usize] = bloom;
    }

    #[must_use]
    pub fn directionality_of(&self, row: u32) -> StyleAtomID {
        self.directionality[row as usize]
    }

    #[must_use]
    pub fn language_of(&self, row: u32) -> StyleAtomID {
        self.language[row as usize]
    }

    #[must_use]
    pub fn namespace_of(&self, row: u32) -> StyleAtomID {
        self.namespace[row as usize]
    }

    #[must_use]
    pub fn has_text_content_of(&self, row: u32) -> bool {
        self.has_text_content[row as usize]
    }

    /// Set whether the row just pushed holds text or comment content.
    pub fn set_row_has_text_content(&mut self, has_text_content: bool) {
        let row = self.has_text_content.len() - 1;
        self.has_text_content[row] = has_text_content;
    }

    /// The element's resolved language tag, empty when it has none.
    #[must_use]
    pub fn language_tag_of(&self, row: u32) -> &[u16] {
        let (offset, length) = self.language_text[row as usize];
        if length != 0 {
            return &self.text[offset as usize..(offset + length) as usize];
        }
        self.attribute_catalogs
            .language_texts
            .get(self.language_of(row).0 as usize)
            .and_then(Option::as_deref)
            .unwrap_or_default()
    }

    /// Set the language tag of the row just pushed, appending it to the batch's text.
    pub fn set_row_language_tag(&mut self, tag: &[u16]) {
        let offset = u32::try_from(self.text.len()).expect("fact text space exhausted");
        self.text.extend_from_slice(tag);
        let row = self.language_text.len() - 1;
        self.language_text[row] = (offset, u32::try_from(tag.len()).expect("fact text space exhausted"));
    }

    #[must_use]
    pub fn heading_level_of(&self, row: u32) -> u8 {
        if self.primary {
            self.rare_facts.get(row as usize).map_or(0, |facts| facts.heading_level)
        } else {
            self.heading_level[row as usize]
        }
    }

    #[must_use]
    pub fn part_exposure_of(&self, row: u32) -> StyleAtomID {
        if self.primary {
            self.rare_facts
                .get(row as usize)
                .map_or(StyleAtomID::NONE, |facts| facts.part_exposure)
        } else {
            self.part_exposure[row as usize]
        }
    }

    #[must_use]
    pub fn custom_states_of(&self, row: u32) -> &[StyleAtomID] {
        let handle = if self.primary {
            self.rare_facts
                .get(row as usize)
                .map_or(PayloadHandle::default(), |facts| facts.custom_states)
        } else {
            self.custom_state_handles[row as usize]
        };
        handle.slice(&self.custom_states)
    }

    #[must_use]
    pub fn parts_of(&self, row: u32) -> &[StyleAtomID] {
        let handle = if self.primary {
            self.rare_facts
                .get(row as usize)
                .map_or(PayloadHandle::default(), |facts| facts.parts)
        } else {
            self.part_handles[row as usize]
        };
        handle.slice(&self.parts)
    }

    #[must_use]
    pub fn classes_of(&self, row: u32) -> &[StyleAtomID] {
        self.class_handles[row as usize].slice(&self.classes)
    }

    #[must_use]
    pub fn attributes_of(&self, row: u32) -> &[AttributeFact] {
        self.attribute_handles[row as usize].slice(&self.attributes)
    }

    #[must_use]
    #[cfg(test)]
    pub fn attribute_of(&self, row: u32, name: StyleAtomID) -> Option<AttributeFact> {
        self.attributes_of(row)
            .iter()
            .copied()
            .find(|attribute| attribute.name == name)
    }

    /// Visit every dispatch probe this row carries.
    ///
    /// Attribute probes retain the value of the attribute that supplied the name. A selector
    /// dispatch can therefore reject value-constrained entries without allocating an intermediate
    /// name-to-value table. Other probes carry no value.
    pub fn for_each_dispatch_probe(
        &self,
        row: u32,
        is_document_root: bool,
        mut visit: impl FnMut(DispatchKey, Option<StyleAtomID>),
    ) {
        visit(DispatchKey::Universal, None);
        for fact in self.states_of(row).facts() {
            visit(DispatchKey::State(fact), None);
        }
        if is_document_root {
            visit(DispatchKey::Root, None);
        }
        if self.heading_level_of(row) != 0 {
            visit(DispatchKey::Heading, None);
        }
        visit(DispatchKey::TagName(self.tag_of(row)), None);
        let folded_tag = self.folded_tag_of(row);
        if !folded_tag.is_none() {
            visit(DispatchKey::TagName(folded_tag), None);
        }
        let id = self.id_of(row);
        if !id.is_none() {
            visit(DispatchKey::Id(id), None);
        }
        for &class in self.classes_of(row) {
            visit(DispatchKey::Class(class), None);
        }
        let directionality = self.directionality_of(row);
        if !directionality.is_none() {
            visit(DispatchKey::Directionality(directionality), None);
        }
        for &state in self.custom_states_of(row) {
            visit(DispatchKey::CustomState(state), None);
        }
        for &part in self.parts_of(row) {
            visit(DispatchKey::Part(part), None);
        }
        for attribute in self.attributes_of(row) {
            // Every name a selector can reach this attribute by, since a compound dispatches under
            // one of them and which one depends on how the selector wrote it.
            visit(DispatchKey::AttributeName(attribute.name), Some(attribute.value));
            let forms = self.attribute_name_forms(attribute.name);
            let aliases = [forms.local, forms.folded_name, forms.folded_local];
            for (index, &other) in aliases.iter().enumerate() {
                if !other.is_none() && other != attribute.name && !aliases[..index].contains(&other) {
                    visit(DispatchKey::AttributeName(other), Some(attribute.value));
                }
            }
        }
    }

    /// Visit every dispatch key this row carries.
    pub fn for_each_dispatch_key(&self, row: u32, is_document_root: bool, mut visit: impl FnMut(DispatchKey)) {
        self.for_each_dispatch_probe(row, is_document_root, |key, _| visit(key));
    }

    #[must_use]
    fn compute_dispatch_bloom_of(&self, row: u32) -> u64 {
        let mut bloom = 0;
        self.for_each_dispatch_key(row, false, |key| bloom |= dispatch_bloom_bit(key));
        bloom
    }

    #[must_use]
    pub fn dispatch_bloom_of(&self, row: u32, is_document_root: bool) -> u64 {
        self.dispatch_bloom[row as usize]
            | if is_document_root {
                dispatch_bloom_bit(DispatchKey::Root)
            } else {
                0
            }
    }

    #[must_use]
    pub fn carries_dispatch_key(&self, row: u32, key: DispatchKey, is_root: bool) -> bool {
        match key {
            DispatchKey::Part(part) => self.parts_of(row).contains(&part),
            DispatchKey::CustomState(state) => self.custom_states_of(row).contains(&state),
            DispatchKey::Id(id) => self.id_of(row) == id,
            DispatchKey::Class(class) => self.classes_of(row).contains(&class),
            DispatchKey::AttributeName(name) => self.attributes_of(row).iter().any(|attribute| {
                if attribute.name == name {
                    return true;
                }
                let forms = self.attribute_name_forms(attribute.name);
                forms.local == name || forms.folded_name == name || forms.folded_local == name
            }),
            DispatchKey::TagName(tag) => self.tag_of(row) == tag || self.folded_tag_of(row) == tag,
            DispatchKey::Directionality(direction) => self.directionality_of(row) == direction,
            DispatchKey::Root => is_root,
            DispatchKey::State(state) => self.states_of(row).contains(state),
            DispatchKey::Heading => self.heading_level_of(row) != 0,
            DispatchKey::Universal => true,
            _ => unreachable!("non-dispatch feature key"),
        }
    }

    #[must_use]
    pub fn text_of(&self, attribute: AttributeFact) -> Option<&[u16]> {
        if let Some(text) = self.local_text_of(attribute) {
            return Some(text);
        }
        self.attribute_catalogs
            .value_texts
            .get(attribute.value.0 as usize)
            .and_then(Option::as_deref)
    }

    fn local_text_of(&self, attribute: AttributeFact) -> Option<&[u16]> {
        if attribute.text_length == 0 && attribute.text_offset == u32::MAX {
            return None;
        }
        let start = attribute.text_offset as usize;
        let end = start + attribute.text_length as usize;
        self.text.get(start..end)
    }

    #[must_use]
    pub fn attribute_name_forms(&self, name: StyleAtomID) -> AttributeNameForms {
        let mut forms = self
            .attribute_catalogs
            .name_forms
            .get(name.0 as usize)
            .unwrap_or_default();
        if forms.folded_name.is_none() {
            forms.folded_name = name;
        }
        if forms.folded_local.is_none() {
            forms.folded_local = forms.local;
        }
        forms
    }

    #[cfg(test)]
    pub fn note_attribute_name_forms(&mut self, name: StyleAtomID, forms: AttributeNameForms) {
        Rc::make_mut(&mut self.attribute_catalogs)
            .name_forms
            .insert(name.0 as usize, forms);
    }

    /// Logical bytes occupied by one packed row, excluding the dense directory shared by all rows.
    /// This measures both fixed columns and variable payload so a large stale row can trigger
    /// compaction without waiting for the same fraction of small rows to become stale.
    fn logical_bytes_of_row(&self, row: u32) -> u64 {
        let fixed = size_of::<StyleNodeID>()
            + 7 * size_of::<StyleAtomID>()
            + size_of::<StateSet>()
            + size_of::<bool>()
            + size_of::<(u32, u32)>()
            + size_of::<u8>()
            + 4 * size_of::<u32>();
        (fixed
            + size_of_val(self.custom_states_of(row))
            + size_of_val(self.parts_of(row))
            + size_of_val(self.classes_of(row))
            + size_of_val(self.attributes_of(row))
            + self
                .attributes_of(row)
                .iter()
                .map(|attribute| attribute.text_length as usize * size_of::<u16>())
                .sum::<usize>()) as u64
    }

    fn payload_bytes_of_row(&self, row: u32) -> u64 {
        (size_of_val(self.custom_states_of(row))
            + size_of_val(self.parts_of(row))
            + size_of_val(self.classes_of(row))
            + size_of_val(self.attributes_of(row))) as u64
    }

    fn compact_primary_payloads(&mut self) {
        assert!(self.primary);
        let mut custom_states = Vec::new();
        let mut parts = Vec::new();
        let mut classes = Vec::new();
        let mut attributes = Vec::new();
        for row in 1..self.row_count() {
            if !self.resident.contains(row) {
                continue;
            }
            if let Some(mut rare_facts) = self.rare_facts.get(row) {
                rare_facts.custom_states =
                    PayloadHandle::appended_to(&mut custom_states, rare_facts.custom_states.slice(&self.custom_states));
                rare_facts.parts = PayloadHandle::appended_to(&mut parts, rare_facts.parts.slice(&self.parts));
                self.rare_facts.insert(row, rare_facts);
            }
            self.class_handles[row] =
                PayloadHandle::appended_to(&mut classes, self.class_handles[row].slice(&self.classes));
            self.attribute_handles[row] =
                PayloadHandle::appended_to(&mut attributes, self.attribute_handles[row].slice(&self.attributes));
        }
        self.custom_states = custom_states;
        self.parts = parts;
        self.classes = classes;
        self.attributes = attributes;
        self.text.clear();
        self.text.shrink_to_fit();
    }

    pub fn clear(&mut self) {
        self.generation = next_batch_generation();
        // Sparse batches can name high-index elements, so clearing their individual slots avoids
        // writing the holes below them. Once enough of the directory is live, one contiguous fill
        // is cheaper than scattered stores.
        if self.nodes.len() < self.row_by_element_index.len() / 4 {
            for &node in &self.nodes {
                let Some(index) = node.element_index() else {
                    continue;
                };
                self.row_by_element_index[index as usize] = NO_ROW;
            }
        } else {
            self.row_by_element_index.fill(NO_ROW);
        }
        self.nodes.clear();
        self.stale_rows = 0;
        self.live_rows = 0;
        self.dispatch_bloom.clear();
        self.tag.clear();
        self.folded_tag.clear();
        self.id.clear();
        self.states.clear();
        self.directionality.clear();
        self.language.clear();
        self.has_text_content.clear();
        self.language_text.clear();
        self.namespace.clear();
        self.heading_level.clear();
        self.custom_states.clear();
        self.parts.clear();
        self.part_handles.clear();
        self.part_exposure.clear();
        self.custom_state_handles.clear();
        self.classes.clear();
        self.class_handles.clear();
        self.attributes.clear();
        self.attribute_handles.clear();
        self.text.clear();
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.resident,
                self.nodes,
                self.row_by_element_index,
                self.dispatch_bloom,
                self.tag,
                self.folded_tag,
                self.id,
                self.states,
                self.directionality,
                self.heading_level,
                self.language,
                self.language_text,
                self.has_text_content,
                self.namespace,
                self.custom_state_handles,
                self.custom_states,
                self.parts,
                self.part_handles,
                self.part_exposure,
                self.class_handles,
                self.classes,
                self.attribute_handles,
                self.attributes,
                self.text,
            ];
            cached [self.rare_facts.capacity_bytes()];
            nested [];
            skip [self.primary, self.stale_rows, self.live_rows, self.generation, self.attribute_catalogs];
        }
    }
}

/// Members per posting chunk. Insertion and removal shift at most this many identities, so a
/// posting stays cheap to maintain without giving up sorted order.
const MAX_POSTING_CHUNK: usize = 256;

/// One feature's candidate set: chunked and sorted by `StyleNodeID`.
#[derive(Default)]
pub(super) struct Posting {
    chunks: Vec<Vec<StyleNodeID>>,
    length: usize,
}

impl Posting {
    pub(super) fn candidates(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.chunks.iter().flat_map(|chunk| chunk.iter().copied())
    }

    pub(super) fn append_candidates_to(&self, candidates: &mut Vec<StyleNodeID>) {
        candidates.reserve(self.length);
        for chunk in &self.chunks {
            candidates.extend_from_slice(chunk);
        }
    }

    #[must_use]
    pub(super) fn len(&self) -> usize {
        self.length
    }

    fn chunk_for(&self, node: StyleNodeID) -> usize {
        match self.chunks.binary_search_by(|chunk| chunk[0].cmp(&node)) {
            Ok(index) => index,
            Err(0) => 0,
            Err(index) => index - 1,
        }
    }

    fn insert(&mut self, node: StyleNodeID) -> Option<u64> {
        if self.chunks.is_empty() {
            let chunks_capacity_before = self.chunks.capacity();
            let chunk = vec![node];
            let chunk_capacity = chunk.capacity();
            self.chunks.push(chunk);
            self.length = 1;
            return Some(
                ((self.chunks.capacity() - chunks_capacity_before) * size_of::<Vec<StyleNodeID>>()
                    + chunk_capacity * size_of::<StyleNodeID>()) as u64,
            );
        }
        if node > *self.chunks.last().unwrap().last().unwrap() {
            let last = self.chunks.len() - 1;
            let added_bytes = if self.chunks[last].len() < MAX_POSTING_CHUNK {
                let capacity_before = self.chunks[last].capacity();
                self.chunks[last].push(node);
                ((self.chunks[last].capacity() - capacity_before) * size_of::<StyleNodeID>()) as u64
            } else {
                let chunks_capacity_before = self.chunks.capacity();
                let chunk = vec![node];
                let chunk_capacity = chunk.capacity();
                self.chunks.push(chunk);
                ((self.chunks.capacity() - chunks_capacity_before) * size_of::<Vec<StyleNodeID>>()
                    + chunk_capacity * size_of::<StyleNodeID>()) as u64
            };
            self.length += 1;
            return Some(added_bytes);
        }
        let index = self.chunk_for(node);
        let chunk_capacity_before = self.chunks[index].capacity();
        let moved = {
            let chunk = &mut self.chunks[index];
            let Err(position) = chunk.binary_search(&node) else {
                return None;
            };
            if chunk.len() == MAX_POSTING_CHUNK {
                let middle = chunk.len() / 2;
                let mut moved = Vec::with_capacity(chunk.len() - middle + 1);
                moved.extend_from_slice(&chunk[middle..]);
                chunk.truncate(middle);
                if position < middle {
                    chunk.insert(position, node);
                } else {
                    moved.insert(position - middle, node);
                }
                Some(moved)
            } else {
                chunk.insert(position, node);
                None
            }
        };
        let mut added_bytes =
            ((self.chunks[index].capacity() - chunk_capacity_before) * size_of::<StyleNodeID>()) as u64;
        self.length += 1;
        if let Some(moved) = moved {
            let chunks_capacity_before = self.chunks.capacity();
            let moved_capacity = moved.capacity();
            self.chunks.insert(index + 1, moved);
            added_bytes += ((self.chunks.capacity() - chunks_capacity_before) * size_of::<Vec<StyleNodeID>>()
                + moved_capacity * size_of::<StyleNodeID>()) as u64;
        }
        Some(added_bytes)
    }

    fn remove(&mut self, node: StyleNodeID) -> Option<u64> {
        if self.chunks.is_empty() {
            return None;
        }
        let index = self.chunk_for(node);
        let chunk = &mut self.chunks[index];
        let Ok(position) = chunk.binary_search(&node) else {
            return None;
        };
        chunk.remove(position);
        self.length -= 1;
        let released_bytes = if chunk.is_empty() {
            let chunk = self.chunks.remove(index);
            (chunk.capacity() * size_of::<StyleNodeID>()) as u64
        } else {
            0
        };
        Some(released_bytes)
    }

    pub(super) fn contains(&self, node: StyleNodeID) -> bool {
        if self.chunks.is_empty() {
            return false;
        }
        self.chunks[self.chunk_for(node)].binary_search(&node).is_ok()
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.chunks];
            cached [];
            nested [self
                .chunks
                .iter()
                .map(|chunk| chunk.capacity() * size_of::<StyleNodeID>())
                .sum::<usize>()];
            skip [self.length];
        }
    }
}

/// One compact key for selector routing, dispatch, and postings.
///
/// Every atom-bearing variant has the same `(kind: u8, atom: u32)` representation. Variants without
/// an atom retain the distinctions needed by routing and dispatch without introducing another key
/// vocabulary or a conversion between equal semantic features.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u8)]
pub enum FeatureKey {
    Part(StyleAtomID),
    CustomState(StyleAtomID),
    TagName(StyleAtomID),
    Id(StyleAtomID),
    Class(StyleAtomID),
    AttributeName(StyleAtomID),
    /// The complete interned value of an attribute. This is query acceleration rather than a CSS
    /// dispatch key: attribute value queries use it to reach only values that can match.
    AttributeValue(StyleAtomID),
    Directionality(StyleAtomID),
    Root,
    State(StateFact),
    Heading,
    Universal,
    Structural,
    Language,
    /// An element whose style resolution called a custom function.
    ///
    /// Which function it called is not reported by the substitution machinery, so an `@function` rule
    /// changing reaches the elements that called one at all - bounded by that, rather than by the
    /// document.
    AnyCustomFunction,
    /// An element whose style resolution consulted custom properties whose names are not all known.
    ///
    /// The bulk cascade substitutes `var()` inside the Rust value path, which does not report the
    /// names it looked up, so an element resolved that way can only be indexed as using something.
    /// A registration reaches these as well as the ones indexed by name - far narrower than the
    /// document, and sound where naming only what is known would not be.
    AnyCustomProperty,
    /// One custom property an element declares or references.
    ///
    /// Registering a property changes how every element that declares or references it computes, and
    /// what it inherits. Which elements those are is not something selector matching can say.
    CustomPropertySet(u32),
    /// One `animation-name` an element's computed style references.
    ///
    /// Unlike the others this is not a fact a selector matches on: it is how a `@keyframes` rule
    /// finds the elements it decides for. A keyframes rule reaches animations referencing its name
    /// rather than anything its position in the cascade implies, and without this the elements
    /// running an animation whose keyframes changed are unreachable.
    AnimationName(StyleAtomID),
}

impl FeatureKey {
    #[must_use]
    pub fn has_selector_posting(self) -> bool {
        matches!(
            self,
            Self::Part(_)
                | Self::CustomState(_)
                | Self::TagName(_)
                | Self::Id(_)
                | Self::Class(_)
                | Self::AttributeName(_)
                | Self::AttributeValue(_)
                | Self::Directionality(_)
        )
    }

    fn atom(self) -> Option<StyleAtomID> {
        match self {
            Self::Part(atom)
            | Self::CustomState(atom)
            | Self::TagName(atom)
            | Self::Id(atom)
            | Self::Class(atom)
            | Self::AttributeName(atom)
            | Self::AttributeValue(atom)
            | Self::Directionality(atom)
            | Self::AnimationName(atom) => Some(atom),
            Self::Root
            | Self::State(_)
            | Self::Heading
            | Self::Universal
            | Self::Structural
            | Self::Language
            | Self::AnyCustomFunction
            | Self::AnyCustomProperty
            | Self::CustomPropertySet(_) => None,
        }
    }
}

pub type SelectorPostingKey = FeatureKey;
pub type DependencyPostingKey = FeatureKey;
pub type PostingKey = FeatureKey;

/// Candidate sets for observed element features.
///
/// This is acceleration and nothing more. Evicting a posting never changes which transpose entry
/// points run and never changes an answer: the routed program reads authoritative DOM facts or runs
/// an exact scope batch instead. Fact setters populate postings eagerly, independent of which
/// selector programs are attached, and they remain until their facts disappear or eviction removes
/// them. This global arrangement avoids program-lifetime registration and first-use fact scans;
/// per-key missing coverage confines eviction fallout to the acceleration that was actually
/// removed.
pub struct FeaturePostings {
    postings: HashMap<PostingKey, Posting>,
    residency: MemoryLease,
    missing: HashSet<PostingKey>,
    cardinality_limited: HashSet<PostingKey>,
    grown_selector_postings: HashSet<PostingKey>,
    selector_posting_limit: usize,
    benefit_hits: Cell<u64>,
    benefit_misses: Cell<u64>,
}

impl Default for FeaturePostings {
    fn default() -> Self {
        Self {
            postings: HashMap::default(),
            residency: MemoryLease::new(MemoryCategory::FeaturePosting),
            missing: HashSet::default(),
            cardinality_limited: HashSet::default(),
            grown_selector_postings: HashSet::default(),
            selector_posting_limit: usize::MAX,
            benefit_hits: Cell::new(0),
            benefit_misses: Cell::new(0),
        }
    }
}

impl FeaturePostings {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Add `node` to a feature's candidate set. Once quota pressure closes admission, existing
    /// postings continue to accept members so they remain exact, while new posting keys stay
    /// missing until the next quota period.
    pub fn insert(&mut self, key: PostingKey, node: StyleNodeID, memory: &mut MemoryController) -> bool {
        if self.missing.contains(&key) || self.cardinality_limited.contains(&key) {
            return false;
        }
        if !memory.is_tier3_admitting(MemoryCategory::FeaturePosting) && !self.postings.contains_key(&key) {
            self.remember_missing(key);
            return false;
        }
        let postings_capacity_before = self.postings_capacity_bytes();
        let posting = self.postings.entry(key).or_default();
        let Some(posting_growth) = posting.insert(node) else {
            return true;
        };
        let posting_length = posting.length;
        let postings_growth = self.postings_capacity_bytes() - postings_capacity_before;
        let growth = posting_growth + postings_growth;
        if growth != 0 {
            self.residency
                .reconcile_committed(memory, self.residency.bytes() + growth);
            memory.finish_committed_acceleration_growth(MemoryCategory::FeaturePosting);
        }
        if key.has_selector_posting() && posting_length > 4096 {
            self.remember_grown_selector_posting(key);
        }
        let exceeds_limit = key.has_selector_posting() && posting_length > self.selector_posting_limit;
        if exceeds_limit {
            self.remove_posting(key);
            self.remember_cardinality_limited(key);
            return false;
        }
        true
    }

    pub fn remove(&mut self, key: PostingKey, node: StyleNodeID) {
        if let Some(posting) = self.postings.get_mut(&key) {
            if let Some(released) = posting.remove(node) {
                self.residency.shrink_to(self.residency.bytes() - released);
            }
            if posting.length == 0 {
                self.remove_posting(key);
            }
        }
    }

    /// Look up one feature's complete candidate set.
    ///
    /// An absent key is known empty unless that exact posting was evicted or refused.
    #[must_use]
    pub(super) fn lookup(&self, key: PostingKey) -> Lookup<&Posting, PostingKey> {
        let result = match self.postings.get(&key) {
            Some(posting) => Lookup::Known(posting),
            None if self.missing.contains(&key) || self.cardinality_limited.contains(&key) => Lookup::Missing(key),
            None => Lookup::KnownAbsent,
        };
        self.record_benefit_lookup(!matches!(result, Lookup::Missing(_)));
        result
    }

    #[cfg(test)]
    pub(crate) fn evict(&mut self, key: PostingKey) {
        self.remember_missing(key);
        self.remove_posting(key);
    }

    fn missing_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.missing];
            cached [];
            nested [];
            skip [];
        }
    }

    fn cardinality_limited_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.cardinality_limited];
            cached [];
            nested [];
            skip [];
        }
    }

    fn postings_capacity_bytes(&self) -> u64 {
        self.postings.shallow_capacity_bytes()
    }

    fn grown_selector_postings_capacity_bytes(&self) -> u64 {
        self.grown_selector_postings.shallow_capacity_bytes()
    }

    fn remember_missing(&mut self, key: PostingKey) {
        let before = self.missing_capacity_bytes();
        self.missing.insert(key);
        let after = self.missing_capacity_bytes();
        self.residency.grow_committed(after - before);
    }

    fn remove_posting(&mut self, key: PostingKey) {
        let Some(posting) = self.postings.remove(&key) else {
            return;
        };
        self.residency
            .shrink_to(self.residency.bytes() - posting.capacity_bytes());
        if self.postings.is_empty() {
            let released = self.postings_capacity_bytes();
            self.postings = HashMap::default();
            self.residency.shrink_to(self.residency.bytes() - released);
        }
    }

    fn remember_cardinality_limited(&mut self, key: PostingKey) {
        let before = self.cardinality_limited_capacity_bytes();
        self.cardinality_limited.insert(key);
        let after = self.cardinality_limited_capacity_bytes();
        self.residency.grow_committed(after - before);
    }

    fn remember_grown_selector_posting(&mut self, key: PostingKey) {
        let before = self.grown_selector_postings_capacity_bytes();
        self.grown_selector_postings.insert(key);
        let after = self.grown_selector_postings_capacity_bytes();
        self.residency.grow_committed(after - before);
    }

    fn set_selector_posting_limit(&mut self, maximum_candidates: usize) {
        let first_limit = self.selector_posting_limit == usize::MAX;
        self.selector_posting_limit = maximum_candidates;
        if !first_limit {
            return;
        }
        let keys: Vec<_> = self
            .postings
            .iter()
            .filter_map(|(&key, posting)| {
                (key.has_selector_posting() && posting.length > maximum_candidates).then_some(key)
            })
            .collect();
        for key in keys {
            self.remove_posting(key);
            self.remember_cardinality_limited(key);
        }
    }

    fn update_selector_posting_limit(&mut self, live_element_count: usize) {
        self.set_selector_posting_limit((live_element_count / 4).max(4096));
        let mut grown = std::mem::take(&mut self.grown_selector_postings);
        for key in grown.drain() {
            if self
                .postings
                .get(&key)
                .is_some_and(|posting| posting.length > self.selector_posting_limit)
            {
                self.remove_posting(key);
                self.remember_cardinality_limited(key);
            }
        }
        self.grown_selector_postings = grown;
    }

    /// Discard every posting. Memory pressure reduces retained acceleration, never correctness.
    pub fn evict_all(&mut self) {
        let missing_before = self.missing_capacity_bytes();
        self.missing.extend(self.postings.keys().copied());
        self.residency
            .grow_committed(self.missing_capacity_bytes() - missing_before);
        let released =
            self.postings.values().map(Posting::capacity_bytes).sum::<u64>() + self.postings_capacity_bytes();
        self.residency.shrink_to(self.residency.bytes() - released);
        self.postings = HashMap::default();
    }

    #[must_use]
    fn is_incomplete(&self) -> bool {
        !self.missing.is_empty()
    }

    fn take_rebuilt(&mut self, mut rebuilt: FeaturePostings) {
        debug_assert!(rebuilt.missing.is_empty());
        let rebuilt_grown_bytes = rebuilt.grown_selector_postings_capacity_bytes();
        rebuilt.grown_selector_postings = HashSet::default();
        rebuilt.residency.shrink_committed(rebuilt_grown_bytes);

        let postings_before = self.postings_capacity_bytes();
        self.postings.extend(rebuilt.postings.drain());
        let postings_growth = self.postings_capacity_bytes() - postings_before;
        let rebuilt_postings_bytes = rebuilt.postings_capacity_bytes();
        rebuilt.postings = HashMap::default();
        rebuilt.residency.shrink_committed(rebuilt_postings_bytes);

        let cardinality_before = self.cardinality_limited_capacity_bytes();
        self.cardinality_limited.extend(rebuilt.cardinality_limited.drain());
        let cardinality_growth = self.cardinality_limited_capacity_bytes() - cardinality_before;
        let rebuilt_cardinality_bytes = rebuilt.cardinality_limited_capacity_bytes();
        rebuilt.cardinality_limited = HashSet::default();
        rebuilt.residency.shrink_committed(rebuilt_cardinality_bytes);

        let rebuilt_bytes = rebuilt.residency.bytes();
        rebuilt.residency.release();
        self.residency
            .grow_committed(rebuilt_bytes + postings_growth + cardinality_growth);

        let released = self.missing_capacity_bytes();
        self.missing = HashSet::default();
        self.residency.shrink_to(self.residency.bytes() - released);
    }

    #[must_use]
    #[cfg(test)]
    pub fn feature_count(&self) -> usize {
        self.postings.len()
    }

    #[cfg(test)]
    fn retained_capacity_bytes(&self) -> u64 {
        self.postings_capacity_bytes()
            + self.postings.values().map(Posting::capacity_bytes).sum::<u64>()
            + self.missing_capacity_bytes()
            + self.cardinality_limited_capacity_bytes()
            + self.grown_selector_postings_capacity_bytes()
    }

    fn record_benefit_lookup(&self, hit: bool) {
        let observations = self.benefit_hits.get() + self.benefit_misses.get();
        if observations >= 4096 {
            self.benefit_hits.set(self.benefit_hits.get() / 2);
            self.benefit_misses.set(self.benefit_misses.get() / 2);
        }
        match hit {
            true => self.benefit_hits.set(self.benefit_hits.get() + 1),
            false => self.benefit_misses.set(self.benefit_misses.get() + 1),
        }
    }

    pub(super) fn take_benefit_lookups(&self) -> (u64, u64) {
        (self.benefit_hits.replace(0), self.benefit_misses.replace(0))
    }

    fn forget_atoms(&mut self, atoms: &HashSet<StyleAtomID>) {
        let mut released = 0;
        self.postings.retain(|key, posting| {
            let keep = !key.atom().is_some_and(|atom| atoms.contains(&atom));
            if !keep {
                released += posting.capacity_bytes();
            }
            keep
        });
        if self.postings.is_empty() {
            released += self.postings_capacity_bytes();
            self.postings = HashMap::default();
        }
        self.residency.shrink_to(self.residency.bytes() - released);
        self.missing
            .retain(|key| !key.atom().is_some_and(|atom| atoms.contains(&atom)));
        self.cardinality_limited
            .retain(|key| !key.atom().is_some_and(|atom| atoms.contains(&atom)));
        self.grown_selector_postings
            .retain(|key| !key.atom().is_some_and(|atom| atoms.contains(&atom)));
    }
}

/// The rightmost distinguishing feature of one selector entry.
pub type DispatchKey = FeatureKey;

/// One lossy bit for a dispatch key.
///
/// A Bloom summary is used only to reject a selector when a necessary key is definitely absent.
/// Collisions admit extra exact evaluation and can never change a selector answer.
#[must_use]
pub fn dispatch_bloom_bit(key: DispatchKey) -> u64 {
    1_u64 << (dispatch_key_hash(key) & 63)
}

/// One well-mixed hash of a dispatch key, which both Bloom summaries take their bit positions from.
#[must_use]
fn dispatch_key_hash(key: DispatchKey) -> u64 {
    let (kind, value) = match key {
        DispatchKey::Part(atom) => (1_u64, u64::from(atom.0)),
        DispatchKey::CustomState(atom) => (2, u64::from(atom.0)),
        DispatchKey::Id(atom) => (3, u64::from(atom.0)),
        DispatchKey::Class(atom) => (4, u64::from(atom.0)),
        DispatchKey::AttributeName(atom) => (5, u64::from(atom.0)),
        DispatchKey::TagName(atom) => (6, u64::from(atom.0)),
        DispatchKey::Directionality(atom) => (7, u64::from(atom.0)),
        DispatchKey::Root => (8, 0),
        DispatchKey::State(fact) => (9, fact as u64),
        DispatchKey::Heading => (10, 0),
        DispatchKey::Universal => (11, 0),
        _ => unreachable!("non-dispatch feature key"),
    };
    let mut hash = value ^ kind.wrapping_mul(0x9e37_79b9_7f4a_7c15);
    hash = (hash ^ (hash >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    hash = (hash ^ (hash >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    hash ^ (hash >> 31)
}

/// One attached selector entry, reachable from its dispatch key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DispatchEntry {
    /// Document identity of the compiled selector entry this scope-local rule row joins.
    pub identity: EntryID,
    pub rule: RuleID,
    pub program: SelectorProgramID,
    /// Index into the selector program's entry list.
    pub entry: u32,
    /// This entry's normal-declaration cascade rank within the dispatch. A rule outside `@scope`
    /// can copy it into its match, so every element need not reconstruct the same priority.
    pub cascade_order: u32,
    /// For an entry in an attribute bucket, the value its subject requires by identity, or none.
    /// An attribute that is on every element puts its whole bucket in front of every candidate, and
    /// comparing two atoms rejects most of them without evaluating a selector.
    pub required_attribute_value: StyleAtomID,
    /// A required feature of the subject's parent, when a child combinator names one.
    pub required_parent: Option<DispatchKey>,
    /// A required feature of some ancestor, when a descendant combinator names one.
    pub required_ancestor: Option<DispatchKey>,
    /// The exact bit assigned to `required_ancestor` in this dispatch.
    pub required_ancestor_index: Option<u32>,
    /// Bloom summary of independently necessary subject features.
    pub required_subject_bloom: u64,
    /// Whether the top-down prefix program answers this entry exactly.
    pub prefix_matched: bool,
    /// Whether this entry is inserted under several keys - one per branch of its subject's
    /// disjunction - so a candidate walk carrying more than one of those keys must evaluate it
    /// once, not once per key. Copies of one entry share a cascade_order, which is the dedup key.
    pub multi_key: bool,
}

/// Selector-derived half of a dispatch row, shared by scopes with the same topology.
#[derive(Clone, Copy)]
struct DispatchEntryMetadata {
    identity: EntryID,
    program: SelectorProgramID,
    entry: u32,
    required_attribute_value: StyleAtomID,
    required_parent: Option<DispatchKey>,
    required_ancestor: Option<DispatchKey>,
    required_ancestor_index: Option<u32>,
    required_subject_bloom: u64,
    prefix_matched: bool,
    multi_key: bool,
}

#[derive(Clone, Copy)]
struct DispatchEntryBinding {
    rule: RuleID,
    cascade_order_index: u32,
}

struct RuleDispatchEntries {
    rows: Vec<DispatchEntryMetadata>,
    residency: MemoryLease,
}

impl Clone for RuleDispatchEntries {
    fn clone(&self) -> Self {
        Self {
            rows: self.rows.clone(),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

impl Default for RuleDispatchEntries {
    fn default() -> Self {
        Self {
            rows: Vec::new(),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

impl RuleDispatchEntries {
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.rows];
            cached [];
            nested [];
            skip [self.residency];
        }
    }
}

define_id! {
    /// Physical row in one scope-local selector dispatch.
    pub(super) struct DispatchRow();
}

impl DispatchRow {
    pub(super) fn from_index(index: usize) -> Self {
        Self(u32::try_from(index).expect("dispatch entry space exhausted"))
    }

    pub(super) fn index(self) -> usize {
        self.0 as usize
    }
}

/// Direct cascade projection metadata indexed by an entry's dense cascade order.
#[derive(Clone, Copy, Default)]
struct CascadeEntryData {
    property_start: u32,
    property_count: u16,
    prunable: bool,
    pruning_blocker: bool,
}

/// Transaction-local duplicate suppression for candidate dispatch.
#[derive(Default)]
pub struct DispatchCandidateWorkspace {
    seen_at_epoch: EpochColumn,
    candidates: Vec<DispatchRow>,
    cascade_sort: Vec<(Reverse<(bool, u32)>, DispatchRow)>,
    epoch: u32,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CandidateEntries {
    All,
    NonPrefix,
}

impl DispatchCandidateWorkspace {
    pub(super) fn with_entry_capacity(entry_count: usize) -> Self {
        Self {
            seen_at_epoch: {
                let mut column = EpochColumn::default();
                column.ensure_len(entry_count);
                column
            },
            candidates: Vec::with_capacity(entry_count),
            cascade_sort: Vec::with_capacity(entry_count),
            epoch: 0,
        }
    }

    fn begin(&mut self, entry_count: usize) {
        self.seen_at_epoch.ensure_len(entry_count);
        self.candidates.clear();
        self.cascade_sort.clear();
        advance_epoch(&mut self.epoch, 1, &mut [&mut self.seen_at_epoch]);
    }

    fn admit(&mut self, id: DispatchRow) -> bool {
        self.seen_at_epoch.mark(id.index(), self.epoch)
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.seen_at_epoch, self.candidates, self.cascade_sort];
            cached [];
            nested [];
            skip [self.epoch];
        }
    }
}

/// What exact parent facts candidate dispatch can consult.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParentDispatchFacts {
    /// The node has no element parent. A child combinator cannot match it.
    NoElementParent,
    /// The parent is an element whose complete local facts are in this batch.
    Known { row: u32, is_document_root: bool },
    /// The parent is an element, but this batch cannot answer what it carries.
    Unknown,
}

/// Exact ancestry facts candidate dispatch can consult.
#[derive(Clone, Copy)]
pub struct AncestorDispatchFacts<'a> {
    words: &'a [u64],
    unbounded: bool,
}

impl<'a> AncestorDispatchFacts<'a> {
    #[must_use]
    pub fn new(words: &'a [u64], unbounded: bool) -> Self {
        Self { words, unbounded }
    }

    #[must_use]
    fn contains(self, index: u32) -> bool {
        if self.unbounded {
            return true;
        }
        let index = index as usize;
        self.words[index / u64::BITS as usize] & (1_u64 << (index % u64::BITS as usize)) != 0
    }
}

#[derive(Clone, Copy, Default)]
struct DispatchBucketRange {
    start: u32,
    length: u32,
}

const DISPATCH_ATOM_KIND_COUNT: usize = 7;

#[derive(Clone, Default)]
struct SegmentedDispatchBucketDirectory {
    ranges: PagedCopyColumn<DispatchBucketRange>,
}

impl SegmentedDispatchBucketDirectory {
    fn get(&self, index: usize) -> DispatchBucketRange {
        self.ranges.get(index).unwrap_or_default()
    }

    fn insert(&mut self, index: usize, range: DispatchBucketRange) {
        self.ranges.insert(index, range);
    }

    fn capacity_bytes(&self) -> u64 {
        self.ranges.shallow_capacity_bytes()
    }
}

#[derive(Clone, Default)]
struct DispatchBucketDirectory {
    atoms: [SegmentedDispatchBucketDirectory; DISPATCH_ATOM_KIND_COUNT],
    states: Vec<DispatchBucketRange>,
    fixed: [DispatchBucketRange; 3],
    rows: Vec<DispatchRow>,
}

impl DispatchBucketDirectory {
    fn build(buckets: HashMap<DispatchKey, Vec<DispatchRow>>) -> Self {
        let mut entries: Vec<_> = buckets.into_iter().collect();
        entries.sort_unstable_by_key(|&(key, _)| key);
        let row_count = entries.iter().map(|(_, rows)| rows.len()).sum();
        let mut directory = Self {
            states: vec![DispatchBucketRange::default(); StateFact::ALL.len()],
            rows: Vec::with_capacity(row_count),
            ..Self::default()
        };
        for (key, rows) in entries {
            let range = DispatchBucketRange {
                start: u32::try_from(directory.rows.len()).expect("dispatch bucket space exhausted"),
                length: u32::try_from(rows.len()).expect("dispatch bucket space exhausted"),
            };
            directory.insert_range(key, range);
            directory.rows.extend(rows);
        }
        directory
    }

    fn get(&self, key: DispatchKey) -> &[DispatchRow] {
        let range = self.range(key);
        let start = range.start as usize;
        &self.rows[start..start + range.length as usize]
    }

    fn range(&self, key: DispatchKey) -> DispatchBucketRange {
        if let Some((kind, atom)) = dispatch_atom_bucket(key) {
            return self.atoms[kind].get(atom);
        }
        match key {
            DispatchKey::State(state) => self.states.get(state as usize).copied().unwrap_or_default(),
            DispatchKey::Root => self.fixed[0],
            DispatchKey::Heading => self.fixed[1],
            DispatchKey::Universal => self.fixed[2],
            _ => unreachable!("non-dispatch feature key"),
        }
    }

    fn insert_range(&mut self, key: DispatchKey, range: DispatchBucketRange) {
        if let Some((kind, atom)) = dispatch_atom_bucket(key) {
            self.atoms[kind].insert(atom, range);
            return;
        }
        match key {
            DispatchKey::State(state) => self.states[state as usize] = range,
            DispatchKey::Root => self.fixed[0] = range,
            DispatchKey::Heading => self.fixed[1] = range,
            DispatchKey::Universal => self.fixed[2] = range,
            _ => unreachable!("non-dispatch feature key"),
        }
    }

    fn to_buckets(&self) -> HashMap<DispatchKey, Vec<DispatchRow>> {
        let mut buckets = HashMap::default();
        for (kind, directory) in self.atoms.iter().enumerate() {
            for (atom, range) in directory.ranges.indexed_iter() {
                if range.length != 0 {
                    buckets.insert(dispatch_atom_bucket_key(kind, atom), self.slice(range).to_vec());
                }
            }
        }
        for (index, &range) in self.states.iter().enumerate() {
            if range.length != 0 {
                buckets.insert(DispatchKey::State(StateFact::ALL[index]), self.slice(range).to_vec());
            }
        }
        for (key, range) in [
            (DispatchKey::Root, self.fixed[0]),
            (DispatchKey::Heading, self.fixed[1]),
            (DispatchKey::Universal, self.fixed[2]),
        ] {
            if range.length != 0 {
                buckets.insert(key, self.slice(range).to_vec());
            }
        }
        buckets
    }

    fn filtered(&self, mut retain: impl FnMut(DispatchRow) -> bool) -> Self {
        let mut buckets = self.to_buckets();
        buckets.retain(|_, rows| {
            rows.retain(|&row| retain(row));
            !rows.is_empty()
        });
        Self::build(buckets)
    }

    fn slice(&self, range: DispatchBucketRange) -> &[DispatchRow] {
        let start = range.start as usize;
        &self.rows[start..start + range.length as usize]
    }

    fn capacity_bytes(&self) -> u64 {
        self.atoms
            .iter()
            .map(SegmentedDispatchBucketDirectory::capacity_bytes)
            .sum::<u64>()
            + (self.states.capacity() * size_of::<DispatchBucketRange>()) as u64
            + (self.rows.capacity() * size_of::<DispatchRow>()) as u64
    }
}

fn dispatch_atom_bucket(key: DispatchKey) -> Option<(usize, usize)> {
    match key {
        DispatchKey::Part(atom) => Some((0, atom.0 as usize)),
        DispatchKey::CustomState(atom) => Some((1, atom.0 as usize)),
        DispatchKey::TagName(atom) => Some((2, atom.0 as usize)),
        DispatchKey::Id(atom) => Some((3, atom.0 as usize)),
        DispatchKey::Class(atom) => Some((4, atom.0 as usize)),
        DispatchKey::AttributeName(atom) => Some((5, atom.0 as usize)),
        DispatchKey::Directionality(atom) => Some((6, atom.0 as usize)),
        _ => None,
    }
}

fn dispatch_atom_bucket_key(kind: usize, atom: usize) -> DispatchKey {
    let atom = StyleAtomID(u32::try_from(atom).expect("dispatch atom exceeds u32"));
    match kind {
        0 => DispatchKey::Part(atom),
        1 => DispatchKey::CustomState(atom),
        2 => DispatchKey::TagName(atom),
        3 => DispatchKey::Id(atom),
        4 => DispatchKey::Class(atom),
        5 => DispatchKey::AttributeName(atom),
        6 => DispatchKey::Directionality(atom),
        _ => unreachable!("dispatch atom kind out of range"),
    }
}

/// Buckets attached selector entries by their rightmost distinguishing feature.
///
/// A candidate probes only the buckets its own facts name, plus the universal bucket, so a document
/// full of `.item` elements never considers a rule whose subject compound requires `#header`. This
/// is program-derived dispatch rather than acceleration over elements: it is Tier 2, it is rebuilt
/// with the program, and it is never evicted independently of it.
struct AncestorDispatchTopology {
    key_indices: HashMap<DispatchKey, u32>,
    residency: MemoryLease,
}

impl Clone for AncestorDispatchTopology {
    fn clone(&self) -> Self {
        Self {
            key_indices: self.key_indices.clone(),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

impl Default for AncestorDispatchTopology {
    fn default() -> Self {
        Self {
            key_indices: HashMap::default(),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

struct RuleDispatchTopology {
    /// Mutable construction form, consumed when the directory is finalized.
    buckets: HashMap<DispatchKey, Vec<DispatchRow>>,
    bucket_directory: DispatchBucketDirectory,
    non_prefix_bucket_directory: DispatchBucketDirectory,
    /// Universal-subject entries that have no exact parent requirement.
    universal_without_parent_filter: Vec<DispatchRow>,
    /// Universal-subject entries indexed by the one feature their parent must carry.
    universal_by_parent: HashMap<DispatchKey, Vec<DispatchRow>>,
    universal_parent_directory: DispatchBucketDirectory,
    non_prefix_universal_parent_directory: DispatchBucketDirectory,
    /// The same parent-filtered entries as a conservative fallback when parent facts are absent.
    universal_with_parent_filter: Vec<DispatchRow>,
    non_prefix_universal_without_parent_filter: Vec<DispatchRow>,
    non_prefix_universal_with_parent_filter: Vec<DispatchRow>,
    finalized: bool,
    ancestors: Rc<AncestorDispatchTopology>,
    prefixes: PrefixAutomaton,
    residency: MemoryLease,
}

impl Default for RuleDispatchTopology {
    fn default() -> Self {
        Self {
            buckets: HashMap::default(),
            bucket_directory: DispatchBucketDirectory::default(),
            non_prefix_bucket_directory: DispatchBucketDirectory::default(),
            universal_without_parent_filter: Vec::new(),
            universal_by_parent: HashMap::default(),
            universal_parent_directory: DispatchBucketDirectory::default(),
            non_prefix_universal_parent_directory: DispatchBucketDirectory::default(),
            universal_with_parent_filter: Vec::new(),
            non_prefix_universal_without_parent_filter: Vec::new(),
            non_prefix_universal_with_parent_filter: Vec::new(),
            finalized: false,
            ancestors: Rc::new(AncestorDispatchTopology::default()),
            prefixes: PrefixAutomaton::default(),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) struct AncestorDispatchTopologyID(*const AncestorDispatchTopology);

pub struct RuleDispatch {
    entries: Rc<RuleDispatchEntries>,
    entry_bindings: Vec<DispatchEntryBinding>,
    entry_rows: Vec<Vec<DispatchRow>>,
    /// Direct cascade-order projection for every rule represented in this dispatch. Rule
    /// identities are program indices, so retained answers can restore an entry's order without
    /// searching the dispatch's much larger candidate table. Sparse pages keep a scope containing
    /// one late-created rule from allocating rows for every preceding rule in the document.
    cascade_order_rule_pages: Vec<Option<Box<[CascadeOrderRule; CASCADE_ORDER_RULE_PAGE_SIZE]>>>,
    cascade_orders_by_rule_entry: Vec<u32>,
    cascade_properties: Vec<u16>,
    cascade_entries: Vec<CascadeEntryData>,
    topology: Rc<RuleDispatchTopology>,
    residency: MemoryLease,
}

impl Default for RuleDispatch {
    fn default() -> Self {
        Self {
            entries: Rc::new(RuleDispatchEntries::default()),
            entry_bindings: Vec::new(),
            entry_rows: Vec::new(),
            cascade_order_rule_pages: Vec::new(),
            cascade_orders_by_rule_entry: Vec::new(),
            cascade_properties: Vec::new(),
            cascade_entries: Vec::new(),
            topology: Rc::new(RuleDispatchTopology::default()),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }
}

const CASCADE_ORDER_RULE_PAGE_SIZE: usize = 256;

#[derive(Clone, Copy)]
struct CascadeOrderRule {
    program: SelectorProgramID,
    entry_start: u32,
    entry_count: u32,
}

impl Default for CascadeOrderRule {
    fn default() -> Self {
        Self {
            program: SelectorProgramID(u32::MAX),
            entry_start: 0,
            entry_count: 0,
        }
    }
}

impl RuleDispatch {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    fn topology_mut(&mut self) -> &mut RuleDispatchTopology {
        Rc::get_mut(&mut self.topology).expect("a shared selector topology is immutable")
    }

    fn entries_mut(&mut self) -> &mut Vec<DispatchEntryMetadata> {
        &mut Rc::make_mut(&mut self.entries).rows
    }

    fn entry(&self, row: DispatchRow) -> DispatchEntry {
        let metadata = self.entries.rows[row.index()];
        let binding = self.entry_bindings[row.index()];
        let cascade_order = if binding.cascade_order_index == u32::MAX {
            0
        } else {
            self.cascade_orders_by_rule_entry[binding.cascade_order_index as usize]
        };
        DispatchEntry {
            identity: metadata.identity,
            rule: binding.rule,
            program: metadata.program,
            entry: metadata.entry,
            cascade_order,
            required_attribute_value: metadata.required_attribute_value,
            required_parent: metadata.required_parent,
            required_ancestor: metadata.required_ancestor,
            required_ancestor_index: metadata.required_ancestor_index,
            required_subject_bloom: metadata.required_subject_bloom,
            prefix_matched: metadata.prefix_matched,
            multi_key: metadata.multi_key,
        }
    }

    pub(super) fn rebind_rules(template: &Self, rules: &[RuleID]) -> Self {
        assert_eq!(template.entries.rows.len(), rules.len());
        Self {
            entries: Rc::clone(&template.entries),
            entry_bindings: rules
                .iter()
                .copied()
                .map(|rule| DispatchEntryBinding {
                    rule,
                    cascade_order_index: u32::MAX,
                })
                .collect(),
            entry_rows: template.entry_rows.clone(),
            cascade_order_rule_pages: Vec::new(),
            cascade_orders_by_rule_entry: Vec::new(),
            cascade_properties: Vec::new(),
            cascade_entries: Vec::new(),
            topology: Rc::clone(&template.topology),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        }
    }

    pub(super) fn rebind_rules_for_extension(template: &Self, rules: &[RuleID]) -> Self {
        let mut dispatch = Self::rebind_rules(template, rules);
        let topology = &template.topology;
        dispatch.topology = Rc::new(RuleDispatchTopology {
            buckets: match topology.finalized {
                true => topology.bucket_directory.to_buckets(),
                false => topology.buckets.clone(),
            },
            bucket_directory: DispatchBucketDirectory::default(),
            non_prefix_bucket_directory: DispatchBucketDirectory::default(),
            universal_without_parent_filter: topology.universal_without_parent_filter.clone(),
            universal_by_parent: match topology.finalized {
                true => topology.universal_parent_directory.to_buckets(),
                false => topology.universal_by_parent.clone(),
            },
            universal_parent_directory: DispatchBucketDirectory::default(),
            non_prefix_universal_parent_directory: DispatchBucketDirectory::default(),
            universal_with_parent_filter: topology.universal_with_parent_filter.clone(),
            non_prefix_universal_without_parent_filter: Vec::new(),
            non_prefix_universal_with_parent_filter: Vec::new(),
            finalized: false,
            ancestors: Rc::new((*topology.ancestors).clone()),
            prefixes: topology.prefixes.clone(),
            residency: MemoryLease::new(MemoryCategory::RuleProgram),
        });
        dispatch.topology_mut().prefixes.prepare_to_extend();
        dispatch
    }

    #[cfg(test)]
    pub(super) fn shares_topology_with(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.topology, &other.topology)
    }

    #[cfg(test)]
    pub(super) fn shares_entries_with(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.entries, &other.entries)
    }

    pub(super) fn shares_ancestor_topology_with(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.topology.ancestors, &other.topology.ancestors)
    }

    pub(super) fn ancestor_topology_id(&self) -> AncestorDispatchTopologyID {
        AncestorDispatchTopologyID(Rc::as_ptr(&self.topology.ancestors))
    }

    pub(super) fn ancestor_dispatch_shape(&self) -> AncestorDispatchShape {
        let key_indices = &self.topology.ancestors.key_indices;
        let mut keys = vec![DispatchKey::Universal; key_indices.len()];
        for (&key, &index) in key_indices {
            keys[index as usize] = key;
        }
        AncestorDispatchShape(keys)
    }

    pub(super) fn share_ancestor_topology_with(&mut self, template: &Self) {
        if self.shares_ancestor_topology_with(template) {
            return;
        }
        debug_assert_eq!(
            self.topology.ancestors.key_indices,
            template.topology.ancestors.key_indices
        );
        self.topology_mut().ancestors = Rc::clone(&template.topology.ancestors);
    }

    pub(super) fn insert(&mut self, key: DispatchKey, mut entry: DispatchEntry) -> DispatchRow {
        debug_assert!(
            !self.topology.finalized,
            "extend a finalized dispatch through the extension path"
        );
        entry.required_ancestor_index = entry.required_ancestor.map(|required| {
            let topology = self.topology_mut();
            let ancestors = Rc::get_mut(&mut topology.ancestors).expect("a shared ancestor topology is immutable");
            let next = u32::try_from(ancestors.key_indices.len()).expect("ancestor requirement space exhausted");
            *ancestors.key_indices.entry(required).or_insert(next)
        });
        let id = DispatchRow::from_index(self.entries.rows.len());
        self.entries_mut().push(DispatchEntryMetadata {
            identity: entry.identity,
            program: entry.program,
            entry: entry.entry,
            required_attribute_value: entry.required_attribute_value,
            required_parent: entry.required_parent,
            required_ancestor: entry.required_ancestor,
            required_ancestor_index: entry.required_ancestor_index,
            required_subject_bloom: entry.required_subject_bloom,
            prefix_matched: entry.prefix_matched,
            multi_key: entry.multi_key,
        });
        self.entry_bindings.push(DispatchEntryBinding {
            rule: entry.rule,
            cascade_order_index: u32::MAX,
        });
        if self.entry_rows.len() <= entry.identity.0 as usize {
            self.entry_rows.resize_with(entry.identity.0 as usize + 1, Vec::new);
        }
        self.entry_rows[entry.identity.0 as usize].push(id);
        self.topology_mut().buckets.entry(key).or_default().push(id);
        if key == DispatchKey::Universal {
            self.index_universal_entry(id);
        }
        id
    }

    pub(super) fn add_prefix_entry(
        &mut self,
        programs: &super::selector::SelectorPrograms,
        program: SelectorProgramID,
        chain: &[super::selector::SelectorPrefixStep],
        row: DispatchRow,
        structural_tests_admissible: bool,
    ) {
        // Registration can refuse a chain whose structural tests would overflow the automaton's
        // truth bit space or whose origin does not admit them; the entry then stays a candidate
        // for the exact evaluator.
        let entry = self.entries.rows[row.index()].identity;
        if self
            .topology_mut()
            .prefixes
            .add_entry(programs, program, chain, entry, structural_tests_admissible)
        {
            self.entries_mut()[row.index()].prefix_matched = true;
        }
    }

    pub(super) fn finish_prefixes(&mut self) {
        debug_assert!(!self.topology.finalized, "a dispatch can only be finalized once");
        self.topology_mut().prefixes.finish();
        self.finalize_bucket_directories();
        self.rebuild_universal_with_parent_filter();
        self.rebuild_non_prefix_index();
    }

    #[must_use]
    pub(super) fn prefixes(&self) -> &PrefixAutomaton {
        &self.topology.prefixes
    }

    pub(super) fn entries_for_identity(&self, entry: EntryID) -> impl Iterator<Item = DispatchEntry> + '_ {
        self.entry_rows
            .get(entry.0 as usize)
            .into_iter()
            .flatten()
            .map(|&row| self.entry(row))
    }

    #[must_use]
    #[cfg(test)]
    pub(super) fn entry_at(&self, index: usize) -> DispatchEntry {
        self.entry(DispatchRow::from_index(index))
    }

    fn index_universal_entry(&mut self, id: DispatchRow) {
        let entry = self.entry(id);
        let topology = self.topology_mut();
        match entry.required_parent {
            Some(parent) => {
                topology.universal_by_parent.entry(parent).or_default().push(id);
            }
            None => topology.universal_without_parent_filter.push(id),
        }
    }

    fn rebuild_universal_with_parent_filter(&mut self) {
        let entries = self
            .bucket_ids(DispatchKey::Universal, CandidateEntries::All)
            .iter()
            .copied()
            .filter(|row| self.entries.rows[row.index()].required_parent.is_some())
            .collect();
        self.topology_mut().universal_with_parent_filter = entries;
    }

    fn finalize_bucket_directories(&mut self) {
        let topology = self.topology_mut();
        topology.bucket_directory = DispatchBucketDirectory::build(std::mem::take(&mut topology.buckets));
        topology.universal_parent_directory =
            DispatchBucketDirectory::build(std::mem::take(&mut topology.universal_by_parent));
        topology.finalized = true;
    }

    fn rebuild_non_prefix_index(&mut self) {
        let entries = Rc::clone(&self.entries);
        let topology = self.topology_mut();
        topology.non_prefix_bucket_directory = topology
            .bucket_directory
            .filtered(|row| !entries.rows[row.index()].prefix_matched);
        topology.non_prefix_universal_parent_directory = topology
            .universal_parent_directory
            .filtered(|row| !entries.rows[row.index()].prefix_matched);
        topology.non_prefix_universal_without_parent_filter = topology
            .universal_without_parent_filter
            .iter()
            .copied()
            .filter(|row| !entries.rows[row.index()].prefix_matched)
            .collect();
        topology.non_prefix_universal_with_parent_filter = topology
            .universal_with_parent_filter
            .iter()
            .copied()
            .filter(|row| !entries.rows[row.index()].prefix_matched)
            .collect();
    }

    /// Assign a dense rank to the static cascade priority of every selector entry.
    ///
    /// The dispatch is Tier 2 program data, so a rule or ordering change rebuilds these ranks along
    /// with the buckets. Dynamic `@scope` proximity is deliberately absent and makes the consumer
    /// fall back to constructing the complete priority.
    ///
    /// A subject whose disjunction has selective branches is inserted once per branch key, and
    /// those copies are one selector entry. They therefore share one rank, which is what the
    /// candidate walk deduplicates them by.
    pub fn assign_cascade_order<K: Ord>(&mut self, mut priority_of: impl FnMut(DispatchEntry) -> K) {
        let mut ordered: Vec<(K, RuleID, SelectorProgramID, u32, DispatchRow)> = (0..self.entry_count())
            .map(|index| {
                let entry = self.entry(DispatchRow::from_index(index));
                (
                    priority_of(entry),
                    entry.rule,
                    entry.program,
                    entry.entry,
                    DispatchRow::from_index(index),
                )
            })
            .collect();
        ordered.sort_unstable();

        let mut cascade_orders_by_row = vec![0; self.entry_count()];
        let mut group_end = 0;
        for group in ordered
            .chunk_by(|left, right| left.0 == right.0 && left.1 == right.1 && left.2 == right.2 && left.3 == right.3)
        {
            group_end += group.len();
            let cascade_order = u32::try_from(group_end - 1).expect("dispatch entry space exhausted");
            for ordered_entry in group {
                cascade_orders_by_row[ordered_entry.4.index()] = cascade_order;
            }
        }

        self.rebuild_cascade_order_projection(&cascade_orders_by_row);
    }

    pub(super) fn reuse_cascade_order(&mut self, template: &Self) {
        assert_eq!(self.entry_count(), template.entry_count());
        let mut cascade_orders_by_row = Vec::with_capacity(self.entry_count());
        for index in 0..self.entry_count() {
            let entry = self.entry(DispatchRow::from_index(index));
            let template_entry = template.entry(DispatchRow::from_index(index));
            assert_eq!(entry.program, template_entry.program);
            assert_eq!(entry.entry, template_entry.entry);
            cascade_orders_by_row.push(template_entry.cascade_order);
        }
        self.rebuild_cascade_order_projection(&cascade_orders_by_row);
    }

    fn rebuild_cascade_order_projection(&mut self, cascade_orders_by_row: &[u32]) {
        assert_eq!(cascade_orders_by_row.len(), self.entry_count());
        let mut entries_by_identity: Vec<_> = (0..self.entry_count()).map(DispatchRow::from_index).collect();
        entries_by_identity.sort_unstable_by_key(|&id| {
            let metadata = self.entries.rows[id.index()];
            (self.entry_bindings[id.index()].rule, metadata.program, metadata.entry)
        });
        entries_by_identity.dedup_by_key(|id| {
            let metadata = self.entries.rows[id.index()];
            (self.entry_bindings[id.index()].rule, metadata.program, metadata.entry)
        });
        self.cascade_order_rule_pages.clear();
        self.cascade_orders_by_rule_entry.clear();
        self.cascade_orders_by_rule_entry.reserve(entries_by_identity.len());
        for id in entries_by_identity {
            let metadata = self.entries.rows[id.index()];
            let binding = self.entry_bindings[id.index()];
            let rule_index = binding.rule.0 as usize;
            let page_index = rule_index / CASCADE_ORDER_RULE_PAGE_SIZE;
            if self.cascade_order_rule_pages.len() <= page_index {
                self.cascade_order_rule_pages.resize_with(page_index + 1, || None);
            }
            let page = self.cascade_order_rule_pages[page_index]
                .get_or_insert_with(|| Box::new([CascadeOrderRule::default(); CASCADE_ORDER_RULE_PAGE_SIZE]));
            let rule = &mut page[rule_index % CASCADE_ORDER_RULE_PAGE_SIZE];
            if rule.entry_count == 0 {
                rule.program = metadata.program;
                rule.entry_start =
                    u32::try_from(self.cascade_orders_by_rule_entry.len()).expect("dispatch entry space exhausted");
            }
            assert_eq!(
                rule.program, metadata.program,
                "one rule cannot have multiple selector programs"
            );
            assert_eq!(
                rule.entry_count, metadata.entry,
                "a rule's selector entries must form a dense identity space"
            );
            rule.entry_count = rule.entry_count.checked_add(1).expect("selector entry space exhausted");
            self.cascade_orders_by_rule_entry
                .push(cascade_orders_by_row[id.index()]);
        }
        for (row, binding) in self.entry_bindings.iter_mut().enumerate() {
            let metadata = self.entries.rows[row];
            let rule_index = binding.rule.0 as usize;
            let page = self.cascade_order_rule_pages[rule_index / CASCADE_ORDER_RULE_PAGE_SIZE]
                .as_deref()
                .expect("a dispatch binding must have a cascade-order page");
            let rule = &page[rule_index % CASCADE_ORDER_RULE_PAGE_SIZE];
            binding.cascade_order_index = rule.entry_start + metadata.entry;
        }
    }

    #[must_use]
    pub(super) fn cascade_order_for_entry(
        &self,
        rule: RuleID,
        program: SelectorProgramID,
        selector_entry: u32,
    ) -> Option<u32> {
        let rule_index = rule.0 as usize;
        let page = self
            .cascade_order_rule_pages
            .get(rule_index / CASCADE_ORDER_RULE_PAGE_SIZE)?
            .as_deref()?;
        let rule = &page[rule_index % CASCADE_ORDER_RULE_PAGE_SIZE];
        if rule.program != program || selector_entry >= rule.entry_count {
            return None;
        }
        self.cascade_orders_by_rule_entry
            .get(rule.entry_start as usize + selector_entry as usize)
            .copied()
    }

    /// Publish each entry's complete normal author declaration inventory beside the dispatch.
    ///
    /// Cascade matching reads this directly while walking candidates. Keeping it in the immutable
    /// dispatch avoids looking the rule up through several program maps for every element.
    pub fn assign_cascade_properties<Properties: ExactSizeIterator<Item = u16>>(
        &mut self,
        mut blocks_pruning: impl FnMut(DispatchEntry) -> bool,
        mut properties_of: impl FnMut(DispatchEntry) -> Option<Properties>,
    ) {
        self.cascade_properties.clear();
        self.cascade_entries.clear();
        self.cascade_entries
            .resize(self.entry_count(), CascadeEntryData::default());
        let mut configured = vec![false; self.entry_count()];
        for index in 0..self.entry_count() {
            let entry = self.entry(DispatchRow::from_index(index));
            let order = entry.cascade_order as usize;
            if configured[order] {
                continue;
            }
            configured[order] = true;
            let data = &mut self.cascade_entries[order];
            data.pruning_blocker = blocks_pruning(entry);
            data.property_start =
                u32::try_from(self.cascade_properties.len()).expect("cascade property space exhausted");
            let Some(properties) = properties_of(entry) else {
                continue;
            };
            data.property_count =
                u16::try_from(properties.len()).expect("a rule cannot declare more than u16::MAX longhands");
            self.cascade_properties.extend(properties);
            data.prunable = true;
        }
    }

    #[must_use]
    pub fn cascade_properties(&self, entry: DispatchEntry) -> Option<&[u16]> {
        let data = self.cascade_entries.get(entry.cascade_order as usize)?;
        if !data.prunable {
            return None;
        }
        let start = data.property_start as usize;
        Some(&self.cascade_properties[start..start + data.property_count as usize])
    }

    #[must_use]
    pub fn cascade_properties_for_order(&self, order: u32) -> Option<&[u16]> {
        let data = self.cascade_entries.get(order as usize)?;
        if !data.prunable {
            return None;
        }
        let start = data.property_start as usize;
        Some(&self.cascade_properties[start..start + data.property_count as usize])
    }

    #[must_use]
    pub fn cascade_pruning_blocker_for_order(&self, order: u32) -> bool {
        self.cascade_entries
            .get(order as usize)
            .is_some_and(|entry| entry.pruning_blocker)
    }

    #[must_use]
    pub fn cascade_pruning_blocker(&self, entry: DispatchEntry) -> bool {
        self.cascade_pruning_blocker_for_order(entry.cascade_order)
    }

    fn bucket_ids(&self, key: DispatchKey, entries: CandidateEntries) -> &[DispatchRow] {
        if self.topology.finalized {
            return match entries {
                CandidateEntries::All => self.topology.bucket_directory.get(key),
                CandidateEntries::NonPrefix => self.topology.non_prefix_bucket_directory.get(key),
            };
        }
        debug_assert!(entries == CandidateEntries::All);
        self.topology.buckets.get(&key).map_or(&[], Vec::as_slice)
    }

    fn universal_parent_bucket_ids(&self, key: DispatchKey, entries: CandidateEntries) -> &[DispatchRow] {
        if self.topology.finalized {
            return match entries {
                CandidateEntries::All => self.topology.universal_parent_directory.get(key),
                CandidateEntries::NonPrefix => self.topology.non_prefix_universal_parent_directory.get(key),
            };
        }
        debug_assert!(entries == CandidateEntries::All);
        self.topology.universal_by_parent.get(&key).map_or(&[], Vec::as_slice)
    }

    pub fn bucket(&self, key: DispatchKey) -> impl ExactSizeIterator<Item = DispatchEntry> + '_ {
        self.bucket_ids(key, CandidateEntries::All)
            .iter()
            .map(|&id| self.entry(id))
    }

    #[must_use]
    pub fn entry_count(&self) -> usize {
        self.entries.rows.len()
    }

    #[must_use]
    pub fn ancestor_key_count(&self) -> usize {
        self.topology.ancestors.key_indices.len()
    }

    #[must_use]
    pub fn ancestor_key_index(&self, key: DispatchKey) -> Option<u32> {
        self.topology.ancestors.key_indices.get(&key).copied()
    }

    /// Entries a candidate has to consider, given its local facts.
    ///
    /// Facts stream their keys directly into the dispatch. `workspace` gives every entry one dense
    /// epoch stamp, so aliases and multiple matching attributes cannot emit it twice.
    #[allow(clippy::too_many_arguments)]
    pub fn candidates_for<'a>(
        &'a self,
        facts: &'a StyleNodeFacts,
        row: u32,
        is_document_root: bool,
        parent: ParentDispatchFacts,
        ancestors: Option<AncestorDispatchFacts<'a>>,
        entries: CandidateEntries,
        descending_cascade_order: bool,
        workspace: &'a mut DispatchCandidateWorkspace,
    ) -> impl Iterator<Item = DispatchEntry> + 'a {
        workspace.begin(self.entry_count());
        debug_assert!(self.topology.finalized || entries == CandidateEntries::All);
        let universal_without_parent_filter = match entries {
            CandidateEntries::All => &self.topology.universal_without_parent_filter,
            CandidateEntries::NonPrefix => &self.topology.non_prefix_universal_without_parent_filter,
        };
        let universal_with_parent_filter = match entries {
            CandidateEntries::All => &self.topology.universal_with_parent_filter,
            CandidateEntries::NonPrefix => &self.topology.non_prefix_universal_with_parent_filter,
        };
        let subject_bloom = facts.dispatch_bloom_of(row, is_document_root);
        {
            let mut offer = |id: DispatchRow, attribute_value: Option<StyleAtomID>| {
                let metadata = self.entries.rows[id.index()];
                if !metadata.required_attribute_value.is_none()
                    && attribute_value != Some(metadata.required_attribute_value)
                {
                    return;
                }
                if metadata.required_subject_bloom & subject_bloom != metadata.required_subject_bloom {
                    return;
                }
                if !workspace.admit(id) {
                    return;
                }
                if !match (metadata.required_parent, parent) {
                    (None, _) => true,
                    (Some(_), ParentDispatchFacts::NoElementParent) => false,
                    (Some(required), ParentDispatchFacts::Known { row, is_document_root }) => {
                        facts.carries_dispatch_key(row, required, is_document_root)
                    }
                    (Some(_), ParentDispatchFacts::Unknown) => true,
                } {
                    return;
                }
                if !match (metadata.required_ancestor_index, ancestors) {
                    (Some(index), Some(ancestors)) => ancestors.contains(index),
                    _ => true,
                } {
                    return;
                }
                workspace.candidates.push(id);
            };

            for &id in universal_without_parent_filter {
                offer(id, None);
            }
            match parent {
                ParentDispatchFacts::Known { row, is_document_root } => {
                    facts.for_each_dispatch_probe(row, is_document_root, |key, _| {
                        for &id in self.universal_parent_bucket_ids(key, entries) {
                            offer(id, None);
                        }
                    });
                }
                ParentDispatchFacts::Unknown => {
                    for &id in universal_with_parent_filter {
                        offer(id, None);
                    }
                }
                ParentDispatchFacts::NoElementParent => {}
            }
            facts.for_each_dispatch_probe(row, is_document_root, |key, attribute_value| {
                if key == DispatchKey::Universal {
                    return;
                }
                for &id in self.bucket_ids(key, entries) {
                    offer(id, attribute_value);
                }
            });
        }
        if descending_cascade_order && workspace.candidates.len() > 1 {
            workspace.cascade_sort.extend(workspace.candidates.iter().map(|&id| {
                let binding = self.entry_bindings[id.index()];
                let cascade_order = if binding.cascade_order_index == u32::MAX {
                    0
                } else {
                    self.cascade_orders_by_rule_entry[binding.cascade_order_index as usize]
                };
                (
                    Reverse((self.cascade_pruning_blocker_for_order(cascade_order), cascade_order)),
                    id,
                )
            }));
            // Rows with the same rank are copies of one selector entry and are deduplicated by
            // cascade order at consumption, so their relative order is unobservable.
            workspace.cascade_sort.sort_unstable_by_key(|&(key, _)| key);
            for (candidate, &(_, sorted)) in workspace.candidates.iter_mut().zip(&workspace.cascade_sort) {
                *candidate = sorted;
            }
        }
        workspace.candidates.iter().map(|&id| self.entry(id))
    }

    fn scope_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.entry_bindings,
                self.entry_rows,
                self.cascade_order_rule_pages,
                self.cascade_orders_by_rule_entry,
                self.cascade_properties,
                self.cascade_entries,
            ];
            cached [];
            nested [
                self
                    .cascade_order_rule_pages
                    .iter()
                    .flatten()
                    .map(|page| size_of_val(page.as_ref()))
                    .sum::<usize>(),
                self
                    .entry_rows
                    .iter()
                    .map(|rows| rows.capacity() * size_of::<DispatchRow>())
                    .sum::<usize>(),
            ];
            skip [self.entries, self.residency];
        }
    }

    fn topology_capacity_bytes(topology: &RuleDispatchTopology) -> u64 {
        capacity_bytes! {
            shallow [
                topology.buckets,
                topology.universal_without_parent_filter,
                topology.universal_by_parent,
                topology.universal_with_parent_filter,
                topology.non_prefix_universal_without_parent_filter,
                topology.non_prefix_universal_with_parent_filter,
            ];
            cached [];
            nested [
                topology
                    .buckets
                    .values()
                .map(|bucket| bucket.capacity() * size_of::<DispatchRow>())
                .sum::<usize>(),
                topology
                    .universal_by_parent
                .values()
                .map(|bucket| bucket.capacity() * size_of::<DispatchRow>())
                .sum::<usize>(),
                topology.bucket_directory.capacity_bytes(),
                topology.non_prefix_bucket_directory.capacity_bytes(),
                topology.universal_parent_directory.capacity_bytes(),
                topology.non_prefix_universal_parent_directory.capacity_bytes(),
                topology.prefixes.capacity_bytes(),
            ];
            skip [topology.ancestors, topology.finalized, topology.residency];
        }
    }

    fn ancestor_capacity_bytes(ancestors: &AncestorDispatchTopology) -> u64 {
        capacity_bytes! {
            shallow [ancestors.key_indices];
            cached [];
            nested [];
            skip [ancestors.residency];
        }
    }

    pub(super) fn settle_memory(&mut self, memory: &mut MemoryController) {
        self.residency.resize_required_to(memory, self.scope_capacity_bytes());
        if let Some(entries) = Rc::get_mut(&mut self.entries) {
            entries.residency.resize_required_to(memory, entries.capacity_bytes());
        }
        let Some(topology) = Rc::get_mut(&mut self.topology) else {
            return;
        };
        topology
            .residency
            .resize_required_to(memory, Self::topology_capacity_bytes(topology));
        let Some(ancestors) = Rc::get_mut(&mut topology.ancestors) else {
            return;
        };
        ancestors
            .residency
            .resize_required_to(memory, Self::ancestor_capacity_bytes(ancestors));
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        self.scope_capacity_bytes()
            + self.entries.capacity_bytes()
            + Self::topology_capacity_bytes(&self.topology)
            + Self::ancestor_capacity_bytes(&self.topology.ancestors)
    }
}

/// The accumulated local facts of the document's style nodes.
///
/// It is built from the same feature and state deltas the journal normalizes, so a fact exists here
/// exactly because a mutation published it. The store's [`StyleNodeFacts`] is both the input-side
/// projection and the current-side evaluation arrangement; bounded batches are temporary views of
/// selected rows.
type ElementDeclaredProperties = Box<[DeclaredProperty]>;

struct ElementDeclarationRow {
    by_kind: [Option<ElementDeclaredProperties>; ElementDeclarationKind::COUNT],
    /// The value each declaration was written with, parallel to `by_kind`, when the block's
    /// publication carried them; a consumer computing from the declarations reads the spelling.
    written_by_kind: [Option<Box<[RetainedStyleValueData]>>; ElementDeclarationKind::COUNT],
    complete: [bool; ElementDeclarationKind::COUNT],
    /// The custom properties the inline style declares, in declaration order. Only the `style`
    /// attribute declares custom properties.
    custom_declarations: Option<Box<[CustomDeclaration]>>,
    /// The values the custom declarations were written with, parallel to them.
    custom_written_values: Option<Box<[RetainedStyleValueData]>>,
}

impl Default for ElementDeclarationRow {
    fn default() -> Self {
        Self {
            by_kind: Default::default(),
            written_by_kind: Default::default(),
            complete: [true; ElementDeclarationKind::COUNT],
            custom_declarations: None,
            custom_written_values: None,
        }
    }
}

impl ElementDeclarationRow {
    fn storage_bytes(&self) -> u64 {
        (size_of::<Self>()
            + self
                .by_kind
                .iter()
                .flatten()
                .map(|declared| size_of_val(declared.as_ref()))
                .sum::<usize>()
            + self
                .written_by_kind
                .iter()
                .flatten()
                .map(|written| size_of_val(written.as_ref()))
                .sum::<usize>()
            + self
                .custom_declarations
                .as_ref()
                .map_or(0, |custom| size_of_val(custom.as_ref()))
            + self
                .custom_written_values
                .as_ref()
                .map_or(0, |written| size_of_val(written.as_ref()))) as u64
    }

    fn is_empty(&self) -> bool {
        self.by_kind.iter().all(Option::is_none)
            && self.complete.iter().all(|&complete| complete)
            && self.custom_declarations.is_none()
    }
}

/// Element-declaration rows addressed directly by dense element identity.
///
/// Each directory slot costs one pointer. A populated slot contains the fixed declaration-kind
/// columns, and each property list owns one exact-sized allocation.
#[derive(Default)]
struct ElementDeclarationRows {
    rows: Column<Option<Box<ElementDeclarationRow>>>,
    payload_bytes: u64,
    /// How many rows hold custom declarations: none means no inline style declares one.
    rows_with_custom_declarations: usize,
}

impl ElementDeclarationRows {
    fn get(&self, node: StyleNodeID, kind: ElementDeclarationKind) -> (&[DeclaredProperty], bool) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return (&[], true);
        };
        let Some(row) = self.rows.get(index).and_then(Option::as_ref) else {
            return (&[], true);
        };
        (
            row.by_kind[kind.index()].as_deref().unwrap_or(&[]),
            row.complete[kind.index()]
                && (kind != ElementDeclarationKind::InlineStyle || row.custom_declarations.is_none()),
        )
    }

    /// Whether the longhand declarations of one kind are all published, whatever custom
    /// properties the inline style declares beside them.
    fn complete_but_for_custom(&self, node: StyleNodeID, kind: ElementDeclarationKind) -> bool {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return true;
        };
        let Some(row) = self.rows.get(index).and_then(Option::as_ref) else {
            return true;
        };
        row.complete[kind.index()]
    }

    /// The values the declarations of one kind were written with, parallel to `get`, or nothing
    /// when the publication carried none.
    fn get_written(&self, node: StyleNodeID, kind: ElementDeclarationKind) -> &[RetainedStyleValueData] {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return &[];
        };
        let Some(row) = self.rows.get(index).and_then(Option::as_ref) else {
            return &[];
        };
        row.written_by_kind[kind.index()].as_deref().unwrap_or(&[])
    }

    fn set(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declared: Vec<DeclaredProperty>,
        written_values: Vec<RetainedStyleValueData>,
        declarations_are_complete: bool,
    ) {
        let index = node.element_index().expect("only elements carry element declarations") as usize;
        if declared.is_empty() && declarations_are_complete {
            self.remove_kind(node, kind);
            return;
        }
        let row = self.rows.entry(index);
        let before = row.as_ref().map_or(0, |row| row.storage_bytes());
        let row = row.get_or_insert_with(Box::default);
        row.written_by_kind[kind.index()] =
            (!declared.is_empty() && written_values.len() == declared.len()).then(|| written_values.into_boxed_slice());
        row.by_kind[kind.index()] = (!declared.is_empty()).then(|| declared.into_boxed_slice());
        row.complete[kind.index()] = declarations_are_complete;
        let after = row.storage_bytes();
        self.payload_bytes = self.payload_bytes - before + after;
    }

    /// The custom properties the node's inline style declares.
    fn get_custom(&self, node: StyleNodeID) -> &[CustomDeclaration] {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return &[];
        };
        let Some(row) = self.rows.get(index).and_then(Option::as_ref) else {
            return &[];
        };
        row.custom_declarations.as_deref().unwrap_or(&[])
    }

    /// The values the node's inline custom declarations were written with, parallel to them, or
    /// nothing when their publication carried none.
    fn get_custom_written(&self, node: StyleNodeID) -> &[RetainedStyleValueData] {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return &[];
        };
        let Some(row) = self.rows.get(index).and_then(Option::as_ref) else {
            return &[];
        };
        row.custom_written_values.as_deref().unwrap_or(&[])
    }

    fn set_custom(
        &mut self,
        node: StyleNodeID,
        custom_declarations: Vec<CustomDeclaration>,
        custom_written_values: Vec<RetainedStyleValueData>,
    ) {
        let index = node.element_index().expect("only elements carry element declarations") as usize;
        if custom_declarations.is_empty() {
            let Some(row) = self.rows.get_mut(index).and_then(Option::as_mut) else {
                return;
            };
            let before = row.storage_bytes();
            if row.custom_declarations.take().is_some() {
                self.rows_with_custom_declarations -= 1;
            }
            row.custom_written_values = None;
            let after = match row.is_empty() {
                true => 0,
                false => row.storage_bytes(),
            };
            if after == 0 {
                self.rows[index] = None;
            }
            self.payload_bytes = self.payload_bytes - before + after;
            return;
        }
        let row = self.rows.entry(index);
        let before = row.as_ref().map_or(0, |row| row.storage_bytes());
        let row = row.get_or_insert_with(Box::default);
        row.custom_written_values = (custom_written_values.len() == custom_declarations.len())
            .then(|| custom_written_values.into_boxed_slice());
        if row
            .custom_declarations
            .replace(custom_declarations.into_boxed_slice())
            .is_none()
        {
            self.rows_with_custom_declarations += 1;
        }
        let after = row.storage_bytes();
        self.payload_bytes = self.payload_bytes - before + after;
    }

    fn remove_kind(&mut self, node: StyleNodeID, kind: ElementDeclarationKind) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        let Some(row) = self.rows.get_mut(index).and_then(Option::as_mut) else {
            return;
        };
        let before = row.storage_bytes();
        row.by_kind[kind.index()] = None;
        row.written_by_kind[kind.index()] = None;
        row.complete[kind.index()] = true;
        if kind == ElementDeclarationKind::InlineStyle {
            if row.custom_declarations.take().is_some() {
                self.rows_with_custom_declarations -= 1;
            }
            row.custom_written_values = None;
        }
        let after = match row.is_empty() {
            true => 0,
            false => row.storage_bytes(),
        };
        if after == 0 {
            self.rows[index] = None;
        }
        self.payload_bytes = self.payload_bytes - before + after;
    }

    fn remove(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        let Some(row) = self.rows.get_mut(index).and_then(Option::take) else {
            return;
        };
        if row.custom_declarations.is_some() {
            self.rows_with_custom_declarations -= 1;
        }
        self.payload_bytes -= row.storage_bytes();
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.rows];
            cached [self.payload_bytes];
            nested [];
            skip [];
        }
    }
}

define_id! { struct CustomPropertyNameSetID(); }

impl super::intern_table::InternIdentity for CustomPropertyNameSetID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

pub struct ElementFactStore {
    /// Required primary arrangement. Element identity selects its fixed column slots directly;
    /// variable facts are append-only payloads reached through the slots' handles.
    rows: Rc<StyleNodeFacts>,
    /// Shared dictionaries used by primary rows and materialized batches. Keep this handle outside
    /// `rows` so publishing a catalog entry never copies every primary fact column.
    attribute_catalogs: Rc<AttributeCatalogs>,
    #[cfg(test)]
    attribute_catalog_copies: u64,
    staging: FactStaging,
    metadata: Column<Option<ElementFactMetadata>>,
    /// Logical bytes reachable through live primary handles.
    primary_live_bytes: u64,
    primary_live_payload_bytes: u64,
    primary_stale_payload_bytes: u64,
    /// Candidate sets over the same facts. A rule arriving needs to find the elements it could
    /// match without walking the document, and this is what answers that.
    postings: FeaturePostings,
    /// Tier-3 headroom when admission last closed during a full rebuild. Until more headroom
    /// appears, another scan can only reach the same closure.
    posting_rebuild_closed_at_headroom: Option<u64>,
    memory: MemoryLease,
    memory_dirty: bool,
    settled_non_apply_capacity_bytes: u64,
    /// One entry per distinct set of declared custom property names, and the sets each name is in.
    /// A theme is one set however many elements it decides for, which is what keeps the index
    /// proportional to the stylesheet rather than to the document times the stylesheet.
    custom_property_name_sets: super::intern_table::InternTable<CustomPropertyNameSetID, Box<[StyleAtomID]>>,
    custom_property_name_set_vacancies: Vec<u32>,
    custom_property_set_ids_by_name: PagedOwnedColumn<Vec<u32>>,
    /// Authoritative semantic references from committed fact rows and per-element metadata. This
    /// is indexed by atom so a lifetime sweep visits distinct identities rather than every live
    /// element row.
    atom_live_counts: PagedCopyColumn<u32>,
    language_live_counts: PagedCopyColumn<u32>,
    attribute_name_live_counts: PagedCopyColumn<u32>,
    attribute_value_live_counts: PagedCopyColumn<u32>,
    /// Changes whenever a newly published value can change an attribute-value query plan.
    attribute_value_catalog_version: u64,
    custom_property_set_live_counts: Vec<u64>,
    /// Attribute-name forms and value text shared by the primary and each bounded fact batch.
    ///
    /// An attribute is keyed by the name that is unique to it - its qualified atom, or its bare local
    /// name where it is in no namespace - because an element can carry the same local name in several
    /// namespaces at once and each of those is a fact of its own. `[*|x]` asks about all of them
    /// together, and the any-namespace atom is the identity they share, so it is held per name rather
    /// than per element: one name has one local form however many elements carry it.
    /// The longhand properties each element declares itself, by the kind of declaration they came
    /// from. An element-attached declaration beats every rule in its context, so the cascade needs
    /// to know which properties one covers just as it does for a rule.
    element_declared_properties: ElementDeclarationRows,
}

#[derive(Debug, Default, PartialEq, Eq)]
struct StagedFactRow {
    tag: StyleAtomID,
    /// The ASCII-lowercase folding of `tag`, held only when it differs from it. Type selectors
    /// dispatch on their folded form, so an element whose local name is not already lowercase is
    /// posted under both names and reachable either way.
    folded_tag: StyleAtomID,
    id: StyleAtomID,
    /// The element's resolved language tag.
    language: StyleAtomID,
    /// The element's namespace URI, keyed by its text.
    namespace: StyleAtomID,
    /// The outermost host a `::part()` rule can address this element from.
    part_exposure: StyleAtomID,
    /// The element's resolved directionality.
    directionality: StyleAtomID,
    /// The element's heading level where it has one, 1 to 6, and zero where it does not.
    heading_level: u8,
    /// Whether the element holds a text or comment child that keeps it from being `:empty`. Element
    /// children are style nodes and answer for themselves; a text node is not, so this is the only
    /// thing that can say it is there.
    has_text_content: bool,
    states: StateSet,
    custom_states: Vec<StyleAtomID>,
    parts: Vec<StyleAtomID>,
    classes: Vec<StyleAtomID>,
    /// The attributes the element carries, sorted by name, each with the atom of its value.
    attributes: Vec<(StyleAtomID, StyleAtomID)>,
}

impl StagedFactRow {
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.custom_states, self.parts, self.classes, self.attributes];
            cached [];
            nested [];
            skip [
                self.tag,
                self.folded_tag,
                self.id,
                self.language,
                self.namespace,
                self.part_exposure,
                self.directionality,
                self.heading_level,
                self.has_text_content,
                self.states,
            ];
        }
    }
}

#[derive(Default)]
struct FactStaging {
    rows: PagedColumn<FactStagingPage>,
    entries: Vec<Option<StagedFactRowPair>>,
    touched: Vec<(StyleNodeID, u32)>,
    dirty: Vec<(StyleNodeID, u32)>,
    live_count: usize,
    dirty_count: usize,
    capacity_bytes: u64,
}

const FACT_STAGING_PAGE_SHIFT: usize = 6;
const FACT_STAGING_PAGE_SIZE: usize = 1 << FACT_STAGING_PAGE_SHIFT;

struct FactStagingPage {
    entries: [u32; FACT_STAGING_PAGE_SIZE],
}

impl Default for FactStagingPage {
    fn default() -> Self {
        Self {
            entries: [NO_ROW; FACT_STAGING_PAGE_SIZE],
        }
    }
}

impl PagedColumnPage for FactStagingPage {
    type Value = u32;

    const SHIFT: usize = FACT_STAGING_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<Self::Value> {
        (self.entries[index] != NO_ROW).then_some(self.entries[index])
    }

    fn insert(&mut self, index: usize, value: Self::Value) {
        self.entries[index] = value;
    }
}

impl RemovablePagedColumnPage for FactStagingPage {
    fn remove(&mut self, index: usize) -> Option<Self::Value> {
        let previous = std::mem::replace(&mut self.entries[index], NO_ROW);
        (previous != NO_ROW).then_some(previous)
    }
}

struct StagedFactRowPair {
    before: Option<PrimaryFactSnapshot>,
    after: StagedFactRow,
    dirty: bool,
}

impl FactStaging {
    fn index(node: StyleNodeID) -> usize {
        node.element_index().expect("only elements carry fact staging") as usize
    }

    fn is_empty(&self) -> bool {
        self.live_count == 0
    }

    fn has_dirty(&self) -> bool {
        self.dirty_count != 0
    }

    fn len(&self) -> usize {
        self.live_count
    }

    fn contains(&self, node: StyleNodeID) -> bool {
        self.get(node).is_some()
    }

    fn get(&self, node: StyleNodeID) -> Option<&StagedFactRow> {
        let entry = self.rows.get(Self::index(node))? as usize;
        Some(&self.entries.get(entry)?.as_ref()?.after)
    }

    fn edit<R>(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut StagedFactRow) -> R) -> Option<R> {
        let entry = self.rows.get(Self::index(node))? as usize;
        let pair = self.entries.get_mut(entry)?.as_mut()?;
        if !pair.dirty {
            let previous_dirty_capacity = self.dirty.capacity();
            pair.dirty = true;
            self.dirty_count += 1;
            self.dirty.push((node, entry as u32));
            self.capacity_bytes = self
                .capacity_bytes
                .checked_add(
                    u64::try_from(self.dirty.capacity() - previous_dirty_capacity)
                        .expect("fact staging dirty capacity exceeds u64")
                        .checked_mul(size_of::<(StyleNodeID, u32)>() as u64)
                        .expect("fact staging dirty byte count overflow"),
                )
                .expect("fact staging byte count overflow");
        }
        let previous_payload_bytes = pair.after.capacity_bytes();
        let result = edit(&mut pair.after);
        self.capacity_bytes = self
            .capacity_bytes
            .checked_sub(previous_payload_bytes)
            .and_then(|bytes| bytes.checked_add(pair.after.capacity_bytes()))
            .expect("fact staging byte count overflow");
        Some(result)
    }

    fn insert_pair(&mut self, node: StyleNodeID, after: StagedFactRow, before: Option<PrimaryFactSnapshot>) {
        let index = Self::index(node);
        assert!(self.rows.get(index).is_none());
        let previous_storage_bytes = self.storage_capacity_bytes();
        let payload_bytes = after.capacity_bytes();
        let entry = u32::try_from(self.entries.len()).expect("fact staging entry overflow");
        self.entries.push(Some(StagedFactRowPair {
            before,
            after,
            dirty: true,
        }));
        self.rows.insert(index, entry);
        self.touched.push((node, entry));
        self.dirty.push((node, entry));
        self.live_count += 1;
        self.dirty_count += 1;
        self.capacity_bytes = self
            .capacity_bytes
            .checked_sub(previous_storage_bytes)
            .and_then(|bytes| bytes.checked_add(self.storage_capacity_bytes()))
            .and_then(|bytes| bytes.checked_add(payload_bytes))
            .expect("fact staging byte count overflow");
    }

    fn insert(&mut self, node: StyleNodeID, row: StagedFactRow, before: Option<PrimaryFactSnapshot>) {
        let index = Self::index(node);
        if let Some(entry) = self.rows.get(index) {
            let previous_storage_bytes = self.storage_capacity_bytes();
            let pair = self.entries[entry as usize]
                .as_mut()
                .expect("mapped fact staging entry must be live");
            let previous_payload_bytes = pair.after.capacity_bytes();
            let replacement_payload_bytes = row.capacity_bytes();
            pair.after = row;
            if !pair.dirty {
                pair.dirty = true;
                self.dirty_count += 1;
                self.dirty.push((node, entry));
            }
            self.capacity_bytes = self
                .capacity_bytes
                .checked_sub(previous_storage_bytes)
                .and_then(|bytes| bytes.checked_add(self.storage_capacity_bytes()))
                .and_then(|bytes| bytes.checked_sub(previous_payload_bytes))
                .and_then(|bytes| bytes.checked_add(replacement_payload_bytes))
                .expect("fact staging byte count overflow");
            return;
        }
        self.insert_pair(node, row, before);
    }

    fn remove(&mut self, node: StyleNodeID) -> Option<StagedFactRow> {
        let entry = self.rows.remove(Self::index(node))? as usize;
        let pair = self.entries.get_mut(entry)?.take()?;
        self.live_count -= 1;
        self.dirty_count -= usize::from(pair.dirty);
        self.capacity_bytes = self
            .capacity_bytes
            .checked_sub(pair.after.capacity_bytes())
            .expect("fact staging byte count underflow");
        Some(pair.after)
    }

    fn keys(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.touched
            .iter()
            .filter_map(|&(node, entry)| (self.rows.get(Self::index(node)) == Some(entry)).then_some(node))
    }

    fn dirty_rows(&self) -> impl Iterator<Item = (StyleNodeID, &StagedFactRow)> {
        self.dirty.iter().filter_map(|&(node, entry)| {
            if self.rows.get(Self::index(node)) != Some(entry) {
                return None;
            }
            let pair = self.entries.get(entry as usize)?.as_ref()?;
            pair.dirty.then_some((node, &pair.after))
        })
    }

    fn mark_applied(&mut self) {
        for &(node, entry) in &self.dirty {
            if self.rows.get(Self::index(node)) == Some(entry)
                && let Some(pair) = self.entries[entry as usize].as_mut()
            {
                pair.dirty = false;
            }
        }
        self.dirty.clear();
        self.dirty_count = 0;
    }

    fn clear(&mut self) {
        for &(node, _) in &self.touched {
            self.rows.remove(Self::index(node));
        }
        const RETAINED_ENTRY_CAPACITY_RATIO: usize = 4;
        const MIN_RETAINED_ENTRY_CAPACITY: usize = 64;
        let transaction_high_water = self.entries.len();
        if self.entries.capacity()
            > transaction_high_water
                .saturating_mul(RETAINED_ENTRY_CAPACITY_RATIO)
                .max(MIN_RETAINED_ENTRY_CAPACITY)
        {
            self.entries.shrink_to(transaction_high_water);
        }
        self.entries.clear();
        self.touched.clear();
        self.dirty.clear();
        self.live_count = 0;
        self.dirty_count = 0;
        self.capacity_bytes = self.storage_capacity_bytes();
    }

    fn capacity_bytes(&self) -> u64 {
        self.capacity_bytes
    }

    fn storage_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries, self.touched, self.dirty];
            cached [self.rows.capacity_bytes()];
            nested [];
            skip [self.live_count, self.dirty_count, self.capacity_bytes];
        }
    }

    #[cfg(test)]
    fn recomputed_capacity_bytes(&self) -> u64 {
        self.storage_capacity_bytes()
            + self
                .touched
                .iter()
                .filter_map(|&(node, entry)| {
                    if self.rows.get(Self::index(node)) != Some(entry) {
                        return None;
                    }
                    self.entries.get(entry as usize)?.as_ref()
                })
                .map(|pair| pair.after.capacity_bytes())
                .sum::<u64>()
    }
}

#[derive(Default)]
struct ElementFactMetadata {
    /// The animation names this element's computed style references, sorted.
    animation_names: Vec<StyleAtomID>,
    /// The custom properties this element declares or references, sorted.
    custom_property_set: u32,
    uses_unnamed_custom_properties: bool,
    uses_custom_functions: bool,
    /// Whether the element is a `<slot>`. A slot inside a shadow tree is not itself a flattened
    /// slottable, so `::slotted()` never names one even when its assignment carries a chain onward.
    is_slot: bool,
}

impl ElementFactMetadata {
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.animation_names];
            cached [];
            nested [];
            skip [
                self.custom_property_set,
                self.uses_unnamed_custom_properties,
                self.uses_custom_functions,
                self.is_slot,
            ];
        }
    }
}

impl Default for ElementFactStore {
    fn default() -> Self {
        let attribute_catalogs = Rc::new(AttributeCatalogs::default());
        let mut rows = StyleNodeFacts::new_primary();
        rows.attribute_catalogs = Rc::clone(&attribute_catalogs);
        let mut store = Self {
            rows: Rc::new(rows),
            attribute_catalogs,
            #[cfg(test)]
            attribute_catalog_copies: 0,
            staging: FactStaging::default(),
            metadata: Column::default(),
            primary_live_bytes: 0,
            primary_live_payload_bytes: 0,
            primary_stale_payload_bytes: 0,
            postings: FeaturePostings::default(),
            posting_rebuild_closed_at_headroom: None,
            memory: MemoryLease::new(MemoryCategory::StyleNodeMapping),
            memory_dirty: false,
            settled_non_apply_capacity_bytes: 0,
            custom_property_name_sets: super::intern_table::InternTable::default(),
            custom_property_name_set_vacancies: Vec::new(),
            custom_property_set_ids_by_name: PagedOwnedColumn::default(),
            atom_live_counts: PagedCopyColumn::default(),
            language_live_counts: PagedCopyColumn::default(),
            attribute_name_live_counts: PagedCopyColumn::default(),
            attribute_value_live_counts: PagedCopyColumn::default(),
            attribute_value_catalog_version: 1,
            custom_property_set_live_counts: vec![0],
            element_declared_properties: ElementDeclarationRows::default(),
        };
        store.settled_non_apply_capacity_bytes = store.capacity_bytes() - store.apply_capacity_bytes();
        store
    }
}

impl ElementFactStore {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    fn attribute_catalogs_mut(&mut self) -> &mut AttributeCatalogs {
        #[cfg(test)]
        if Rc::strong_count(&self.attribute_catalogs) != 1 {
            self.attribute_catalog_copies += 1;
        }
        Rc::make_mut(&mut self.attribute_catalogs)
    }

    #[cfg(test)]
    fn attribute_catalog_copies(&self) -> u64 {
        self.attribute_catalog_copies
    }

    pub(super) fn staging_is_empty(&self) -> bool {
        self.staging.is_empty()
    }

    fn sync_attribute_catalogs(&mut self) {
        if Rc::ptr_eq(&self.rows.attribute_catalogs, &self.attribute_catalogs) {
            return;
        }
        // A retained traversal may still share the old rows and their catalog snapshot.
        let rows = Rc::make_mut(&mut self.rows);
        rows.attribute_catalogs = Rc::clone(&self.attribute_catalogs);
    }

    fn increment_atom_count(counts: &mut PagedCopyColumn<u32>, atom: StyleAtomID) {
        if atom.is_none() {
            return;
        }
        let index = atom.0 as usize;
        let count = counts.get(index).unwrap_or(0);
        counts.insert(index, count.checked_add(1).expect("live fact atom count overflow"));
    }

    fn decrement_atom_count(counts: &mut PagedCopyColumn<u32>, atom: StyleAtomID) {
        if atom.is_none() {
            return;
        }
        let index = atom.0 as usize;
        let count = counts.get(index).expect("a live fact atom must have a count");
        counts.insert(index, count.checked_sub(1).expect("live fact atom count underflow"));
    }

    fn add_row_catalog_references(&mut self, facts: &StagedFactRow) {
        Self::for_each_row_atom(facts, |atom| {
            Self::increment_atom_count(&mut self.atom_live_counts, atom);
        });
        Self::increment_atom_count(&mut self.language_live_counts, facts.language);
        for &(name, value) in &facts.attributes {
            Self::increment_atom_count(&mut self.attribute_name_live_counts, name);
            Self::increment_atom_count(&mut self.attribute_value_live_counts, value);
        }
    }

    fn remove_row_catalog_references(&mut self, row: u32) {
        for atom in [
            self.rows.tag_of(row),
            self.rows.folded_tag_of(row),
            self.rows.id_of(row),
            self.rows.language_of(row),
            self.rows.namespace_of(row),
            self.rows.part_exposure_of(row),
            self.rows.directionality_of(row),
        ] {
            Self::decrement_atom_count(&mut self.atom_live_counts, atom);
        }
        for atoms in [
            self.rows.custom_states_of(row),
            self.rows.parts_of(row),
            self.rows.classes_of(row),
        ] {
            for &atom in atoms {
                Self::decrement_atom_count(&mut self.atom_live_counts, atom);
            }
        }
        Self::decrement_atom_count(&mut self.language_live_counts, self.rows.language_of(row));
        for attribute in self.rows.attributes_of(row) {
            Self::decrement_atom_count(&mut self.atom_live_counts, attribute.name);
            Self::decrement_atom_count(&mut self.atom_live_counts, attribute.value);
            Self::decrement_atom_count(&mut self.attribute_name_live_counts, attribute.name);
            Self::decrement_atom_count(&mut self.attribute_value_live_counts, attribute.value);
        }
    }

    fn for_each_row_atom(facts: &StagedFactRow, mut visit: impl FnMut(StyleAtomID)) {
        for atom in [
            facts.tag,
            facts.folded_tag,
            facts.id,
            facts.language,
            facts.namespace,
            facts.part_exposure,
            facts.directionality,
        ] {
            visit(atom);
        }
        for &atom in &facts.custom_states {
            visit(atom);
        }
        for &atom in &facts.parts {
            visit(atom);
        }
        for &atom in &facts.classes {
            visit(atom);
        }
        for &(name, value) in &facts.attributes {
            visit(name);
            visit(value);
        }
    }

    pub(super) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        for row in self.element_declared_properties.rows.iter().flatten() {
            visited += 1;
            for declaration in row.custom_declarations.iter().flatten() {
                visited += 1;
                atoms.insert(declaration.name);
            }
        }
        for (index, count) in self.atom_live_counts.indexed_iter() {
            visited += 1;
            if count != 0 {
                atoms.insert(StyleAtomID(u32::try_from(index).expect("fact atom index exceeds u32")));
            }
        }
        for (index, count) in self.attribute_name_live_counts.indexed_iter() {
            visited += 1;
            if count == 0 {
                continue;
            }
            let forms = self.attribute_name_forms(StyleAtomID(
                u32::try_from(index).expect("attribute-name atom index exceeds u32"),
            ));
            atoms.extend(
                [forms.local, forms.folded_name, forms.folded_local]
                    .into_iter()
                    .filter(|atom| !atom.is_none()),
            );
        }
        for (index, count) in self.custom_property_set_live_counts.iter().enumerate().skip(1) {
            visited += 1;
            if *count != 0 {
                atoms.extend(
                    self.custom_property_name_sets
                        [CustomPropertyNameSetID(u32::try_from(index).expect("custom property set index exceeds u32"))]
                    .iter()
                    .copied(),
                );
            }
        }
        visited
    }

    pub(super) fn extend_live_attribute_name_forms(&self, atoms: &mut HashSet<StyleAtomID>) {
        for (index, forms) in self.attribute_catalogs.name_forms.indexed_iter() {
            let name = StyleAtomID(u32::try_from(index).expect("attribute-name atom index exceeds u32"));
            if !atoms.contains(&name) {
                continue;
            }
            atoms.extend(
                [forms.local, forms.folded_name, forms.folded_local]
                    .into_iter()
                    .filter(|atom| !atom.is_none()),
            );
        }
    }

    #[must_use]
    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.rows.live_row_count()
    }

    #[must_use]
    #[cfg(test)]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    #[must_use]
    pub fn postings_mut(&mut self) -> &mut FeaturePostings {
        &mut self.postings
    }

    #[must_use]
    pub fn postings(&self) -> &FeaturePostings {
        &self.postings
    }

    fn rebuild_missing_posting(
        &self,
        rebuilt: &mut FeaturePostings,
        key: PostingKey,
        node: StyleNodeID,
        memory: &mut MemoryController,
    ) -> bool {
        !self.postings.missing.contains(&key)
            || rebuilt.insert(key, node, memory)
            || rebuilt.cardinality_limited.contains(&key)
    }

    fn build_missing_postings(&self, memory: &mut MemoryController) -> Option<FeaturePostings> {
        let mut rebuilt = FeaturePostings::new();
        rebuilt.selector_posting_limit = self.postings.selector_posting_limit;
        for node in self.rows.live_nodes() {
            let row = self.rows.row_of(node).expect("a live node must have a fact row");
            for (atom, key) in [
                (
                    self.rows.tag_of(row),
                    SelectorPostingKey::TagName(self.rows.tag_of(row)),
                ),
                (
                    self.rows.folded_tag_of(row),
                    SelectorPostingKey::TagName(self.rows.folded_tag_of(row)),
                ),
                (self.rows.id_of(row), SelectorPostingKey::Id(self.rows.id_of(row))),
                (
                    self.rows.directionality_of(row),
                    SelectorPostingKey::Directionality(self.rows.directionality_of(row)),
                ),
            ] {
                if atom.is_none() {
                    continue;
                }
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return None;
                }
            }
            for &part in self.rows.parts_of(row) {
                let key = SelectorPostingKey::Part(part);
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return None;
                }
            }
            for &state in self.rows.custom_states_of(row) {
                let key = SelectorPostingKey::CustomState(state);
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return None;
                }
            }
            for &class in self.rows.classes_of(row) {
                let key = SelectorPostingKey::Class(class);
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return None;
                }
            }
            for attribute in self.rows.attributes_of(row) {
                for name in self.attribute_name_keys(attribute.name) {
                    let key = SelectorPostingKey::AttributeName(name);
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return None;
                    }
                }
                if !attribute.value.is_none() {
                    let key = SelectorPostingKey::AttributeValue(attribute.value);
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return None;
                    }
                }
            }
            if let Some(metadata) = self.metadata_of(node) {
                for &name in &metadata.animation_names {
                    let key = DependencyPostingKey::AnimationName(name);
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return None;
                    }
                }
                if metadata.custom_property_set != 0 {
                    let key = DependencyPostingKey::CustomPropertySet(metadata.custom_property_set);
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return None;
                    }
                }
                for (uses, key) in [
                    (
                        metadata.uses_unnamed_custom_properties,
                        DependencyPostingKey::AnyCustomProperty,
                    ),
                    (metadata.uses_custom_functions, DependencyPostingKey::AnyCustomFunction),
                ] {
                    if uses && !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return None;
                    }
                }
            }
        }
        Some(rebuilt)
    }

    fn posting_rebuild_headroom(memory: &MemoryController) -> u64 {
        memory
            .tier3_limit()
            .saturating_sub(memory.bytes_in_tier(Tier::Acceleration))
    }

    fn rebuild_missing_postings(&mut self, memory: &mut MemoryController) {
        if !self.postings.is_incomplete() {
            self.posting_rebuild_closed_at_headroom = None;
            return;
        }

        let headroom = Self::posting_rebuild_headroom(memory);
        if self
            .posting_rebuild_closed_at_headroom
            .is_some_and(|closed_at| headroom <= closed_at)
        {
            return;
        }

        let rebuild_started_admitting = memory.is_tier3_admitting(MemoryCategory::FeaturePosting);
        let Some(rebuilt) = self.build_missing_postings(memory) else {
            self.posting_rebuild_closed_at_headroom =
                rebuild_started_admitting.then(|| Self::posting_rebuild_headroom(memory));
            return;
        };
        self.postings.take_rebuilt(rebuilt);
        self.posting_rebuild_closed_at_headroom = None;
    }

    #[must_use]
    pub fn primary(&self) -> &StyleNodeFacts {
        assert!(
            !self.has_dirty_staging(),
            "cannot evaluate facts while fact staging is unapplied"
        );
        &self.rows
    }

    pub(super) fn primary_view(&mut self) -> MatchingFactBatch {
        assert!(
            !self.has_dirty_staging(),
            "cannot evaluate facts while fact staging is unapplied"
        );
        self.sync_attribute_catalogs();
        MatchingFactBatch::primary_view(Rc::clone(&self.rows))
    }

    #[must_use]
    pub fn has_dirty_staging(&self) -> bool {
        self.staging.has_dirty()
    }

    #[must_use]
    pub fn has_staged_input(&self) -> bool {
        !self.staging.is_empty()
    }

    #[must_use]
    pub fn parts_of(&self, node: StyleNodeID) -> &[StyleAtomID] {
        if let Some(row) = self.staging.get(node) {
            return &row.parts;
        }
        self.rows.row_of(node).map_or(&[], |row| self.rows.parts_of(row))
    }

    #[must_use]
    pub fn custom_states_of(&self, node: StyleNodeID) -> &[StyleAtomID] {
        if let Some(row) = self.staging.get(node) {
            return &row.custom_states;
        }
        self.rows
            .row_of(node)
            .map_or(&[], |row| self.rows.custom_states_of(row))
    }

    fn metadata_mut(&mut self, node: StyleNodeID) -> &mut ElementFactMetadata {
        self.memory_dirty = true;
        let index = node.element_index().expect("only elements carry element facts") as usize;
        self.metadata
            .entry(index)
            .get_or_insert_with(ElementFactMetadata::default)
    }

    fn metadata_of(&self, node: StyleNodeID) -> Option<&ElementFactMetadata> {
        self.metadata.get(node.element_index()? as usize)?.as_ref()
    }

    fn staged_row_from_primary_snapshot(&self, snapshot: PrimaryFactSnapshot) -> StagedFactRow {
        let rare_facts = snapshot.rare_facts.unwrap_or_default();
        StagedFactRow {
            tag: snapshot.tag,
            folded_tag: snapshot.folded_tag,
            id: snapshot.id,
            language: snapshot.language,
            namespace: snapshot.namespace,
            part_exposure: rare_facts.part_exposure,
            directionality: snapshot.directionality,
            heading_level: rare_facts.heading_level,
            has_text_content: snapshot.has_text_content,
            states: snapshot.states,
            custom_states: rare_facts.custom_states.slice(&self.rows.custom_states).to_vec(),
            parts: rare_facts.parts.slice(&self.rows.parts).to_vec(),
            classes: snapshot.class_handle.slice(&self.rows.classes).to_vec(),
            attributes: snapshot
                .attribute_handle
                .slice(&self.rows.attributes)
                .iter()
                .map(|attribute| (attribute.name, attribute.value))
                .collect(),
        }
    }

    fn edit_staged_row<R>(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut StagedFactRow) -> R) -> R {
        self.memory_dirty = true;
        if !self.staging.contains(node) {
            let before = self.rows.row_of(node).map(|row| self.rows.primary_snapshot(row));
            let row = before.map_or_else(StagedFactRow::default, |snapshot| {
                self.staged_row_from_primary_snapshot(snapshot)
            });
            self.staging.insert(node, row, before);
        }
        self.staging.edit(node, edit).unwrap()
    }

    /// Give a node a row of its own without putting a feature in it.
    ///
    /// A shadow root is a node of the tree and a bearer of no features at all: it is featureless by
    /// definition, and nothing publishes anything about it. It still has to be materializable, or a
    /// selector that reads it - a relative anchor bound at the root, the leftmost step of a chain
    /// that lands there - asks for a row that never arrives and the whole match gives up.
    pub fn ensure_row(&mut self, node: StyleNodeID) {
        if self.rows.row_of(node).is_none() && !self.staging.contains(node) {
            self.memory_dirty = true;
            self.staging.insert(node, StagedFactRow::default(), None);
        }
    }

    pub fn set_tag(&mut self, node: StyleNodeID, tag: StyleAtomID, memory: &mut MemoryController) {
        let previous = self.edit_staged_row(node, |facts| std::mem::replace(&mut facts.tag, tag));
        if previous != tag {
            if !previous.is_none() {
                self.postings.remove(SelectorPostingKey::TagName(previous), node);
            }
            if !tag.is_none() {
                self.postings.insert(SelectorPostingKey::TagName(tag), node, memory);
            }
        }
    }

    /// Record the folded form of the element's name as a second name it is posted under. A folding
    /// equal to the name itself carries no information and is dropped, so the posting is never
    /// inserted twice for one element.
    pub fn set_folded_tag(&mut self, node: StyleNodeID, folded: StyleAtomID, memory: &mut MemoryController) {
        let (previous, folded) = self.edit_staged_row(node, |facts| {
            let folded = if folded == facts.tag { StyleAtomID::NONE } else { folded };
            (std::mem::replace(&mut facts.folded_tag, folded), folded)
        });
        if previous != folded {
            if !previous.is_none() {
                self.postings.remove(SelectorPostingKey::TagName(previous), node);
            }
            if !folded.is_none() {
                self.postings.insert(SelectorPostingKey::TagName(folded), node, memory);
            }
        }
    }

    pub fn set_id(&mut self, node: StyleNodeID, id: StyleAtomID, memory: &mut MemoryController) {
        let previous = self.edit_staged_row(node, |facts| std::mem::replace(&mut facts.id, id));
        if previous != id {
            if !previous.is_none() {
                self.postings.remove(SelectorPostingKey::Id(previous), node);
            }
            if !id.is_none() {
                self.postings.insert(SelectorPostingKey::Id(id), node, memory);
            }
        }
    }

    /// The interned facts an element carries, for routing an arrival that journalled them as one.
    #[must_use]
    pub fn tag_of_node(&self, node: StyleNodeID) -> StyleAtomID {
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.tag_of(row))
    }

    #[must_use]
    pub fn folded_tag_of_node(&self, node: StyleNodeID) -> StyleAtomID {
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.folded_tag_of(row))
    }

    #[must_use]
    pub fn id_of_node(&self, node: StyleNodeID) -> StyleAtomID {
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.id_of(row))
    }

    #[must_use]
    pub fn classes_of_node(&self, node: StyleNodeID) -> &[StyleAtomID] {
        self.rows.row_of(node).map_or(&[][..], |row| self.rows.classes_of(row))
    }

    /// Test a dispatch key directly against the authoritative element row when that key is local.
    ///
    /// Candidate postings are arranged for enumeration. Once a routing batch is already streaming
    /// nodes, reading the compact row avoids a second hash lookup followed by a posting membership
    /// lookup for every key it tests.
    #[must_use]
    pub fn carries_local_dispatch_key(&self, node: StyleNodeID, key: DispatchKey) -> Option<bool> {
        let row = self.rows.row_of(node);
        Some(match key {
            DispatchKey::Id(id) => row.is_some_and(|row| self.rows.id_of(row) == id),
            DispatchKey::Class(class) => row.is_some_and(|row| self.rows.classes_of(row).binary_search(&class).is_ok()),
            DispatchKey::AttributeName(name) => row.is_some_and(|row| {
                self.rows.attributes_of(row).iter().any(|attribute| {
                    let forms = self.rows.attribute_name_forms(attribute.name);
                    attribute.name == name
                        || forms.local == name
                        || forms.folded_name == name
                        || forms.folded_local == name
                })
            }),
            DispatchKey::AttributeValue(value) => row.is_some_and(|row| {
                self.rows
                    .attributes_of(row)
                    .iter()
                    .any(|attribute| attribute.value == value)
            }),
            DispatchKey::TagName(tag) => {
                row.is_some_and(|row| self.rows.tag_of(row) == tag || self.rows.folded_tag_of(row) == tag)
            }
            DispatchKey::Directionality(directionality) => {
                row.is_some_and(|row| self.rows.directionality_of(row) == directionality)
            }
            DispatchKey::Part(_)
            | DispatchKey::CustomState(_)
            | DispatchKey::Root
            | DispatchKey::State(_)
            | DispatchKey::Heading
            | DispatchKey::Universal => return None,
            _ => return None,
        })
    }

    pub fn attributes_of_node(&self, node: StyleNodeID) -> impl Iterator<Item = StyleAtomID> {
        self.rows
            .row_of(node)
            .map_or(&[][..], |row| self.rows.attributes_of(row))
            .iter()
            .map(|attribute| attribute.name)
    }

    #[must_use]
    pub fn states_of_node(&self, node: StyleNodeID) -> StateSet {
        if let Some(row) = self.staging.get(node) {
            return row.states;
        }
        self.rows
            .row_of(node)
            .map_or(StateSet::default(), |row| self.rows.states_of(row))
    }

    #[must_use]
    pub fn part_exposure_of(&self, node: StyleNodeID) -> StyleAtomID {
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.part_exposure_of(row))
    }

    pub fn set_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        self.edit_staged_row(node, |facts| facts.part_exposure = exposure);
    }

    #[must_use]
    pub fn language_of(&self, node: StyleNodeID) -> StyleAtomID {
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.language_of(row))
    }

    #[must_use]
    pub fn namespace_of(&self, node: StyleNodeID) -> StyleAtomID {
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.namespace_of(row))
    }

    #[must_use]
    pub fn directionality_of(&self, node: StyleNodeID) -> StyleAtomID {
        if let Some(row) = self.staging.get(node) {
            return row.directionality;
        }
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.directionality_of(row))
    }

    pub fn set_has_text_content(&mut self, node: StyleNodeID, has_text_content: bool) {
        self.edit_staged_row(node, |facts| facts.has_text_content = has_text_content);
    }

    /// An element's heading level follows from what it is, so it is published as it arrives and
    /// whenever an `aria-level` that overrides it moves.
    pub fn set_is_slot(&mut self, node: StyleNodeID, is_slot: bool) {
        self.memory_dirty = true;
        if is_slot {
            self.metadata_mut(node).is_slot = true;
        } else if let Some(metadata) = node
            .element_index()
            .and_then(|index| self.metadata.get_mut(index as usize))
            .and_then(Option::as_mut)
        {
            metadata.is_slot = false;
        }
    }

    #[must_use]
    pub fn is_slot(&self, node: StyleNodeID) -> bool {
        self.metadata_of(node).is_some_and(|metadata| metadata.is_slot)
    }

    pub fn set_heading_level(&mut self, node: StyleNodeID, level: u8) {
        self.edit_staged_row(node, |facts| facts.heading_level = level);
    }

    #[must_use]
    pub fn heading_level_of(&self, node: StyleNodeID) -> u8 {
        self.rows.row_of(node).map_or(0, |row| self.rows.heading_level_of(row))
    }

    /// Record what one language atom spells, so `:lang()` can compare ranges against it.
    pub fn set_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        let index = language.0 as usize;
        if language.is_none()
            || self
                .attribute_catalogs
                .language_texts
                .get(index)
                .is_some_and(Option::is_some)
        {
            return;
        }
        self.memory_dirty = true;
        self.attribute_catalogs_mut()
            .language_texts
            .insert(index, Some(text.into()));
    }

    pub fn set_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        self.edit_staged_row(node, |facts| facts.language = language);
    }

    /// An element's namespace is fixed when it is created, so this is published once, on arrival,
    /// and is never an input that moves.
    pub fn set_namespace(&mut self, node: StyleNodeID, namespace: StyleAtomID) {
        self.edit_staged_row(node, |facts| facts.namespace = namespace);
    }

    pub fn set_directionality(
        &mut self,
        node: StyleNodeID,
        directionality: StyleAtomID,
        memory: &mut MemoryController,
    ) {
        let previous = self.edit_staged_row(node, |facts| {
            std::mem::replace(&mut facts.directionality, directionality)
        });
        if previous != directionality {
            if !previous.is_none() {
                self.postings.remove(SelectorPostingKey::Directionality(previous), node);
            }
            if !directionality.is_none() {
                self.postings
                    .insert(SelectorPostingKey::Directionality(directionality), node, memory);
            }
        }
    }

    pub fn set_class(&mut self, node: StyleNodeID, class: StyleAtomID, present: bool, memory: &mut MemoryController) {
        let changed = self.edit_staged_row(node, |facts| match (present, facts.classes.binary_search(&class)) {
            (true, Err(index)) => {
                facts.classes.insert(index, class);
                true
            }
            (false, Ok(index)) => {
                facts.classes.remove(index);
                true
            }
            _ => false,
        });
        if changed {
            if present {
                self.postings.insert(SelectorPostingKey::Class(class), node, memory);
            } else {
                self.postings.remove(SelectorPostingKey::Class(class), node);
            }
        }
    }

    /// Replace the set of animation names an element references.
    ///
    /// This arrives from computed style rather than from the DOM, so it is published as a set rather
    /// than one name at a time: an element's `animation-name` is recomputed whole.
    pub fn set_animation_names(&mut self, node: StyleNodeID, names: &[StyleAtomID], memory: &mut MemoryController) {
        let previous = self
            .metadata_of(node)
            .map_or(&[][..], |metadata| &metadata.animation_names);
        if previous == names {
            return;
        }
        let mut sorted: Vec<StyleAtomID> = names.to_vec();
        sorted.sort_unstable_by_key(|name| name.0);
        sorted.dedup();
        if previous == sorted {
            return;
        }
        let previous = std::mem::take(&mut self.metadata_mut(node).animation_names);
        for entry in merge_sorted_by(&previous, &sorted, |left, right| left.0.cmp(&right.0)) {
            match entry {
                SortedMergeEntry::Left(&name) => {
                    self.postings.remove(DependencyPostingKey::AnimationName(name), node);
                    Self::decrement_atom_count(&mut self.atom_live_counts, name);
                }
                SortedMergeEntry::Right(&name) => {
                    self.postings
                        .insert(DependencyPostingKey::AnimationName(name), node, memory);
                    Self::increment_atom_count(&mut self.atom_live_counts, name);
                }
                SortedMergeEntry::Both(..) => {}
            }
        }
        self.metadata_mut(node).animation_names = sorted;
    }

    /// Whether this element's style resolution called a custom function.
    pub fn set_uses_custom_functions(&mut self, node: StyleNodeID, uses: bool, memory: &mut MemoryController) {
        if self
            .metadata_of(node)
            .is_some_and(|metadata| metadata.uses_custom_functions)
            == uses
        {
            return;
        }
        self.metadata_mut(node).uses_custom_functions = uses;
        match uses {
            true => self
                .postings
                .insert(DependencyPostingKey::AnyCustomFunction, node, memory),
            false => {
                self.postings.remove(DependencyPostingKey::AnyCustomFunction, node);
                true
            }
        };
    }

    /// Whether this element used custom properties whose names could not all be enumerated.
    pub fn uses_unnamed_custom_properties(&self, node: StyleNodeID) -> bool {
        self.metadata_of(node)
            .is_some_and(|metadata| metadata.uses_unnamed_custom_properties)
    }

    pub fn set_uses_unnamed_custom_properties(&mut self, node: StyleNodeID, uses: bool, memory: &mut MemoryController) {
        if self
            .metadata_of(node)
            .is_some_and(|metadata| metadata.uses_unnamed_custom_properties)
            == uses
        {
            return;
        }
        self.metadata_mut(node).uses_unnamed_custom_properties = uses;
        match uses {
            true => self
                .postings
                .insert(DependencyPostingKey::AnyCustomProperty, node, memory),
            false => {
                self.postings.remove(DependencyPostingKey::AnyCustomProperty, node);
                true
            }
        };
    }

    /// Replace the set of custom properties an element declares or references.
    /// Record which custom properties one element's own cascade declares.
    ///
    /// The names are held as an interned *set*: a page whose theme declares two thousand names
    /// hands the same two thousand to every element it decides for, and one posting entry per name
    /// per element is millions of entries for one answer. The set is stored once, the element
    /// names it by identity, and a registration asks which sets hold its name.
    pub fn set_custom_property_names(
        &mut self,
        node: StyleNodeID,
        names: &[StyleAtomID],
        memory: &mut MemoryController,
    ) {
        self.memory_dirty = true;
        // The caller hands these sorted and deduplicated, worked out once for the environment the
        // element resolved to.
        debug_assert!(names.windows(2).all(|pair| pair[0].0 < pair[1].0));
        let set = self.intern_custom_property_name_set(names);
        let previous = self
            .metadata_of(node)
            .map_or(0, |metadata| metadata.custom_property_set);
        if previous == set {
            return;
        }
        let required_counts = usize::try_from(previous.max(set)).expect("custom property set index overflow") + 1;
        if self.custom_property_set_live_counts.len() < required_counts {
            self.custom_property_set_live_counts.resize(required_counts, 0);
        }
        if previous != 0 {
            self.custom_property_set_live_counts[previous as usize] = self.custom_property_set_live_counts
                [previous as usize]
                .checked_sub(1)
                .expect("custom property set live count underflow");
        }
        if set != 0 {
            self.custom_property_set_live_counts[set as usize] = self.custom_property_set_live_counts[set as usize]
                .checked_add(1)
                .expect("custom property set live count overflow");
        }
        self.metadata_mut(node).custom_property_set = set;
        if previous != 0 {
            self.postings
                .remove(DependencyPostingKey::CustomPropertySet(previous), node);
        }
        if set != 0 {
            self.postings
                .insert(DependencyPostingKey::CustomPropertySet(set), node, memory);
        }
    }

    /// One identity per distinct set of declared names. Zero is the empty set and is never stored.
    fn intern_custom_property_name_set(&mut self, names: &[StyleAtomID]) -> u32 {
        if names.is_empty() {
            return 0;
        }
        let hash = super::intern_table::content_hash(names);
        if let Some(candidate) = self
            .custom_property_name_sets
            .find(hash, |_identity, candidate| candidate.as_ref() == names)
        {
            return candidate.0;
        }
        let id = self.custom_property_name_set_vacancies.pop().unwrap_or_else(|| {
            u32::try_from(self.custom_property_name_sets.len() + 1).expect("custom property name set space exhausted")
        });
        self.custom_property_name_sets
            .insert(hash, CustomPropertyNameSetID(id), names.into());
        for name in names {
            self.custom_property_set_ids_by_name.entry(name.0 as usize).push(id);
        }
        id
    }

    /// The elements whose own cascade declares `name`, which is every element in any set holding it.
    pub fn custom_property_candidates(&self, name: StyleAtomID) -> Result<Vec<StyleNodeID>, PostingKey> {
        let Some(sets) = self.custom_property_set_ids_by_name.get(name.0 as usize) else {
            return Ok(Vec::new());
        };
        let mut nodes = Vec::new();
        for &set in sets {
            match self.postings.lookup(DependencyPostingKey::CustomPropertySet(set)) {
                Lookup::Known(posting) => posting.append_candidates_to(&mut nodes),
                Lookup::KnownAbsent => {}
                Lookup::Missing(gap) => return Err(gap),
            }
        }
        Ok(nodes)
    }

    pub fn set_attribute(
        &mut self,
        node: StyleNodeID,
        name: StyleAtomID,
        value: StyleAtomID,
        present: bool,
        memory: &mut MemoryController,
    ) {
        // An attribute is one fact under one key, but it is indexed under every name a selector can
        // reach it by: its own, the any-namespace form `[*|x]` dispatches under, and the ASCII
        // foldings of both, which is how `[aB]` reaches an attribute written either way. The extra
        // names are postings rather than facts, so they say only that at least one attribute of the
        // element answers to them.
        let (old_value, changed) = self.edit_staged_row(node, |facts| {
            let found = facts.attributes.binary_search_by_key(&name, |entry| entry.0);
            match (present, found) {
                (true, Ok(index)) => (Some(std::mem::replace(&mut facts.attributes[index].1, value)), false),
                (true, Err(index)) => {
                    facts.attributes.insert(index, (name, value));
                    (None, true)
                }
                (false, Ok(index)) => (Some(facts.attributes.remove(index).1), true),
                (false, Err(_)) => (None, false),
            }
        });
        if changed && present {
            for key in self.attribute_name_keys(name) {
                self.postings
                    .insert(SelectorPostingKey::AttributeName(key), node, memory);
            }
        } else if changed {
            // A shared name stays true of the element while another of its attributes still
            // answers to it, so only the names nothing implies any more are dropped.
            for key in self.attribute_name_keys(name) {
                if key == name || !self.node_answers_to_attribute_name(node, key) {
                    self.postings.remove(SelectorPostingKey::AttributeName(key), node);
                }
            }
        }
        if !present || old_value != Some(value) {
            if let Some(old_value) = old_value {
                let old_value_is_still_present = self
                    .staging
                    .get(node)
                    .is_some_and(|facts| facts.attributes.iter().any(|&(_, candidate)| candidate == old_value));
                if !old_value.is_none() && !old_value_is_still_present {
                    self.postings
                        .remove(SelectorPostingKey::AttributeValue(old_value), node);
                }
            }
            if present && !value.is_none() {
                self.postings
                    .insert(SelectorPostingKey::AttributeValue(value), node, memory);
            }
        }
    }

    pub(super) fn matching_attribute_values(&self, test: AttributeTest, literal: &[u16]) -> Vec<StyleAtomID> {
        if test.operator == AttributeOperator::Exact
            && test.case == AttributeCase::Sensitive
            && !test.value_atom.is_none()
        {
            return vec![test.value_atom];
        }

        let insensitive = test.case != AttributeCase::Sensitive;
        let mut values = Vec::new();
        for (index, text) in self.attribute_catalogs.value_texts.indexed_iter() {
            let Some(text) = text else {
                continue;
            };
            if !attribute_value_may_match(text, literal, test.operator, insensitive) {
                continue;
            }
            let value = StyleAtomID(u32::try_from(index).expect("attribute-value atom index exceeds u32"));
            values.push(value);
        }
        values
    }

    pub(super) fn attribute_value_candidates(&self, values: &[StyleAtomID]) -> Option<Vec<StyleNodeID>> {
        let mut candidates = Vec::new();
        for &value in values {
            match self.postings.lookup(SelectorPostingKey::AttributeValue(value)) {
                Lookup::Known(posting) => posting.append_candidates_to(&mut candidates),
                Lookup::KnownAbsent => {}
                Lookup::Missing(_) => return None,
            }
        }
        Some(candidates)
    }

    #[must_use]
    pub(super) fn attribute_value_catalog_version(&self) -> u64 {
        self.attribute_value_catalog_version
    }

    /// Whether any attribute the node still carries is indexed under `key`.
    #[must_use]
    fn node_answers_to_attribute_name(&self, node: StyleNodeID, key: StyleAtomID) -> bool {
        self.staging.get(node).is_some_and(|facts| {
            facts
                .attributes
                .iter()
                .any(|entry| self.attribute_name_keys(entry.0).any(|candidate| candidate == key))
        })
    }

    /// Record which longhand properties one of an element's own declarations covers.
    pub fn set_element_declared_properties(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declared: Vec<DeclaredProperty>,
        written_values: Vec<RetainedStyleValueData>,
        declarations_are_complete: bool,
    ) {
        self.memory_dirty = true;
        self.element_declared_properties
            .set(node, kind, declared, written_values, declarations_are_complete);
    }

    /// Record the custom properties the node's inline style declares, with the values they were
    /// written with.
    pub fn set_element_custom_declarations(
        &mut self,
        node: StyleNodeID,
        custom_declarations: Vec<CustomDeclaration>,
        custom_written_values: Vec<RetainedStyleValueData>,
    ) {
        self.memory_dirty = true;
        self.element_declared_properties
            .set_custom(node, custom_declarations, custom_written_values);
    }

    /// The custom properties the node's inline style declares, in declaration order.
    #[must_use]
    pub fn element_custom_declarations(&self, node: StyleNodeID) -> &[CustomDeclaration] {
        self.element_declared_properties.get_custom(node)
    }

    /// The values the node's inline custom declarations were written with, parallel to them, or
    /// nothing when their publication carried none.
    #[must_use]
    pub fn element_custom_written_values(&self, node: StyleNodeID) -> &[RetainedStyleValueData] {
        self.element_declared_properties.get_custom_written(node)
    }

    /// Whether any inline style declares a custom property.
    #[must_use]
    pub fn any_element_declares_custom_properties(&self) -> bool {
        self.element_declared_properties.rows_with_custom_declarations != 0
    }

    /// Whether one kind of the node's element declarations is complete once the custom properties
    /// the inline style declares are set aside.
    #[must_use]
    pub fn element_declarations_are_complete_but_for_custom_properties(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
    ) -> bool {
        self.element_declared_properties.complete_but_for_custom(node, kind)
    }

    /// The values one kind of the node's element declarations were written with, parallel to
    /// `element_declared_properties`, or nothing when their publication carried none.
    #[must_use]
    pub fn element_written_declared_values(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
    ) -> &[RetainedStyleValueData] {
        self.element_declared_properties.get_written(node, kind)
    }

    #[must_use]
    pub fn element_declared_properties(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
    ) -> (&[DeclaredProperty], bool) {
        self.element_declared_properties.get(node, kind)
    }

    /// Record what one attribute-value atom spells, so a value operator can test it.
    ///
    /// Values repeat heavily across a document, so the text is held once per distinct value.
    /// Record that `name` is an attribute name whose any-namespace form is `local`.
    ///
    /// Published where the two atoms are minted, which is the only place that knows the pair. It is
    /// idempotent and total: every attribute name goes through it, including one in no namespace,
    /// whose local form is still an atom of its own.
    pub fn note_attribute_name_forms(&mut self, name: StyleAtomID, forms: AttributeNameForms) {
        self.memory_dirty = true;
        self.attribute_catalogs_mut().name_forms.insert(name.0 as usize, forms);
    }

    /// The other names an attribute name answers to, all `NONE` if the name has not been published.
    #[must_use]
    pub fn attribute_name_forms(&self, name: StyleAtomID) -> AttributeNameForms {
        self.attribute_catalogs
            .name_forms
            .get(name.0 as usize)
            .unwrap_or_default()
    }

    /// Every atom an attribute of this name is indexed under, without repeats.
    pub fn attribute_name_keys(&self, name: StyleAtomID) -> impl Iterator<Item = StyleAtomID> + use<> {
        let forms = self.attribute_name_forms(name);
        let keys = [name, forms.local, forms.folded_name, forms.folded_local];
        keys.into_iter()
            .enumerate()
            .filter_map(move |(index, key)| (!key.is_none() && !keys[..index].contains(&key)).then_some(key))
    }

    pub fn set_attribute_value_text(&mut self, value: StyleAtomID, text: &[u16]) {
        let index = value.0 as usize;
        if value.is_none()
            || self
                .attribute_catalogs
                .value_texts
                .get(index)
                .is_some_and(Option::is_some)
        {
            return;
        }
        self.memory_dirty = true;
        self.attribute_catalogs_mut()
            .value_texts
            .insert(index, Some(text.into()));
        self.attribute_value_catalog_version = self
            .attribute_value_catalog_version
            .checked_add(1)
            .expect("attribute-value catalog version overflow");
    }

    #[must_use]
    pub fn has_attribute_value_text(&self, value: StyleAtomID) -> bool {
        self.attribute_catalogs
            .value_texts
            .get(value.0 as usize)
            .is_some_and(Option::is_some)
    }

    pub fn set_state(&mut self, node: StyleNodeID, fact: StateFact, value: bool) {
        self.edit_staged_row(node, |facts| {
            if value {
                facts.states.insert(fact);
            } else {
                facts.states.remove(fact);
            }
        });
    }

    pub fn set_parts(&mut self, node: StyleNodeID, parts: &[StyleAtomID], memory: &mut MemoryController) {
        let previous = self.staging.get(node).map_or_else(
            || self.rows.row_of(node).map_or(&[][..], |row| self.rows.parts_of(row)),
            |facts| facts.parts.as_slice(),
        );
        for &part in previous.iter().filter(|part| !parts.contains(part)) {
            self.postings.remove(SelectorPostingKey::Part(part), node);
        }
        for &part in parts.iter().filter(|part| !previous.contains(part)) {
            self.postings.insert(SelectorPostingKey::Part(part), node, memory);
        }
        self.edit_staged_row(node, |facts| parts.clone_into(&mut facts.parts));
    }

    pub fn set_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID], memory: &mut MemoryController) {
        let previous = self.staging.get(node).map_or_else(
            || {
                self.rows
                    .row_of(node)
                    .map_or(&[][..], |row| self.rows.custom_states_of(row))
            },
            |facts| facts.custom_states.as_slice(),
        );
        for &state in previous.iter().filter(|state| !states.contains(state)) {
            self.postings.remove(SelectorPostingKey::CustomState(state), node);
        }
        for &state in states.iter().filter(|state| !previous.contains(state)) {
            self.postings
                .insert(SelectorPostingKey::CustomState(state), node, memory);
        }
        self.edit_staged_row(node, |facts| states.clone_into(&mut facts.custom_states));
    }

    pub fn forget(&mut self, node: StyleNodeID) {
        self.memory_dirty = true;
        self.element_declared_properties.remove(node);
        self.staging.remove(node);
        let Some(row) = self.rows.row_of(node) else {
            return;
        };
        let row_bytes = self.rows.logical_bytes_of_row(row);
        let payload_bytes = self.rows.payload_bytes_of_row(row);
        self.remove_row_catalog_references(row);
        let tag = self.rows.tag_of(row);
        if !tag.is_none() {
            self.postings.remove(SelectorPostingKey::TagName(tag), node);
        }
        let folded_tag = self.rows.folded_tag_of(row);
        if !folded_tag.is_none() {
            self.postings.remove(SelectorPostingKey::TagName(folded_tag), node);
        }
        let id = self.rows.id_of(row);
        if !id.is_none() {
            self.postings.remove(SelectorPostingKey::Id(id), node);
        }
        let directionality = self.rows.directionality_of(row);
        if !directionality.is_none() {
            self.postings
                .remove(SelectorPostingKey::Directionality(directionality), node);
        }
        for &class in self.rows.classes_of(row) {
            self.postings.remove(SelectorPostingKey::Class(class), node);
        }
        for &part in self.rows.parts_of(row) {
            self.postings.remove(SelectorPostingKey::Part(part), node);
        }
        for &state in self.rows.custom_states_of(row) {
            self.postings.remove(SelectorPostingKey::CustomState(state), node);
        }
        for &AttributeFact { name, value, .. } in self.rows.attributes_of(row) {
            for key in self.attribute_name_keys(name) {
                self.postings.remove(SelectorPostingKey::AttributeName(key), node);
            }
            if !value.is_none() {
                self.postings.remove(SelectorPostingKey::AttributeValue(value), node);
            }
        }
        Rc::get_mut(&mut self.rows)
            .expect("forgetting a fact row requires unique primary rows")
            .forget_row(node);
        self.primary_live_bytes = self
            .primary_live_bytes
            .checked_sub(row_bytes)
            .expect("primary live fact byte count underflow");
        self.primary_live_payload_bytes = self
            .primary_live_payload_bytes
            .checked_sub(payload_bytes)
            .expect("primary live fact payload byte count underflow");
        self.primary_stale_payload_bytes = self
            .primary_stale_payload_bytes
            .checked_add(payload_bytes)
            .expect("primary stale fact payload byte count overflow");
        if let Some(metadata) = node
            .element_index()
            .and_then(|index| self.metadata.get_mut(index as usize))
            .and_then(Option::take)
        {
            if metadata.custom_property_set != 0 {
                self.custom_property_set_live_counts[metadata.custom_property_set as usize] = self
                    .custom_property_set_live_counts[metadata.custom_property_set as usize]
                    .checked_sub(1)
                    .expect("custom property set live count underflow");
            }
            for name in metadata.animation_names {
                self.postings.remove(DependencyPostingKey::AnimationName(name), node);
                Self::decrement_atom_count(&mut self.atom_live_counts, name);
            }
            if metadata.custom_property_set != 0 {
                self.postings.remove(
                    DependencyPostingKey::CustomPropertySet(metadata.custom_property_set),
                    node,
                );
            }
            if metadata.uses_unnamed_custom_properties {
                self.postings.remove(DependencyPostingKey::AnyCustomProperty, node);
            }
            if metadata.uses_custom_functions {
                self.postings.remove(DependencyPostingKey::AnyCustomFunction, node);
            }
        }
    }

    /// Whether a borrowed primary view (an active or prepared traversal) shares the fact rows.
    #[cfg(test)]
    pub(super) fn primary_rows_are_shared(&self) -> bool {
        Rc::strong_count(&self.rows) != 1
    }

    pub(super) fn sweep_auxiliary_catalogs_without_sync(&mut self) {
        assert_eq!(
            Rc::strong_count(&self.rows),
            1,
            "auxiliary catalog sweeping requires unique primary rows"
        );
        self.memory_dirty = true;
        let attribute_catalogs = Rc::make_mut(&mut self.attribute_catalogs);
        // Language spellings and attribute-name forms are retained until their atom is reclaimed;
        // forget_atoms clears them at that authoritative boundary so a reused identity can publish
        // different text. Attribute values can be dropped earlier when their last fact leaves.
        for (index, text) in attribute_catalogs.value_texts.indexed_iter_mut() {
            if self.attribute_value_live_counts.get(index).unwrap_or(0) == 0 {
                *text = None;
            }
        }

        for sets in self.custom_property_set_ids_by_name.iter_mut() {
            *sets = Vec::new();
        }
        self.custom_property_name_set_vacancies.clear();
        for id in 1..=self.custom_property_name_sets.len() {
            if self.custom_property_set_live_counts.get(id).copied().unwrap_or(0) == 0 {
                let identity = CustomPropertyNameSetID(id as u32);
                if !self.custom_property_name_sets[identity].is_empty() {
                    let hash = super::intern_table::content_hash(&self.custom_property_name_sets[identity]);
                    self.custom_property_name_sets.remove_identity(hash, identity);
                    self.custom_property_name_sets[identity] = Box::default();
                }
                self.custom_property_name_set_vacancies.push(id as u32);
                continue;
            }
            let names = &self.custom_property_name_sets[CustomPropertyNameSetID(id as u32)];
            for name in names {
                self.custom_property_set_ids_by_name
                    .entry(name.0 as usize)
                    .push(id as u32);
            }
        }
    }

    pub fn sweep_auxiliary_catalogs(&mut self) {
        self.sweep_auxiliary_catalogs_without_sync();
        self.sync_attribute_catalogs();
    }

    /// Remove every derived row keyed by an identity before that identity can be reused.
    pub(super) fn forget_atoms(&mut self, atoms: &[StyleAtomID]) {
        if atoms.is_empty() {
            return;
        }
        self.memory_dirty = true;
        let atoms = atoms.iter().copied().collect::<HashSet<_>>();
        let catalogs = Rc::make_mut(&mut self.attribute_catalogs);
        for atom in &atoms {
            let index = atom.0 as usize;
            if catalogs.name_forms.get(index).is_some() {
                catalogs.name_forms.insert(index, AttributeNameForms::default());
            }
            if let Some(text) = catalogs.value_texts.get_mut(index) {
                *text = None;
            }
            if let Some(text) = catalogs.language_texts.get_mut(index) {
                *text = None;
            }
            if let Some(sets) = self.custom_property_set_ids_by_name.get_mut(index) {
                sets.clear();
            }
        }
        for (_, forms) in catalogs.name_forms.indexed_iter() {
            assert!(
                ![forms.local, forms.folded_name, forms.folded_local]
                    .into_iter()
                    .any(|atom| atoms.contains(&atom)),
                "a live attribute name must keep all of its derived forms live"
            );
        }
        self.postings.forget_atoms(&atoms);
        self.sync_attribute_catalogs();
    }

    #[must_use]
    #[cfg(test)]
    pub fn covers(&self, node: StyleNodeID) -> bool {
        self.rows.row_of(node).is_some() || self.staging.contains(node)
    }

    /// Pack the facts of a bounded set of style nodes into one batch.
    ///
    /// A node the store has never heard a fact for is skipped rather than given defaults, so the
    /// evaluator reports a miss and the caller widens instead of matching against a fiction.
    ///
    /// Every column an operator can read is filled, not only the ones a compound tests: a row whose
    /// parameters were left at their defaults answers `:dir()` and `:state()` as though the element
    /// held neither, which is a wrong answer rather than a missing one.
    pub fn materialize(&mut self, nodes: impl Iterator<Item = StyleNodeID>, batch: &mut StyleNodeFacts) {
        self.sync_attribute_catalogs();
        batch.clear();
        batch.attribute_catalogs = Rc::clone(&self.attribute_catalogs);
        for node in nodes {
            self.materialize_row(node, batch);
        }
    }

    /// Append the rows of `nodes` that the batch does not hold yet, keeping existing rows.
    ///
    /// One shared batch can serve a whole pass this way: consecutive asks overwhelmingly share
    /// their ancestor chains, and each row is packed at most once per pass instead of once per
    /// ask.
    pub fn materialize_missing(&mut self, nodes: impl Iterator<Item = StyleNodeID>, batch: &mut StyleNodeFacts) {
        self.sync_attribute_catalogs();
        batch.attribute_catalogs = Rc::clone(&self.attribute_catalogs);
        for node in nodes {
            if batch.row_of(node).is_some() {
                continue;
            }
            self.materialize_row(node, batch);
        }
    }

    fn materialize_row(&self, node: StyleNodeID, batch: &mut StyleNodeFacts) {
        let Some(row) = self.rows.row_of(node) else {
            return;
        };
        batch.push_row_from(node, &self.rows, row);
    }

    fn auxiliary_capacity_bytes(&self) -> u64 {
        let metadata_payloads = self
            .metadata
            .iter()
            .flatten()
            .map(ElementFactMetadata::capacity_bytes)
            .sum::<u64>();
        let custom_property_name_payloads = self
            .custom_property_name_sets
            .iter()
            .map(|names| size_of_val(names.as_ref()))
            .sum::<usize>();
        let custom_property_name_index_payloads = self
            .custom_property_set_ids_by_name
            .iter()
            .map(|ids| ids.capacity() * size_of::<u32>())
            .sum::<usize>();
        let language_payloads = self
            .attribute_catalogs
            .language_texts
            .iter()
            .flatten()
            .map(|text| text.len() * size_of::<u16>())
            .sum::<usize>();
        let attribute_value_payloads = self
            .attribute_catalogs
            .value_texts
            .iter()
            .flatten()
            .map(|text| text.len() * size_of::<u16>())
            .sum::<usize>();

        capacity_bytes! {
            shallow [
                self.custom_property_name_sets,
                self.custom_property_name_set_vacancies,
                self.custom_property_set_ids_by_name,
                self.atom_live_counts,
                self.language_live_counts,
                self.attribute_name_live_counts,
                self.attribute_value_live_counts,
                self.custom_property_set_live_counts,
                self.attribute_catalogs.language_texts,
                self.attribute_catalogs.value_texts,
                self.attribute_catalogs.name_forms,
            ];
            cached [];
            nested [
                self.staging.capacity_bytes(),
                metadata_payloads,
                custom_property_name_payloads,
                custom_property_name_index_payloads,
                language_payloads,
                attribute_value_payloads,
            ];
            skip [];
        }
    }

    fn apply_capacity_bytes(&self) -> u64 {
        self.rows.capacity_bytes() + self.staging.capacity_bytes() + self.element_declared_properties.capacity_bytes()
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.metadata];
            cached [];
            nested [
                self.rows.capacity_bytes(),
                self.auxiliary_capacity_bytes(),
                self.element_declared_properties.capacity_bytes(),
            ];
            skip [
                self.primary_live_bytes,
                self.primary_live_payload_bytes,
                self.primary_stale_payload_bytes,
                self.postings,
                self.posting_rebuild_closed_at_headroom,
                self.attribute_catalogs,
                self.memory,
                self.memory_dirty,
                self.staging,
                self.custom_property_name_sets,
                self.custom_property_name_set_vacancies,
                self.custom_property_set_ids_by_name,
                self.atom_live_counts,
                self.language_live_counts,
                self.attribute_name_live_counts,
                self.attribute_value_live_counts,
                self.custom_property_set_live_counts,
            ];
        }
    }

    /// Commit every fact-staging edit to the authoritative arrangement.
    pub fn apply_staged(&mut self, memory: &mut MemoryController) {
        if !self.memory_dirty {
            self.rebuild_missing_postings(memory);
            return;
        }
        self.sync_attribute_catalogs();
        let mut staging = std::mem::take(&mut self.staging);
        for (node, facts) in staging.dirty_rows() {
            let previous = self.rows.row_of(node);
            let replaced_bytes = previous.map_or(0, |row| self.rows.logical_bytes_of_row(row));
            let replaced_payload_bytes = previous.map_or(0, |row| self.rows.payload_bytes_of_row(row));
            if let Some(previous) = previous {
                self.remove_row_catalog_references(previous);
            }
            self.add_row_catalog_references(facts);
            // A selector-free transaction may retain the active traversal's immutable primary
            // view. Preserve that view while advancing the authoritative rows for the next
            // transaction.
            let stale_payload_bytes = Rc::make_mut(&mut self.rows).set_primary_row(node, facts);
            let row = self.rows.row_of(node).unwrap();
            let replacement_bytes = self.rows.logical_bytes_of_row(row);
            let replacement_payload_bytes = self.rows.payload_bytes_of_row(row);
            self.primary_live_bytes = self
                .primary_live_bytes
                .checked_sub(replaced_bytes)
                .and_then(|bytes| bytes.checked_add(replacement_bytes))
                .expect("primary live fact byte count overflow");
            self.primary_live_payload_bytes = self
                .primary_live_payload_bytes
                .checked_sub(replaced_payload_bytes)
                .and_then(|bytes| bytes.checked_add(replacement_payload_bytes))
                .expect("primary live fact payload byte count overflow");
            self.primary_stale_payload_bytes = self
                .primary_stale_payload_bytes
                .checked_add(stale_payload_bytes)
                .expect("primary stale fact payload byte count overflow");
        }
        staging.mark_applied();
        self.staging = staging;
        self.postings.update_selector_posting_limit(self.rows.live_row_count());
        let apply_capacity_bytes = self.apply_capacity_bytes();
        let current = self
            .settled_non_apply_capacity_bytes
            .checked_add(apply_capacity_bytes)
            .expect("element fact byte count overflow");
        self.memory.resize_required_to(memory, current);
        self.memory_dirty = false;
        self.rebuild_missing_postings(memory);
    }

    pub fn prepare_selector_query(&mut self, memory: &mut MemoryController) {
        self.apply_staged(memory);
    }

    /// Snapshot the committed rows which staged local facts will replace at the barrier.
    #[must_use]
    pub fn staged_before_facts(&mut self) -> StyleNodeFacts {
        self.sync_attribute_catalogs();
        let mut nodes = Vec::with_capacity(self.staging.len());
        for node in self.staging.keys() {
            nodes.push(node);
        }
        nodes.sort_unstable();
        let mut before = StyleNodeFacts::new();
        before.attribute_catalogs = Rc::clone(&self.attribute_catalogs);
        for node in nodes {
            let pair = self
                .staging
                .rows
                .get(FactStaging::index(node))
                .and_then(|entry| self.staging.entries[entry as usize].as_ref())
                .expect("fact staging node must have a staged row");
            if let Some(snapshot) = pair.before {
                before.push_row_from_primary_snapshot(node, &self.rows, snapshot);
            } else {
                before.push_row(
                    node,
                    StyleAtomID::NONE,
                    StyleAtomID::NONE,
                    StateSet::default(),
                    &[],
                    &[],
                );
            }
        }
        before
    }

    pub fn release_staging(&mut self, memory: &mut MemoryController) {
        self.staging.clear();
        if self.primary_stale_payload_bytes > self.primary_live_payload_bytes {
            Rc::get_mut(&mut self.rows)
                .expect("compacting fact payloads requires unique primary rows")
                .compact_primary_payloads();
            self.primary_stale_payload_bytes = 0;
        }
        let current = self.capacity_bytes();
        self.memory.resize_required_to(memory, current);
        self.settled_non_apply_capacity_bytes = current - self.apply_capacity_bytes();
    }
}

#[cfg(test)]
mod tests {
    use super::super::memory::DeviceClass;
    use super::super::tree::StyleNodeTree;
    use super::*;

    fn known_posting(postings: &FeaturePostings, key: PostingKey) -> &Posting {
        match postings.lookup(key) {
            Lookup::Known(posting) => posting,
            Lookup::KnownAbsent | Lookup::Missing(_) => panic!("expected a retained posting"),
        }
    }

    #[test]
    fn a_posting_stays_sorted_across_chunk_splits() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let key = SelectorPostingKey::Class(StyleAtomID(1));

        // Insert in order and across enough members to force several append-only chunks.
        for index in 1..2000_u32 {
            assert!(postings.insert(key, StyleNodeID::element(index), &mut memory));
        }
        // Rebuild the same posting out of order to exercise chunk splits as well.
        for index in 1..2000_u32 {
            postings.remove(key, StyleNodeID::element(index));
        }
        for index in (1..2000_u32).rev() {
            assert!(postings.insert(key, StyleNodeID::element(index), &mut memory));
        }
        assert_eq!(known_posting(&postings, key).len(), 1999);

        let candidates: Vec<StyleNodeID> = known_posting(&postings, key).candidates().collect();
        assert_eq!(candidates.len(), 1999);
        assert!(candidates.windows(2).all(|pair| pair[0] < pair[1]));
        assert!(known_posting(&postings, key).contains(StyleNodeID::element(1000)));
        assert!(!known_posting(&postings, key).contains(StyleNodeID::element(2500)));

        // Re-inserting a member changes nothing.
        assert!(postings.insert(key, StyleNodeID::element(1000), &mut memory));
        assert_eq!(known_posting(&postings, key).len(), 1999);
    }

    #[test]
    fn removing_the_last_member_reclaims_the_posting() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let key = SelectorPostingKey::Class(StyleAtomID(1));
        assert!(matches!(postings.lookup(key), Lookup::KnownAbsent));
        for index in 1..500_u32 {
            postings.insert(key, StyleNodeID::element(index), &mut memory);
        }
        assert!(memory.bytes_in_category(MemoryCategory::FeaturePosting) > 0);

        for index in 1..500_u32 {
            postings.remove(key, StyleNodeID::element(index));
        }
        assert!(matches!(postings.lookup(key), Lookup::KnownAbsent));
        assert_eq!(postings.feature_count(), 0);
        assert_eq!(memory.bytes_in_category(MemoryCategory::FeaturePosting), 0);
    }

    #[test]
    fn local_dispatch_keys_read_the_authoritative_element_row() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        let tag = StyleAtomID(10);
        let folded_tag = StyleAtomID(11);
        let id = StyleAtomID(20);
        let class = StyleAtomID(30);
        let attribute = StyleAtomID(40);
        let local_attribute = StyleAtomID(41);
        let directionality = StyleAtomID(50);

        facts.set_tag(node, tag, &mut memory);
        facts.set_folded_tag(node, folded_tag, &mut memory);
        facts.set_id(node, id, &mut memory);
        facts.set_class(node, class, true, &mut memory);
        facts.note_attribute_name_forms(
            attribute,
            AttributeNameForms {
                local: local_attribute,
                ..Default::default()
            },
        );
        facts.set_attribute(node, attribute, StyleAtomID(42), true, &mut memory);
        facts.set_directionality(node, directionality, &mut memory);
        facts.apply_staged(&mut memory);

        for key in [
            DispatchKey::TagName(tag),
            DispatchKey::TagName(folded_tag),
            DispatchKey::Id(id),
            DispatchKey::Class(class),
            DispatchKey::AttributeName(attribute),
            DispatchKey::AttributeName(local_attribute),
            DispatchKey::Directionality(directionality),
        ] {
            assert_eq!(facts.carries_local_dispatch_key(node, key), Some(true));
        }
        assert_eq!(
            facts.carries_local_dispatch_key(node, DispatchKey::Class(StyleAtomID(31))),
            Some(false)
        );
        assert_eq!(facts.carries_local_dispatch_key(node, DispatchKey::Universal), None);
    }

    #[test]
    fn directionality_reads_include_pending_changes() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);

        facts.set_directionality(node, StyleAtomID(2), &mut memory);
        facts.apply_staged(&mut memory);
        facts.set_directionality(node, StyleAtomID(4), &mut memory);

        assert_eq!(facts.directionality_of(node), StyleAtomID(4));
    }

    #[test]
    fn forgetting_an_element_removes_every_owned_posting() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        let name = StyleAtomID(10);
        let part = StyleAtomID(30);
        let custom_state = StyleAtomID(31);
        let forms = AttributeNameForms {
            local: StyleAtomID(11),
            folded_name: StyleAtomID(12),
            folded_local: StyleAtomID(13),
        };
        facts.note_attribute_name_forms(name, forms);
        facts.set_attribute(node, name, StyleAtomID(20), true, &mut memory);
        facts.set_parts(node, &[part], &mut memory);
        facts.set_custom_states(node, &[custom_state], &mut memory);
        facts.apply_staged(&mut memory);

        for key in [name, forms.local, forms.folded_name, forms.folded_local] {
            assert!(known_posting(&facts.postings, SelectorPostingKey::AttributeName(key)).contains(node));
        }
        assert!(known_posting(&facts.postings, SelectorPostingKey::Part(part)).contains(node));
        assert!(known_posting(&facts.postings, SelectorPostingKey::CustomState(custom_state)).contains(node));

        facts.forget(node);

        for key in [name, forms.local, forms.folded_name, forms.folded_local] {
            assert!(matches!(
                facts.postings.lookup(SelectorPostingKey::AttributeName(key)),
                Lookup::KnownAbsent
            ));
        }
        assert!(matches!(
            facts.postings.lookup(SelectorPostingKey::Part(part)),
            Lookup::KnownAbsent
        ));
        assert!(matches!(
            facts.postings.lookup(SelectorPostingKey::CustomState(custom_state)),
            Lookup::KnownAbsent
        ));
    }

    #[test]
    fn resident_fact_rows_follow_dense_element_identity_slots() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let first = StyleNodeID::element(3);
        let later = StyleNodeID::element(64);

        facts.set_tag(first, StyleAtomID(10), &mut memory);
        facts.set_tag(later, StyleAtomID(20), &mut memory);
        facts.apply_staged(&mut memory);
        assert_eq!(facts.len(), 2);
        assert!(facts.rows.row_by_element_index.is_empty());
        assert_eq!(facts.rows.row_of(first), Some(3));
        assert_eq!(facts.rows.row_of(later), Some(64));
        assert_eq!(facts.tag_of_node(first), StyleAtomID(10));
        assert_eq!(facts.tag_of_node(later), StyleAtomID(20));

        facts.forget(first);
        assert_eq!(facts.len(), 1);
        assert!(!facts.covers(first));
        facts.ensure_row(first);
        facts.apply_staged(&mut memory);
        assert_eq!(facts.len(), 2);
        assert_eq!(facts.tag_of_node(first), StyleAtomID::NONE);
    }

    #[test]
    fn primary_fact_rows_replace_element_slots_in_place() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(3);
        let first_class = StyleAtomID(20);
        let second_class = StyleAtomID(21);
        let part = StyleAtomID(30);
        let custom_state = StyleAtomID(31);

        facts.set_tag(node, StyleAtomID(10), &mut memory);
        facts.set_class(node, first_class, true, &mut memory);
        assert!(facts.has_dirty_staging());
        facts.apply_staged(&mut memory);
        assert!(!facts.has_dirty_staging());
        let initial_generation = facts.primary().generation();
        assert_eq!(facts.primary().row_count(), 4);
        assert_eq!(facts.primary().stale_rows(), 0);

        facts.set_class(node, second_class, true, &mut memory);
        facts.set_state(node, StateFact::Hover, true);
        facts.set_parts(node, &[part], &mut memory);
        facts.set_custom_states(node, &[custom_state], &mut memory);
        facts.apply_staged(&mut memory);
        assert_eq!(facts.primary().generation(), initial_generation);
        assert_eq!(facts.primary().row_count(), 4);
        assert_eq!(facts.primary().stale_rows(), 0);
        assert_eq!(facts.classes_of_node(node), &[first_class, second_class]);
        let row = facts.primary().row_of(node).unwrap();
        assert_eq!(facts.primary().parts_of(row), &[part]);
        assert_eq!(facts.primary().custom_states_of(row), &[custom_state]);

        facts.set_class(node, first_class, false, &mut memory);
        facts.apply_staged(&mut memory);
        assert_eq!(facts.primary().generation(), initial_generation);
        assert_eq!(facts.primary().row_count(), 4);
        assert_eq!(facts.primary().stale_rows(), 0);
        assert_eq!(facts.classes_of_node(node), &[second_class]);
        assert!(facts.states_of_node(node).contains(StateFact::Hover));
    }

    #[test]
    fn fixed_fact_changes_reuse_primary_payload_handles() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        facts.set_class(node, StyleAtomID(10), true, &mut memory);
        facts.set_attribute(node, StyleAtomID(20), StyleAtomID(21), true, &mut memory);
        facts.apply_staged(&mut memory);
        let payload_lengths = (facts.rows.classes.len(), facts.rows.attributes.len());

        facts.set_state(node, StateFact::Hover, true);
        facts.apply_staged(&mut memory);

        assert_eq!((facts.rows.classes.len(), facts.rows.attributes.len()), payload_lengths);
        assert_eq!(facts.primary_stale_payload_bytes, 0);
    }

    #[test]
    fn ordinary_elements_allocate_no_optional_fact_metadata() {
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(64);

        facts.set_is_slot(node, false);
        assert!(facts.metadata.is_empty());

        facts.set_is_slot(node, true);
        assert!(facts.is_slot(node));
        facts.set_is_slot(node, false);
        assert!(!facts.is_slot(node));
    }

    #[test]
    fn element_fact_capacity_includes_auxiliary_catalogs_and_staging() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(64);
        let initial = facts.capacity_bytes();

        facts.set_class(node, StyleAtomID(1), true, &mut memory);
        facts.set_parts(node, &[StyleAtomID(2), StyleAtomID(3)], &mut memory);
        facts.set_custom_states(node, &[StyleAtomID(4), StyleAtomID(5)], &mut memory);
        facts.set_attribute(node, StyleAtomID(6), StyleAtomID(7), true, &mut memory);
        assert!(facts.capacity_bytes() > initial);

        let pending = facts.capacity_bytes();
        facts.set_animation_names(node, &[StyleAtomID(8), StyleAtomID(9)], &mut memory);
        assert!(facts.capacity_bytes() > pending);

        let metadata = facts.capacity_bytes();
        facts.set_language_text(StyleAtomID(10), &[1, 2, 3, 4]);
        facts.set_attribute_value_text(StyleAtomID(11), &[5, 6, 7, 8]);
        facts.note_attribute_name_forms(
            StyleAtomID(12),
            AttributeNameForms {
                local: StyleAtomID(13),
                folded_name: StyleAtomID(14),
                folded_local: StyleAtomID(15),
            },
        );
        assert!(facts.capacity_bytes() > metadata);

        let text_catalogs = facts.capacity_bytes();
        facts.set_custom_property_names(node, &[StyleAtomID(16), StyleAtomID(17)], &mut memory);
        assert!(facts.capacity_bytes() > text_catalogs);
        facts.apply_staged(&mut memory);
        facts.release_staging(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
            facts.capacity_bytes()
        );
    }

    #[test]
    fn live_fact_atom_roots_use_incremental_counts() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        facts.set_tag(node, StyleAtomID(1), &mut memory);
        facts.set_folded_tag(node, StyleAtomID(2), &mut memory);
        facts.set_id(node, StyleAtomID(3), &mut memory);
        facts.set_language(node, StyleAtomID(4));
        facts.set_namespace(node, StyleAtomID(5));
        facts.set_part_exposure(node, StyleAtomID(6));
        facts.set_directionality(node, StyleAtomID(7), &mut memory);
        facts.set_custom_states(node, &[StyleAtomID(8)], &mut memory);
        facts.set_parts(node, &[StyleAtomID(9)], &mut memory);
        facts.set_class(node, StyleAtomID(10), true, &mut memory);
        facts.note_attribute_name_forms(
            StyleAtomID(11),
            AttributeNameForms {
                local: StyleAtomID(13),
                folded_name: StyleAtomID(14),
                folded_local: StyleAtomID(15),
            },
        );
        facts.set_attribute(node, StyleAtomID(11), StyleAtomID(12), true, &mut memory);
        facts.set_animation_names(node, &[StyleAtomID(16)], &mut memory);
        facts.set_custom_property_names(node, &[StyleAtomID(17)], &mut memory);
        facts.apply_staged(&mut memory);

        let mut atoms = HashSet::default();
        let visited = facts.collect_atoms(&mut atoms);
        assert_eq!(visited, 15);
        assert_eq!(atoms, (1..=17).map(StyleAtomID).collect());

        facts.forget(node);
        let mut atoms = HashSet::default();
        facts.collect_atoms(&mut atoms);
        assert!(atoms.is_empty());
    }

    #[test]
    fn detached_element_churn_reuses_reclaimable_auxiliary_catalog_storage() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);

        for index in 1..128_u32 {
            let language = StyleAtomID(index * 4);
            let attribute_name = StyleAtomID(index * 4 + 1);
            let attribute_value = StyleAtomID(index * 4 + 2);
            let custom_property = StyleAtomID(index * 4 + 3);
            let name_forms = AttributeNameForms {
                local: StyleAtomID(index * 4 + 1000),
                folded_name: StyleAtomID(index * 4 + 1001),
                folded_local: StyleAtomID(index * 4 + 1002),
            };
            facts.set_language_text(language, &[index as u16]);
            facts.set_language(node, language);
            facts.note_attribute_name_forms(attribute_name, name_forms);
            facts.set_attribute_value_text(attribute_value, &[index as u16]);
            facts.set_attribute(node, attribute_name, attribute_value, true, &mut memory);
            facts.set_custom_property_names(node, &[custom_property], &mut memory);
            facts.apply_staged(&mut memory);

            facts.forget(node);
            facts.sweep_auxiliary_catalogs();
            assert_eq!(
                facts.rows.attribute_catalogs.language_texts.get(language.0 as usize),
                Some(&Some(Box::from([index as u16])))
            );
            assert_eq!(
                facts.rows.attribute_catalogs.name_forms.get(attribute_name.0 as usize),
                Some(name_forms)
            );
            assert!(facts.rows.attribute_catalogs.value_texts.iter().all(Option::is_none));
            assert!(facts.custom_property_name_sets.live_is_empty());
            assert!(facts.custom_property_set_ids_by_name.iter().all(Vec::is_empty));
        }

        assert_eq!(facts.custom_property_name_sets.len(), 1);
        assert_eq!(facts.custom_property_name_set_vacancies, [1]);
    }

    #[test]
    fn element_declarations_follow_dense_element_identity_slots() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let first = StyleNodeID::element(3);
        let later = StyleNodeID::element(64);
        let inline = ElementDeclarationKind::InlineStyle;
        let hints = ElementDeclarationKind::PresentationalHint;
        let svg = ElementDeclarationKind::SvgPresentationAttribute;
        let declared = |property, important, value| DeclaredProperty {
            property,
            important,
            operator: super::super::cascade::CascadeOperator::Declared,
            value: super::super::cascade::SpecifiedValueID(value),
        };

        facts.set_element_declared_properties(
            first,
            inline,
            vec![declared(1, false, 10), declared(2, true, 20)],
            Vec::new(),
            true,
        );
        facts.set_element_declared_properties(first, hints, vec![declared(3, false, 30)], Vec::new(), false);
        facts.set_element_declared_properties(later, svg, vec![declared(4, false, 40)], Vec::new(), true);
        facts.set_element_declared_properties(later, inline, Vec::new(), Vec::new(), false);
        assert_eq!(
            facts.element_declared_properties.get(first, inline),
            (&[declared(1, false, 10), declared(2, true, 20)][..], true)
        );
        assert_eq!(
            facts.element_declared_properties.get(first, hints),
            (&[declared(3, false, 30)][..], false)
        );
        assert_eq!(
            facts.element_declared_properties.get(later, svg),
            (&[declared(4, false, 40)][..], true)
        );
        assert_eq!(facts.element_declared_properties.get(later, inline), (&[][..], false));
        assert_eq!(facts.element_declared_properties.rows.len(), 65);
        facts.set_element_declared_properties(first, inline, Vec::new(), Vec::new(), true);
        assert!(facts.element_declared_properties.get(first, inline).0.is_empty());
        assert_eq!(
            facts.element_declared_properties.get(first, hints),
            (&[declared(3, false, 30)][..], false)
        );
        facts.set_element_declared_properties(later, inline, Vec::new(), Vec::new(), true);
        assert_eq!(facts.element_declared_properties.get(later, inline), (&[][..], true));

        // Declaration rows can exist without a resident selector-fact row. Retirement still has to
        // clear them before the dense identity is reused.
        facts.forget(first);
        assert!(facts.element_declared_properties.get(first, hints).0.is_empty());
        assert_eq!(
            facts.element_declared_properties.get(later, svg),
            (&[declared(4, false, 40)][..], true)
        );
        facts.apply_staged(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
            facts.capacity_bytes()
        );
    }

    #[test]
    fn a_posting_that_crosses_the_limit_stays_exact_until_the_boundary() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        memory.set_tier3_limit_for_test(0);
        memory.begin_tier3_quota_period();
        let mut postings = FeaturePostings::new();
        let key = SelectorPostingKey::Class(StyleAtomID(1));

        assert!(postings.insert(key, StyleNodeID::element(1), &mut memory));
        assert_eq!(known_posting(&postings, key).length, 1);
        assert!(memory.finish_tier3_quota_period()[MemoryCategory::FeaturePosting as usize]);
        postings.evict_all();
        assert!(matches!(postings.lookup(key), Lookup::Missing(gap) if gap == key));
    }

    #[test]
    fn closed_posting_admission_keeps_existing_postings_exact() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let key = SelectorPostingKey::Class(StyleAtomID(1));
        let missing_key = SelectorPostingKey::Class(StyleAtomID(2));

        assert!(postings.insert(key, StyleNodeID::element(1), &mut memory));
        let admitted_bytes = memory.bytes_in_category(MemoryCategory::FeaturePosting);
        memory.set_tier3_limit_for_test(admitted_bytes);
        for index in 2..512 {
            assert!(postings.insert(key, StyleNodeID::element(index), &mut memory));
        }

        assert_eq!(known_posting(&postings, key).length, 511);
        assert!(!postings.insert(missing_key, StyleNodeID::element(1), &mut memory));
        assert!(matches!(postings.lookup(missing_key), Lookup::Missing(gap) if gap == missing_key));
    }

    #[test]
    fn high_cardinality_caps_apply_only_to_selector_postings() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let selector = SelectorPostingKey::Class(StyleAtomID(1));
        let dependency = DependencyPostingKey::AnimationName(StyleAtomID(2));
        postings.set_selector_posting_limit(2);
        for index in 1..=3 {
            let node = StyleNodeID::element(index);
            assert_eq!(postings.insert(selector, node, &mut memory), index <= 2);
            assert!(postings.insert(dependency, node, &mut memory));
        }

        assert!(matches!(postings.lookup(selector), Lookup::Missing(gap) if gap == selector));
        assert_eq!(known_posting(&postings, dependency).length, 3);
        assert!(!postings.insert(selector, StyleNodeID::element(4), &mut memory));
    }

    #[test]
    fn grown_postings_are_rechecked_against_the_fresh_limit() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let key = SelectorPostingKey::Class(StyleAtomID(1));
        postings.set_selector_posting_limit(5000);
        for index in 1..=4097 {
            assert!(postings.insert(key, StyleNodeID::element(index), &mut memory));
        }
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::FeaturePosting),
            postings.retained_capacity_bytes()
        );

        postings.update_selector_posting_limit(1);

        assert!(matches!(postings.lookup(key), Lookup::Missing(gap) if gap == key));
        assert!(postings.grown_selector_postings.is_empty());
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::FeaturePosting),
            postings.retained_capacity_bytes()
        );
    }

    #[test]
    fn evicting_every_posting_retains_only_the_missing_key_charge() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        for feature in 1..20_u32 {
            for index in 1..50_u32 {
                postings.insert(
                    SelectorPostingKey::Class(StyleAtomID(feature)),
                    StyleNodeID::element(index),
                    &mut memory,
                );
            }
        }
        assert!(memory.bytes_in_category(MemoryCategory::FeaturePosting) > 0);
        postings.evict_all();
        assert_eq!(postings.feature_count(), 0);
        let key = SelectorPostingKey::Class(StyleAtomID(1));
        assert!(matches!(postings.lookup(key), Lookup::Missing(gap) if gap == key));
        assert!(matches!(
            postings.lookup(SelectorPostingKey::Class(StyleAtomID(100))),
            Lookup::KnownAbsent
        ));
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::FeaturePosting),
            postings.missing_capacity_bytes()
        );
    }

    #[test]
    fn missing_postings_rebuild_from_mutated_authoritative_facts_when_budget_returns() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        let old_class = StyleAtomID(1);
        let new_class = StyleAtomID(2);
        let old_animation = StyleAtomID(3);
        let new_animation = StyleAtomID(4);
        facts.ensure_row(node);
        facts.set_class(node, old_class, true, &mut memory);
        facts.set_animation_names(node, &[old_animation], &mut memory);
        facts.apply_staged(&mut memory);
        facts.postings_mut().evict_all();

        memory.set_tier3_limit_for_test(0);
        facts.set_class(node, old_class, false, &mut memory);
        facts.set_class(node, new_class, true, &mut memory);
        facts.set_animation_names(node, &[new_animation], &mut memory);
        facts.apply_staged(&mut memory);
        let new_class_key = SelectorPostingKey::Class(new_class);
        let new_animation_key = DependencyPostingKey::AnimationName(new_animation);
        assert_eq!(known_posting(facts.postings(), new_class_key).length, 1);
        assert!(matches!(facts.postings().lookup(new_animation_key), Lookup::Missing(gap) if gap == new_animation_key));
        assert_eq!(facts.posting_rebuild_closed_at_headroom, None);

        // Admission was already closed before the rebuild began, so its failure says nothing about
        // whether the same headroom could fund a later rebuild after another category reopens it.
        facts.apply_staged(&mut memory);
        assert_eq!(facts.posting_rebuild_closed_at_headroom, None);

        memory.set_tier3_limit_for_test(u64::MAX);
        memory.begin_tier3_quota_period();
        facts.apply_staged(&mut memory);
        assert_eq!(facts.posting_rebuild_closed_at_headroom, None);
        let old_class_key = SelectorPostingKey::Class(old_class);
        let old_animation_key = DependencyPostingKey::AnimationName(old_animation);
        assert!(matches!(facts.postings().lookup(old_class_key), Lookup::KnownAbsent));
        assert!(matches!(
            facts.postings().lookup(old_animation_key),
            Lookup::KnownAbsent
        ));
        assert!(known_posting(facts.postings(), new_class_key).contains(node));
        assert!(known_posting(facts.postings(), new_animation_key).contains(node));
    }

    #[test]
    fn evicting_one_posting_preserves_exact_absence_for_other_keys() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let evicted = SelectorPostingKey::Class(StyleAtomID(1));
        let absent = SelectorPostingKey::Class(StyleAtomID(2));
        postings.insert(evicted, StyleNodeID::element(1), &mut memory);

        postings.evict(evicted);

        assert!(matches!(postings.lookup(evicted), Lookup::Missing(gap) if gap == evicted));
        assert!(matches!(postings.lookup(absent), Lookup::KnownAbsent));
    }

    #[test]
    fn primary_fact_capacity_matches_its_columns() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut store = ElementFactStore::new();

        // Grow: enough classes and attributes per row to force several reallocations.
        for index in 1..40_u32 {
            let node = StyleNodeID::element(index);
            for name in 1..12_u32 {
                store.set_class(node, StyleAtomID(name), true, &mut memory);
                store.set_attribute(node, StyleAtomID(name), StyleAtomID::NONE, true, &mut memory);
            }
            store.apply_staged(&mut memory);
            store.release_staging(&mut memory);
            assert_eq!(
                memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
                store.capacity_bytes()
            );
        }

        // Shrink: removals leave capacity where it was, and forgetting a row gives it all back.
        for index in 1..40_u32 {
            let node = StyleNodeID::element(index);
            for name in 1..6_u32 {
                store.set_class(node, StyleAtomID(name), false, &mut memory);
                store.set_attribute(node, StyleAtomID(name), StyleAtomID::NONE, false, &mut memory);
            }
            store.apply_staged(&mut memory);
            store.release_staging(&mut memory);
            assert_eq!(
                memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
                store.capacity_bytes()
            );
        }

        for index in 1..40_u32 {
            store.forget(StyleNodeID::element(index));
            store.apply_staged(&mut memory);
            store.release_staging(&mut memory);
            assert_eq!(
                memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
                store.capacity_bytes()
            );
        }
        assert!(store.is_empty());

        // The retained column capacities remain charged after every live row is forgotten.
        store.apply_staged(&mut memory);
        store.release_staging(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
            store.capacity_bytes()
        );
    }

    #[test]
    fn cascade_order_projection_handles_duplicate_entries_and_sparse_rules() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32, selector_entry: u32, multi_key| DispatchEntry {
            identity: EntryID(rule * 10 + selector_entry),
            rule: RuleID(rule),
            program: SelectorProgramID(rule),
            entry: selector_entry,
            cascade_order: 0,
            required_attribute_value: StyleAtomID::NONE,
            required_parent: None,
            required_ancestor: None,
            required_ancestor_index: None,
            required_subject_bloom: 0,
            prefix_matched: false,
            multi_key,
        };
        dispatch.insert(DispatchKey::Class(StyleAtomID(10)), entry(1, 0, true));
        dispatch.insert(DispatchKey::Class(StyleAtomID(11)), entry(1, 0, true));
        dispatch.insert(DispatchKey::Class(StyleAtomID(12)), entry(1, 1, false));
        dispatch.insert(DispatchKey::Class(StyleAtomID(13)), entry(1000, 0, false));

        dispatch.assign_cascade_order(|candidate| candidate.rule);

        let first_branch = dispatch.bucket(DispatchKey::Class(StyleAtomID(10))).next().unwrap();
        let second_branch = dispatch.bucket(DispatchKey::Class(StyleAtomID(11))).next().unwrap();
        let later_entry = dispatch.bucket(DispatchKey::Class(StyleAtomID(12))).next().unwrap();
        let sparse_rule = dispatch.bucket(DispatchKey::Class(StyleAtomID(13))).next().unwrap();
        assert_eq!(first_branch.cascade_order, second_branch.cascade_order);
        assert!(first_branch.cascade_order < later_entry.cascade_order);
        assert_eq!(
            dispatch.cascade_order_for_entry(first_branch.rule, first_branch.program, first_branch.entry),
            Some(first_branch.cascade_order)
        );
        assert_eq!(
            dispatch.cascade_order_for_entry(later_entry.rule, later_entry.program, later_entry.entry),
            Some(later_entry.cascade_order)
        );
        assert_eq!(
            dispatch.cascade_order_for_entry(sparse_rule.rule, sparse_rule.program, sparse_rule.entry),
            Some(sparse_rule.cascade_order)
        );
        assert_eq!(
            dispatch.cascade_order_for_entry(first_branch.rule, SelectorProgramID(2), first_branch.entry),
            None
        );
        assert_eq!(
            dispatch.cascade_order_for_entry(first_branch.rule, first_branch.program, 2),
            None
        );
        assert_eq!(
            dispatch.cascade_order_for_entry(RuleID(999), SelectorProgramID(999), 0),
            None
        );
    }

    #[test]
    fn shared_dispatch_allocations_are_charged_once() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut dispatch = RuleDispatch::new();
        dispatch.insert(
            DispatchKey::Class(StyleAtomID(10)),
            DispatchEntry {
                identity: EntryID(1),
                rule: RuleID(1),
                program: SelectorProgramID(1),
                entry: 0,
                cascade_order: 0,
                required_attribute_value: StyleAtomID::NONE,
                required_parent: None,
                required_ancestor: Some(DispatchKey::Class(StyleAtomID(20))),
                required_ancestor_index: None,
                required_subject_bloom: 0,
                prefix_matched: false,
                multi_key: false,
            },
        );
        dispatch.settle_memory(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::RuleProgram),
            dispatch.capacity_bytes()
        );

        let mut rebound = RuleDispatch::rebind_rules(&dispatch, &[RuleID(2)]);
        rebound.settle_memory(&mut memory);
        assert!(dispatch.shares_entries_with(&rebound));
        let shared_bytes = dispatch.entries.capacity_bytes()
            + RuleDispatch::topology_capacity_bytes(&dispatch.topology)
            + RuleDispatch::ancestor_capacity_bytes(&dispatch.topology.ancestors);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::RuleProgram),
            dispatch.scope_capacity_bytes() + rebound.scope_capacity_bytes() + shared_bytes
        );

        drop(dispatch);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::RuleProgram),
            rebound.scope_capacity_bytes() + shared_bytes
        );
        drop(rebound);
        assert_eq!(memory.bytes_in_category(MemoryCategory::RuleProgram), 0);
    }

    #[test]
    fn a_candidate_probes_only_the_buckets_its_own_facts_name() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32| DispatchEntry {
            identity: EntryID(rule),
            rule: RuleID(rule),
            program: SelectorProgramID(rule),
            entry: 0,
            cascade_order: 0,
            required_attribute_value: StyleAtomID::NONE,
            required_parent: None,
            required_ancestor: None,
            required_ancestor_index: None,
            required_subject_bloom: 0,
            prefix_matched: false,
            multi_key: false,
        };
        dispatch.insert(DispatchKey::Class(StyleAtomID(10)), entry(1));
        dispatch.insert(DispatchKey::Id(StyleAtomID(20)), entry(2));
        dispatch.insert(DispatchKey::TagName(StyleAtomID(1)), entry(3));
        dispatch.insert(DispatchKey::Universal, entry(4));
        dispatch.insert(DispatchKey::Class(StyleAtomID(11)), entry(5));
        dispatch.insert(DispatchKey::AttributeName(StyleAtomID(30)), entry(6));

        let mut facts = StyleNodeFacts::new();
        facts.push_row(
            StyleNodeID::element(1),
            StyleAtomID(1),
            StyleAtomID(20),
            StateSet::default(),
            &[StyleAtomID(10)],
            &[AttributeFact {
                name: StyleAtomID(30),
                value: StyleAtomID::NONE,
                text_offset: u32::MAX,
                text_length: 0,
            }],
        );

        let mut workspace = DispatchCandidateWorkspace::default();
        let mut rules: Vec<u32> = dispatch
            .candidates_for(
                &facts,
                0,
                false,
                ParentDispatchFacts::Unknown,
                None,
                CandidateEntries::All,
                false,
                &mut workspace,
            )
            .map(|candidate| candidate.rule.0)
            .collect();
        rules.sort_unstable();
        assert_eq!(rules, vec![1, 2, 3, 4, 6], "the .other-class bucket must not be probed");
    }

    #[test]
    fn attribute_aliases_emit_each_candidate_once_without_hiding_value_matches() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32, required_attribute_value| DispatchEntry {
            identity: EntryID(rule),
            rule: RuleID(rule),
            program: SelectorProgramID(rule),
            entry: 0,
            cascade_order: 0,
            required_attribute_value,
            required_parent: None,
            required_ancestor: None,
            required_ancestor_index: None,
            required_subject_bloom: 0,
            prefix_matched: false,
            multi_key: false,
        };
        let shared_local_name = StyleAtomID(30);
        let matching_value = StyleAtomID(40);
        dispatch.insert(
            DispatchKey::AttributeName(shared_local_name),
            entry(1, StyleAtomID::NONE),
        );
        dispatch.insert(DispatchKey::AttributeName(shared_local_name), entry(2, matching_value));

        let mut facts = StyleNodeFacts::new();
        facts.note_attribute_name_forms(
            StyleAtomID(31),
            AttributeNameForms {
                local: shared_local_name,
                ..AttributeNameForms::default()
            },
        );
        facts.note_attribute_name_forms(
            StyleAtomID(32),
            AttributeNameForms {
                local: shared_local_name,
                ..AttributeNameForms::default()
            },
        );
        facts.push_row(
            StyleNodeID::element(1),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[
                AttributeFact {
                    name: StyleAtomID(31),
                    value: StyleAtomID(41),
                    text_offset: u32::MAX,
                    text_length: 0,
                },
                AttributeFact {
                    name: StyleAtomID(32),
                    value: matching_value,
                    text_offset: u32::MAX,
                    text_length: 0,
                },
            ],
        );

        let mut workspace = DispatchCandidateWorkspace::default();
        for _ in 0..2 {
            let rules: Vec<u32> = dispatch
                .candidates_for(
                    &facts,
                    0,
                    false,
                    ParentDispatchFacts::Unknown,
                    None,
                    CandidateEntries::All,
                    false,
                    &mut workspace,
                )
                .map(|candidate| candidate.rule.0)
                .collect();
            assert_eq!(rules, vec![1, 2]);
        }
    }

    #[test]
    fn a_universal_subject_probes_only_the_bucket_its_parent_names() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32, required_parent| DispatchEntry {
            identity: EntryID(rule),
            rule: RuleID(rule),
            program: SelectorProgramID(rule),
            entry: 0,
            cascade_order: 0,
            required_attribute_value: StyleAtomID::NONE,
            required_parent,
            required_ancestor: None,
            required_ancestor_index: None,
            required_subject_bloom: 0,
            prefix_matched: false,
            multi_key: false,
        };
        dispatch.insert(
            DispatchKey::Universal,
            entry(1, Some(DispatchKey::Class(StyleAtomID(10)))),
        );
        dispatch.insert(
            DispatchKey::Universal,
            entry(2, Some(DispatchKey::Class(StyleAtomID(11)))),
        );
        dispatch.insert(DispatchKey::Universal, entry(3, None));
        dispatch.finish_prefixes();

        let mut facts = StyleNodeFacts::new();
        facts.push_row(
            StyleNodeID::element(1),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet::default(),
            &[StyleAtomID(10)],
            &[],
        );
        facts.push_row(
            StyleNodeID::element(2),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[],
        );

        let mut workspace = DispatchCandidateWorkspace::default();
        let mut rules = |dispatch: &RuleDispatch, parent| {
            dispatch
                .candidates_for(
                    &facts,
                    1,
                    false,
                    parent,
                    None,
                    CandidateEntries::All,
                    false,
                    &mut workspace,
                )
                .map(|candidate| candidate.rule.0)
                .collect::<Vec<_>>()
        };
        assert_eq!(
            rules(
                &dispatch,
                ParentDispatchFacts::Known {
                    row: 0,
                    is_document_root: true,
                }
            ),
            vec![3, 1]
        );
        assert_eq!(rules(&dispatch, ParentDispatchFacts::NoElementParent), vec![3]);
        assert_eq!(rules(&dispatch, ParentDispatchFacts::Unknown), vec![3, 1, 2]);
    }

    #[test]
    fn a_local_subject_bucket_respects_its_parent_requirement() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32, required_parent| DispatchEntry {
            identity: EntryID(rule),
            rule: RuleID(rule),
            program: SelectorProgramID(rule),
            entry: 0,
            cascade_order: 0,
            required_attribute_value: StyleAtomID::NONE,
            required_parent,
            required_ancestor: None,
            required_ancestor_index: None,
            required_subject_bloom: 0,
            prefix_matched: false,
            multi_key: false,
        };
        let item = StyleAtomID(20);
        dispatch.insert(
            DispatchKey::Class(item),
            entry(1, Some(DispatchKey::Class(StyleAtomID(10)))),
        );
        dispatch.insert(
            DispatchKey::Class(item),
            entry(2, Some(DispatchKey::Class(StyleAtomID(11)))),
        );
        dispatch.insert(DispatchKey::Class(item), entry(3, None));

        let mut facts = StyleNodeFacts::new();
        facts.push_row(
            StyleNodeID::element(1),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet::default(),
            &[StyleAtomID(10)],
            &[],
        );
        facts.push_row(
            StyleNodeID::element(2),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet::default(),
            &[item],
            &[],
        );

        let mut workspace = DispatchCandidateWorkspace::default();
        let mut rules = |dispatch: &RuleDispatch, parent| {
            dispatch
                .candidates_for(
                    &facts,
                    1,
                    false,
                    parent,
                    None,
                    CandidateEntries::All,
                    false,
                    &mut workspace,
                )
                .map(|candidate| candidate.rule.0)
                .collect::<Vec<_>>()
        };
        assert_eq!(
            rules(
                &dispatch,
                ParentDispatchFacts::Known {
                    row: 0,
                    is_document_root: true,
                }
            ),
            vec![1, 3]
        );
        assert_eq!(rules(&dispatch, ParentDispatchFacts::NoElementParent), vec![3]);
        assert_eq!(rules(&dispatch, ParentDispatchFacts::Unknown), vec![1, 2, 3]);
    }

    #[test]
    fn rows_are_addressed_by_element_index() {
        let mut facts = StyleNodeFacts::new();
        let first = StyleNodeID::element(1);
        let third = StyleNodeID::element(3);
        facts.push_row(
            first,
            StyleAtomID(10),
            StyleAtomID::NONE,
            StateSet::default(),
            &[StyleAtomID(20)],
            &[],
        );
        facts.push_row(
            third,
            StyleAtomID(11),
            StyleAtomID(30),
            StateSet::default(),
            &[StyleAtomID(21), StyleAtomID(22)],
            &[],
        );

        assert_eq!(facts.row_count(), 2);
        let row = facts.row_of(third).unwrap();
        assert_eq!(facts.node_at(row), third);
        assert_eq!(facts.tag_of(row), StyleAtomID(11));
        assert_eq!(facts.id_of(row), StyleAtomID(30));
        assert_eq!(facts.classes_of(row), &[StyleAtomID(21), StyleAtomID(22)]);
        assert_eq!(facts.classes_of(facts.row_of(first).unwrap()), &[StyleAtomID(20)]);

        // A node the batch does not cover is a miss, never a negative answer.
        assert_eq!(facts.row_of(StyleNodeID::element(2)), None);
        assert_eq!(facts.row_of(StyleNodeID::element(99)), None);
    }

    #[test]
    fn attribute_text_is_carried_only_where_a_string_operator_needs_it() {
        let mut facts = StyleNodeFacts::new();
        let href: Vec<u16> = "https://example.com".encode_utf16().collect();
        let (offset, length) = facts.push_text(&href);
        facts.push_row(
            StyleNodeID::element(1),
            StyleAtomID(10),
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[
                AttributeFact {
                    name: StyleAtomID(40),
                    value: StyleAtomID::NONE,
                    text_offset: offset,
                    text_length: length,
                },
                AttributeFact {
                    name: StyleAtomID(41),
                    value: StyleAtomID(50),
                    text_offset: u32::MAX,
                    text_length: 0,
                },
            ],
        );

        let row = facts.row_of(StyleNodeID::element(1)).unwrap();
        let with_text = facts.attribute_of(row, StyleAtomID(40)).unwrap();
        assert_eq!(facts.text_of(with_text), Some(href.as_slice()));

        // An attribute answered by an atom carries no text at all.
        let interned = facts.attribute_of(row, StyleAtomID(41)).unwrap();
        assert_eq!(facts.text_of(interned), None);
        assert_eq!(interned.value, StyleAtomID(50));

        assert_eq!(facts.attribute_of(row, StyleAtomID(42)), None);
    }

    #[test]
    fn pending_attribute_exposes_both_transaction_sides() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut tree = StyleNodeTree::new(&mut memory);
        let node = tree.allocate_element(&mut memory);
        let name = StyleAtomID(40);
        let old = StyleAtomID(50);
        let new = StyleAtomID(51);
        let newest = StyleAtomID(52);
        let old_text: Vec<u16> = "old".encode_utf16().collect();
        let new_text: Vec<u16> = "new".encode_utf16().collect();
        let newest_text: Vec<u16> = "newest".encode_utf16().collect();
        let mut store = ElementFactStore::new();
        store.set_attribute_value_text(old, &old_text);
        store.set_attribute_value_text(new, &new_text);
        store.set_attribute_value_text(newest, &newest_text);
        store.set_attribute(node, name, old, true, &mut memory);
        store.apply_staged(&mut memory);
        store.release_staging(&mut memory);
        store.set_attribute(node, name, new, true, &mut memory);
        store.apply_staged(&mut memory);
        store.set_attribute(node, name, newest, true, &mut memory);

        let before = store.staged_before_facts();
        store.apply_staged(&mut memory);
        let after = store.primary();
        assert!(before.text.is_empty());
        assert!(after.text.is_empty());

        let old_attribute = before.attribute_of(before.row_of(node).unwrap(), name).unwrap();
        assert_eq!(old_attribute.value, old);
        assert_eq!(before.text_of(old_attribute), Some(old_text.as_slice()));
        let new_attribute = after.attribute_of(after.row_of(node).unwrap(), name).unwrap();
        assert_eq!(new_attribute.value, newest);
        assert_eq!(after.text_of(new_attribute), Some(newest_text.as_slice()));
    }

    #[test]
    fn reclaimed_atoms_leave_no_catalog_or_posting_rows_for_reuse() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut store = ElementFactStore::new();
        let atom = StyleAtomID(40);
        store.note_attribute_name_forms(
            atom,
            AttributeNameForms {
                local: StyleAtomID(41),
                folded_name: StyleAtomID(42),
                folded_local: StyleAtomID(43),
            },
        );
        store.set_attribute_value_text(atom, &[1, 2, 3]);
        store.set_language_text(atom, &[4, 5, 6]);
        store
            .postings
            .insert(SelectorPostingKey::Class(atom), StyleNodeID::element(1), &mut memory);

        store.forget_atoms(&[atom, StyleAtomID(42)]);

        assert_eq!(store.attribute_name_forms(atom), AttributeNameForms::default());
        assert!(!store.has_attribute_value_text(atom));
        assert_eq!(
            store.attribute_catalogs.language_texts.get(atom.0 as usize),
            Some(&None)
        );
        assert!(matches!(
            store.postings.lookup(SelectorPostingKey::Class(atom)),
            Lookup::KnownAbsent
        ));

        store.note_attribute_name_forms(
            atom,
            AttributeNameForms {
                local: StyleAtomID(60),
                ..AttributeNameForms::default()
            },
        );
        store.set_attribute_value_text(atom, &[7, 8]);
        store.set_language_text(atom, &[9, 10]);
        assert_eq!(store.attribute_name_forms(atom).local, StyleAtomID(60));
        assert_eq!(
            store
                .attribute_catalogs
                .value_texts
                .get(atom.0 as usize)
                .and_then(Option::as_deref),
            Some([7, 8].as_slice())
        );
        assert_eq!(
            store
                .attribute_catalogs
                .language_texts
                .get(atom.0 as usize)
                .and_then(Option::as_deref),
            Some([9, 10].as_slice())
        );
    }

    #[test]
    fn live_attribute_names_keep_their_derived_forms_live() {
        let mut store = ElementFactStore::new();
        let name = StyleAtomID(40);
        let forms = AttributeNameForms {
            local: StyleAtomID(41),
            folded_name: StyleAtomID(42),
            folded_local: StyleAtomID(43),
        };
        store.note_attribute_name_forms(name, forms);
        let mut live = HashSet::default();
        live.insert(name);

        store.extend_live_attribute_name_forms(&mut live);

        assert_eq!(live.len(), 4);
        assert!(
            [name, forms.local, forms.folded_name, forms.folded_local]
                .into_iter()
                .all(|atom| live.contains(&atom))
        );
    }

    #[test]
    fn a_primary_fact_view_is_immutable_and_uncharged() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut tree = StyleNodeTree::new(&mut memory);
        let node = tree.allocate_element(&mut memory);
        let mut store = ElementFactStore::new();
        store.set_tag(node, StyleAtomID(1), &mut memory);
        store.apply_staged(&mut memory);
        store.release_staging(&mut memory);

        let view = store.primary_view();
        assert_eq!(view.capacity_bytes(), 0);
        store.set_tag(node, StyleAtomID(2), &mut memory);
        assert_eq!(view.tag_of(view.row_of(node).unwrap()), StyleAtomID(1));
        store.apply_staged(&mut memory);
        assert_eq!(view.tag_of(view.row_of(node).unwrap()), StyleAtomID(1));
        assert_eq!(store.tag_of_node(node), StyleAtomID(2));
    }

    #[test]
    fn catalog_publication_does_not_copy_primary_fact_rows() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut store = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        let name = StyleAtomID(20);
        let forms = AttributeNameForms {
            local: StyleAtomID(21),
            folded_name: StyleAtomID(22),
            folded_local: StyleAtomID(23),
        };
        store.ensure_row(node);
        store.apply_staged(&mut memory);
        store.release_staging(&mut memory);

        let primary_rows = Rc::as_ptr(&store.rows);
        let view = store.primary_view();
        store.note_attribute_name_forms(name, forms);
        assert_eq!(Rc::as_ptr(&store.rows), primary_rows);
        assert_eq!(store.attribute_name_forms(name), forms);
        assert_eq!(
            view.attribute_name_forms(name),
            AttributeNameForms {
                folded_name: name,
                ..AttributeNameForms::default()
            }
        );

        drop(view);
        store.apply_staged(&mut memory);
        assert_eq!(Rc::as_ptr(&store.rows), primary_rows);
        assert_eq!(store.primary().attribute_name_forms(name), forms);
    }

    #[test]
    fn catalog_synchronization_preserves_a_retained_primary_fact_view() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut store = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        let name = StyleAtomID(20);
        let forms = AttributeNameForms {
            local: StyleAtomID(21),
            folded_name: StyleAtomID(22),
            folded_local: StyleAtomID(23),
        };
        store.ensure_row(node);
        store.apply_staged(&mut memory);
        store.release_staging(&mut memory);

        let view = store.primary_view();
        store.note_attribute_name_forms(name, forms);
        store.set_tag(node, StyleAtomID(1), &mut memory);

        let before = store.staged_before_facts();
        store.apply_staged(&mut memory);

        assert_eq!(before.attribute_name_forms(name), forms);
        assert_eq!(store.primary().attribute_name_forms(name), forms);
        assert_eq!(
            view.attribute_name_forms(name),
            AttributeNameForms {
                folded_name: name,
                ..AttributeNameForms::default()
            }
        );
    }

    #[test]
    fn repeated_catalog_publication_copies_the_catalog_at_most_once() {
        let mut store = ElementFactStore::new();

        for raw in 1..=128 {
            let atom = StyleAtomID(raw);
            store.set_attribute_value_text(atom, &[raw as u16]);
        }

        assert_eq!(store.attribute_catalog_copies(), 1);
    }

    #[test]
    fn distant_process_atom_ids_allocate_only_touched_catalog_pages() {
        let mut store = ElementFactStore::new();
        let atom = StyleAtomID(1_000_000);
        store.note_attribute_name_forms(atom, AttributeNameForms::default());
        store.set_attribute_value_text(atom, &[1, 2, 3]);
        store.set_language_text(atom, &[4, 5, 6]);
        ElementFactStore::increment_atom_count(&mut store.atom_live_counts, atom);
        store.custom_property_set_ids_by_name.entry(atom.0 as usize).push(1);

        assert_eq!(store.attribute_catalogs.name_forms.values.page_count(), 1);
        assert_eq!(store.attribute_catalogs.value_texts.handles.page_count(), 1);
        assert_eq!(store.attribute_catalogs.language_texts.handles.page_count(), 1);
        assert_eq!(store.atom_live_counts.values.page_count(), 1);
        assert_eq!(store.custom_property_set_ids_by_name.handles.page_count(), 1);
        assert_eq!(size_of_val(&store.atom_live_counts.get(atom.0 as usize).unwrap()), 4);

        let mut directory = SegmentedDispatchBucketDirectory::default();
        directory.insert(atom.0 as usize, DispatchBucketRange { start: 1, length: 1 });
        assert_eq!(directory.ranges.values.page_count(), 1);
    }

    #[test]
    fn staged_fact_rows_use_paged_element_identity_slots() {
        let first = StyleNodeID::element(1);
        let distant = StyleNodeID::element(64);
        let mut rows = FactStaging::default();
        let before = StagedFactRow {
            tag: StyleAtomID(1),
            ..Default::default()
        };
        let after = StagedFactRow {
            tag: StyleAtomID(2),
            ..Default::default()
        };

        rows.insert(distant, before, None);
        rows.insert(first, StagedFactRow::default(), None);
        rows.insert(distant, after, None);

        assert!(rows.has_dirty());
        assert_eq!(rows.rows.page_count(), 2);
        assert_eq!(rows.entries.len(), 2);
        assert_eq!(rows.keys().collect::<Vec<_>>(), vec![distant, first]);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows.get(distant).unwrap().tag, StyleAtomID(2));
        rows.mark_applied();
        assert!(!rows.has_dirty());
        rows.edit(first, |row| row.tag = StyleAtomID(3)).unwrap();
        assert!(rows.has_dirty());
        rows.remove(distant);
        assert!(rows.has_dirty());
        assert_eq!(rows.keys().collect::<Vec<_>>(), vec![first]);
        rows.remove(first);
        assert!(!rows.has_dirty());
        rows.insert(first, StagedFactRow::default(), None);
        assert_eq!(rows.keys().collect::<Vec<_>>(), vec![first]);
        assert_eq!(rows.dirty_rows().count(), 1);
        assert_eq!(rows.capacity_bytes(), rows.recomputed_capacity_bytes());

        let directory_capacity = rows.rows.directory_capacity();
        let entry_capacity = rows.entries.capacity();
        rows.clear();
        assert!(rows.is_empty());
        assert_eq!(rows.rows.page_count(), 2);
        assert_eq!(rows.rows.directory_capacity(), directory_capacity);
        assert_eq!(rows.entries.capacity(), entry_capacity);
    }

    #[test]
    fn staged_fact_rows_release_oversized_entry_arenas() {
        let mut rows = FactStaging::default();
        for index in 1..=1024 {
            rows.insert(StyleNodeID::element(index), StagedFactRow::default(), None);
        }
        rows.clear();
        let peak_capacity = rows.entries.capacity();

        rows.insert(StyleNodeID::element(1), StagedFactRow::default(), None);
        rows.clear();

        assert!(rows.entries.capacity() < peak_capacity);
        assert_eq!(rows.capacity_bytes(), rows.recomputed_capacity_bytes());
    }

    #[test]
    fn states_pack_into_one_word() {
        let mut states = StateSet::default();
        states.insert(StateFact::Hover);
        states.insert(StateFact::Checked);
        assert!(states.contains(StateFact::Hover));
        assert!(states.contains(StateFact::Checked));
        assert!(!states.contains(StateFact::Focus));
        assert_eq!(
            states.facts().collect::<Vec<_>>(),
            vec![StateFact::Checked, StateFact::Hover]
        );
        states.remove(StateFact::Hover);
        assert!(!states.contains(StateFact::Hover));
        assert_eq!(size_of::<StateSet>(), 8);
    }

    #[test]
    fn attribute_facts_keep_only_identity_and_an_optional_text_handle() {
        assert_eq!(size_of::<AttributeFact>(), 16);
    }

    #[test]
    fn selector_feature_keys_are_eight_bytes() {
        assert_eq!(size_of::<FeatureKey>(), 8);
    }

    #[test]
    fn clearing_keeps_the_arena_for_the_next_transaction() {
        let mut facts = StyleNodeFacts::new();
        for index in 1..200_u32 {
            facts.push_row(
                StyleNodeID::element(index),
                StyleAtomID(1),
                StyleAtomID::NONE,
                StateSet::default(),
                &[StyleAtomID(2)],
                &[],
            );
        }
        let bytes = facts.capacity_bytes();
        facts.clear();
        assert_eq!(facts.row_count(), 0);
        assert_eq!(facts.row_of(StyleNodeID::element(5)), None);
        assert_eq!(facts.capacity_bytes(), bytes);
    }
}
