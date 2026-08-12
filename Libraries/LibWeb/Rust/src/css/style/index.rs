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
use super::capacity::capacity_bytes;
use super::column::Column;
use super::column::EpochColumn;
use super::column::advance_epoch;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::FastSet as HashSet;
use std::cell::Cell;
use std::cmp::Reverse;
use std::rc::Rc;

use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::partial_view::Lookup;
use super::prefix::PrefixAutomaton;
use super::program::DeclaredProperty;
use super::program::RuleID;
use super::program::SelectorProgramID;
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

/// Which local fact of a style node is being described.
///
/// Attribute presence and attribute value share one key: changing an attribute changes both facts
/// at once, and splitting them would let a consumer handle one and miss the other.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum FeatureKey {
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
        StateFact::ALL.into_iter().filter(move |&fact| self.contains(fact))
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
/// `value` is the interned value when the value was worth interning. `text` is a range into the
/// batch's UTF-16 blob, present only when an attached selector applies a substring or token
/// operator to this attribute name - the case where an atom cannot answer the test.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AttributeFact {
    pub name: StyleAtomID,
    /// The any-namespace form of `name`, which is the identity `[*|x]` asks about. An element can
    /// carry the same local name in several namespaces, so this is shared between those facts while
    /// `name` is not.
    pub local: StyleAtomID,
    /// The ASCII-lowercase foldings of `name` and `local`, equal to them where the attribute's own
    /// name is already lowercase - which is every attribute of an HTML element in an HTML document,
    /// since both the parser and `setAttribute` fold it. They exist for the selector's sake: `[aB]`
    /// dispatches under the folded form, so an attribute written `aB` has to answer to it as well.
    pub folded_name: StyleAtomID,
    pub folded_local: StyleAtomID,
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

const NO_ROW: u32 = u32::MAX;

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
    nodes: Vec<StyleNodeID>,
    row_by_element_index: Vec<u32>,
    /// Rows no longer reachable through the element mapping. The primary arrangement repoints an
    /// element to a freshly packed row when its facts move; the old row stays as garbage until
    /// its measured carrying cost makes one rebuild cheaper than retaining it.
    stale_rows: u32,
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
    custom_state_offsets: Vec<u32>,
    custom_states: Vec<StyleAtomID>,
    /// The part names the element is exposed under. `::part()` dispatches on one, so a row that did
    /// not carry them would make every such rule unreachable rather than merely slower.
    part_offsets: Vec<u32>,
    parts: Vec<StyleAtomID>,
    /// The host a `::part()` rule addresses the element from, which `exportparts` moves outwards.
    /// The outer compound of such a rule describes that host and not the one the part sits under.
    part_exposure: Vec<StyleAtomID>,

    class_offsets: Vec<u32>,
    classes: Vec<StyleAtomID>,
    attribute_offsets: Vec<u32>,
    attributes: Vec<AttributeFact>,

    /// UTF-16 payloads for the attribute values a string operator has to read. The representation
    /// matches the C++ side's, so nothing is converted on the way in.
    text: Vec<u16>,
}

impl StyleNodeFacts {
    #[must_use]
    pub fn new() -> Self {
        Self {
            generation: next_batch_generation(),
            class_offsets: vec![0],
            attribute_offsets: vec![0],
            custom_state_offsets: vec![0],
            part_offsets: vec![0],
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
        self.custom_state_offsets
            .push(u32::try_from(self.custom_states.len()).expect("custom state payload overflow"));
        self.part_offsets
            .push(u32::try_from(self.parts.len()).expect("part payload overflow"));
        self.part_exposure.push(StyleAtomID::NONE);
        self.classes.extend_from_slice(classes);
        self.class_offsets
            .push(u32::try_from(self.classes.len()).expect("class payload overflow"));
        self.attributes.extend_from_slice(attributes);
        self.attribute_offsets
            .push(u32::try_from(self.attributes.len()).expect("attribute payload overflow"));

        self.map_row(node, row);
    }

    /// Append one row from another packed fact arrangement without materializing an owned row.
    fn push_row_from(&mut self, node: StyleNodeID, source: &Self, source_row: u32) {
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
        self.custom_states
            .extend_from_slice(source.custom_states_of(source_row));
        self.custom_state_offsets
            .push(u32::try_from(self.custom_states.len()).expect("custom state payload overflow"));
        self.parts.extend_from_slice(source.parts_of(source_row));
        self.part_offsets
            .push(u32::try_from(self.parts.len()).expect("part payload overflow"));
        self.part_exposure.push(source.part_exposure_of(source_row));
        self.classes.extend_from_slice(source.classes_of(source_row));
        self.class_offsets
            .push(u32::try_from(self.classes.len()).expect("class payload overflow"));
        for &attribute in source.attributes_of(source_row) {
            let (text_offset, text_length) = source
                .text_of(attribute)
                .map_or((u32::MAX, 0), |text| self.push_text(text));
            self.attributes.push(AttributeFact {
                text_offset,
                text_length,
                ..attribute
            });
        }
        self.attribute_offsets
            .push(u32::try_from(self.attributes.len()).expect("attribute payload overflow"));

        self.map_row(node, row);
    }

    fn map_row(&mut self, node: StyleNodeID, row: u32) {
        let Some(index) = node.element_index() else {
            return;
        };
        let index = index as usize;
        if index >= self.row_by_element_index.len() {
            self.row_by_element_index.resize(index + 1, NO_ROW);
        }
        if self.row_by_element_index[index] != NO_ROW {
            self.stale_rows += 1;
        }
        self.row_by_element_index[index] = row;
    }

    /// Append UTF-16 text for an attribute value and return its range.
    pub fn push_text(&mut self, text: &[u16]) -> (u32, u32) {
        let offset = u32::try_from(self.text.len()).expect("text payload overflow");
        self.text.extend_from_slice(text);
        (offset, u32::try_from(text.len()).expect("text payload overflow"))
    }

    #[must_use]
    pub fn row_count(&self) -> usize {
        self.nodes.len()
    }

    #[must_use]
    pub fn node_at(&self, row: u32) -> StyleNodeID {
        self.nodes[row as usize]
    }

    /// The row holding `node`'s facts, or `None` when the batch does not cover it. A miss is not a
    /// negative answer: it means the evaluator has to widen or ask for a different batch.
    /// Detach one element's row from the mapping, leaving the packed row as garbage.
    pub fn forget_row(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index() else {
            return;
        };
        if let Some(slot) = self.row_by_element_index.get_mut(index as usize)
            && *slot != NO_ROW
        {
            *slot = NO_ROW;
            self.stale_rows += 1;
        }
    }

    #[must_use]
    pub fn stale_rows(&self) -> u32 {
        self.stale_rows
    }

    #[must_use]
    pub fn generation(&self) -> u64 {
        self.generation
    }

    #[must_use]
    pub fn row_of(&self, node: StyleNodeID) -> Option<u32> {
        let index = node.element_index()? as usize;
        match self.row_by_element_index.get(index) {
            Some(&NO_ROW) | None => None,
            Some(&row) => Some(row),
        }
    }

    fn live_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.nodes
            .iter()
            .copied()
            .enumerate()
            .filter_map(|(row, node)| (self.row_of(node) == u32::try_from(row).ok()).then_some(node))
    }

    /// Record the folded name of the row just pushed. Only a name that is not already lowercase
    /// has one.
    pub fn set_row_folded_tag(&mut self, folded: StyleAtomID) {
        let row = self.folded_tag.len() - 1;
        self.folded_tag[row] = folded;
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
        self.custom_states.extend_from_slice(custom_states);
        let end = u32::try_from(self.custom_states.len()).expect("custom state payload overflow");
        *self.custom_state_offsets.last_mut().expect("a row was pushed") = end;
        self.parts.extend_from_slice(parts);
        let end = u32::try_from(self.parts.len()).expect("part payload overflow");
        *self.part_offsets.last_mut().expect("a row was pushed") = end;
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
        &self.text[offset as usize..(offset + length) as usize]
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
        self.heading_level[row as usize]
    }

    #[must_use]
    pub fn part_exposure_of(&self, row: u32) -> StyleAtomID {
        self.part_exposure[row as usize]
    }

    #[must_use]
    pub fn custom_states_of(&self, row: u32) -> &[StyleAtomID] {
        let start = self.custom_state_offsets[row as usize] as usize;
        let end = self.custom_state_offsets[row as usize + 1] as usize;
        &self.custom_states[start..end]
    }

    #[must_use]
    pub fn parts_of(&self, row: u32) -> &[StyleAtomID] {
        let start = self.part_offsets[row as usize] as usize;
        let end = self.part_offsets[row as usize + 1] as usize;
        &self.parts[start..end]
    }

    #[must_use]
    pub fn classes_of(&self, row: u32) -> &[StyleAtomID] {
        let start = self.class_offsets[row as usize] as usize;
        let end = self.class_offsets[row as usize + 1] as usize;
        &self.classes[start..end]
    }

    #[must_use]
    pub fn attributes_of(&self, row: u32) -> &[AttributeFact] {
        let start = self.attribute_offsets[row as usize] as usize;
        let end = self.attribute_offsets[row as usize + 1] as usize;
        &self.attributes[start..end]
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
            for other in [attribute.local, attribute.folded_name, attribute.folded_local] {
                if !other.is_none() && other != attribute.name {
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
    pub fn dispatch_bloom_of(&self, row: u32, is_document_root: bool) -> u64 {
        let mut bloom = 0;
        self.for_each_dispatch_key(row, is_document_root, |key| bloom |= dispatch_bloom_bit(key));
        bloom
    }

    #[must_use]
    pub fn carries_dispatch_key(&self, row: u32, key: DispatchKey, is_root: bool) -> bool {
        match key {
            DispatchKey::Part(part) => self.parts_of(row).contains(&part),
            DispatchKey::CustomState(state) => self.custom_states_of(row).contains(&state),
            DispatchKey::Id(id) => self.id_of(row) == id,
            DispatchKey::Class(class) => self.classes_of(row).contains(&class),
            DispatchKey::AttributeName(name) => self.attributes_of(row).iter().any(|attribute| {
                attribute.name == name
                    || attribute.local == name
                    || attribute.folded_name == name
                    || attribute.folded_local == name
            }),
            DispatchKey::TagName(tag) => self.tag_of(row) == tag || self.folded_tag_of(row) == tag,
            DispatchKey::Directionality(direction) => self.directionality_of(row) == direction,
            DispatchKey::Root => is_root,
            DispatchKey::State(state) => self.states_of(row).contains(state),
            DispatchKey::Heading => self.heading_level_of(row) != 0,
            DispatchKey::Universal => true,
        }
    }

    #[must_use]
    pub fn text_of(&self, attribute: AttributeFact) -> Option<&[u16]> {
        if attribute.text_length == 0 && attribute.text_offset == u32::MAX {
            return None;
        }
        let start = attribute.text_offset as usize;
        let end = start + attribute.text_length as usize;
        self.text.get(start..end)
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
        let attribute_text_bytes = self
            .attributes_of(row)
            .iter()
            .map(|attribute| attribute.text_length as usize * size_of::<u16>())
            .sum::<usize>();
        (fixed
            + size_of_val(self.custom_states_of(row))
            + size_of_val(self.parts_of(row))
            + size_of_val(self.classes_of(row))
            + size_of_val(self.attributes_of(row))
            + attribute_text_bytes) as u64
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
        self.part_offsets.clear();
        self.part_offsets.push(0);
        self.part_exposure.clear();
        self.custom_state_offsets.clear();
        self.custom_state_offsets.push(0);
        self.classes.clear();
        self.class_offsets.clear();
        self.class_offsets.push(0);
        self.attributes.clear();
        self.attribute_offsets.clear();
        self.attribute_offsets.push(0);
        self.text.clear();
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.nodes,
                self.row_by_element_index,
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
                self.custom_state_offsets,
                self.custom_states,
                self.parts,
                self.part_offsets,
                self.part_exposure,
                self.class_offsets,
                self.classes,
                self.attribute_offsets,
                self.attributes,
                self.text,
            ];
            cached [];
            nested [];
            skip [self.stale_rows];
        }
    }
}

/// Members per posting chunk. Insertion and removal shift at most this many identities, so a
/// posting stays cheap to maintain without giving up sorted order.
const MAX_POSTING_CHUNK: usize = 256;

/// One feature's candidate set: chunked and sorted by `StyleNodeID`.
#[derive(Default)]
pub(super) struct Posting {
    id: PostingID,
    chunks: Vec<Vec<StyleNodeID>>,
    length: usize,
}

impl Posting {
    pub(super) fn candidates(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.chunks.iter().flat_map(|chunk| chunk.iter().copied())
    }

    #[must_use]
    pub(super) fn len(&self) -> usize {
        self.length
    }

    #[must_use]
    pub(super) fn id(&self) -> PostingID {
        self.id
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
            match chunk.binary_search(&node) {
                Ok(_) => return None,
                Err(position) => chunk.insert(position, node),
            }
            (chunk.len() > MAX_POSTING_CHUNK).then(|| chunk.split_off(chunk.len() / 2))
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
            skip [self.id, self.length];
        }
    }
}

/// A selector posting's key: a matchable feature and the atom that identifies it.
///
/// Distinct from [`FeatureKey`], which names a *fact of an element* - "this element's tag" - where
/// a posting names a *set of elements* - "the elements whose tag is div". The fact needs no atom
/// because its value carries one; the posting needs one because it is the value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum SelectorPostingKey {
    Part(StyleAtomID),
    /// An element in one custom state, which `:state()` tests.
    CustomState(StyleAtomID),
    Tag(StyleAtomID),
    Id(StyleAtomID),
    Class(StyleAtomID),
    AttributeName(StyleAtomID),
    /// An element with this resolved directionality.
    Directionality(StyleAtomID),
}

/// A dependency posting's key: computed-style use which lets a named rule find its consumers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum DependencyPostingKey {
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

/// The shared physical posting store's disjoint selector and dependency vocabularies.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum PostingKey {
    Selector(SelectorPostingKey),
    Dependency(DependencyPostingKey),
}

define_id! {
    /// Dense identity of one live feature posting for the current posting generation.
    default pub(super) struct PostingID();
}

impl PostingID {
    pub(super) fn index(self) -> usize {
        self.0 as usize
    }
}

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
    dense_ids_are_current: bool,
    residency: MemoryLease,
    missing: HashSet<PostingKey>,
    benefit_hits: Cell<u64>,
    benefit_misses: Cell<u64>,
}

impl Default for FeaturePostings {
    fn default() -> Self {
        Self {
            postings: HashMap::default(),
            dense_ids_are_current: true,
            residency: MemoryLease::new(MemoryCategory::FeaturePosting),
            missing: HashSet::default(),
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

    /// Add `node` to a feature's candidate set. Returns false when the Tier-3 budget refused the
    /// growth, in which case the posting is dropped entirely rather than left partial - a posting
    /// that silently lost members would be a wrong answer, not a slower one. Adding a node that is
    /// already a member changes nothing and succeeds.
    pub fn insert(&mut self, key: PostingKey, node: StyleNodeID, memory: &mut MemoryController) -> bool {
        if self.missing.contains(&key) {
            return false;
        }
        let posting = match self.postings.entry(key) {
            std::collections::hash_map::Entry::Occupied(entry) => entry.into_mut(),
            std::collections::hash_map::Entry::Vacant(entry) => {
                self.dense_ids_are_current = false;
                entry.insert(Posting::default())
            }
        };
        let Some(growth) = posting.insert(node) else {
            return true;
        };
        if growth != 0 && !self.residency.grow(memory, growth).is_granted() {
            let resident_bytes = posting.capacity_bytes() - growth;
            self.postings.remove(&key);
            self.residency.shrink_to(self.residency.bytes() - resident_bytes);
            self.remember_missing(key);
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
            None if self.missing.contains(&key) => Lookup::Missing(key),
            None => Lookup::KnownAbsent,
        };
        self.record_benefit_lookup(!matches!(result, Lookup::Missing(_)));
        result
    }

    /// Assign transaction-local direct-column identities to the current live postings.
    pub(super) fn ensure_dense_ids(&mut self) {
        if self.dense_ids_are_current {
            return;
        }
        for (index, posting) in self.postings.values_mut().enumerate() {
            posting.id = PostingID(u32::try_from(index).expect("feature posting identity space exhausted"));
        }
        self.dense_ids_are_current = true;
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
        self.dense_ids_are_current = false;
        self.residency
            .shrink_to(self.residency.bytes() - posting.capacity_bytes());
    }

    /// Discard every posting. Memory pressure reduces retained acceleration, never correctness.
    pub fn evict_all(&mut self) {
        let keys: Vec<PostingKey> = self.postings.keys().copied().collect();
        for key in keys {
            self.remember_missing(key);
        }
        let released = self.postings.values().map(Posting::capacity_bytes).sum::<u64>();
        self.residency.shrink_to(self.residency.bytes() - released);
        self.postings = HashMap::default();
        self.dense_ids_are_current = true;
    }

    #[must_use]
    fn is_incomplete(&self) -> bool {
        !self.missing.is_empty()
    }

    fn take_rebuilt(&mut self, mut rebuilt: FeaturePostings) {
        debug_assert!(rebuilt.missing.is_empty());
        let rebuilt_bytes = rebuilt.residency.bytes();
        self.postings.extend(rebuilt.postings.drain());
        rebuilt.residency.release();
        self.residency.grow_committed(rebuilt_bytes);

        let released = self.missing_capacity_bytes();
        self.missing = HashSet::default();
        self.residency.shrink_to(self.residency.bytes() - released);
        self.dense_ids_are_current = false;
    }

    #[must_use]
    #[cfg(test)]
    pub fn feature_count(&self) -> usize {
        self.postings.len()
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
}

/// The rightmost distinguishing feature of one selector entry: the dispatch key a candidate is
/// probed against.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum DispatchKey {
    /// `::part(label)`: a part name the element exposes.
    Part(StyleAtomID),
    /// `:state(name)`: a custom state the element is in.
    CustomState(StyleAtomID),
    Id(StyleAtomID),
    Class(StyleAtomID),
    AttributeName(StyleAtomID),
    TagName(StyleAtomID),
    /// `:dir(value)`: the directionality the element must resolve to.
    Directionality(StyleAtomID),
    /// `:root`: the one element of the document that has no parent.
    Root,
    /// One pseudo-class state the subject must be in. Most of them hold on almost no element, so
    /// this is where a `:hover` or a `:link` rule belongs rather than in front of every candidate.
    State(StateFact),
    /// `:heading` and `:heading(n)`: the subject is a heading of some level.
    Heading,
    /// The entry has no selective rightmost feature, so every candidate has to consider it.
    Universal,
}

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
    };
    let mut hash = value ^ kind.wrapping_mul(0x9e37_79b9_7f4a_7c15);
    hash = (hash ^ (hash >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    hash = (hash ^ (hash >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    hash ^ (hash >> 31)
}

/// One attached selector entry, reachable from its dispatch key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DispatchEntry {
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

define_id! {
    /// Stable identity of one entry in a selector dispatch.
    pub(super) struct DispatchEntryID();
}

impl DispatchEntryID {
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
    candidates: Vec<DispatchEntryID>,
    epoch: u32,
}

#[derive(Clone, Copy)]
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
            epoch: 0,
        }
    }

    fn begin(&mut self, entry_count: usize) {
        self.seen_at_epoch.ensure_len(entry_count);
        self.candidates.clear();
        advance_epoch(&mut self.epoch, 1, &mut [&mut self.seen_at_epoch]);
    }

    fn admit(&mut self, id: DispatchEntryID) -> bool {
        self.seen_at_epoch.mark(id.index(), self.epoch)
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.seen_at_epoch, self.candidates];
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

/// Buckets attached selector entries by their rightmost distinguishing feature.
///
/// A candidate probes only the buckets its own facts name, plus the universal bucket, so a document
/// full of `.item` elements never considers a rule whose subject compound requires `#header`. This
/// is program-derived dispatch rather than acceleration over elements: it is Tier 2, it is rebuilt
/// with the program, and it is never evicted independently of it.
#[derive(Clone, Default)]
struct AncestorDispatchTopology {
    key_indices: HashMap<DispatchKey, u32>,
}

#[derive(Default)]
struct RuleDispatchTopology {
    buckets: HashMap<DispatchKey, Vec<DispatchEntryID>>,
    /// Universal-subject entries that have no exact parent requirement.
    universal_without_parent_filter: Vec<DispatchEntryID>,
    /// Universal-subject entries indexed by the one feature their parent must carry.
    universal_by_parent: HashMap<DispatchKey, Vec<DispatchEntryID>>,
    /// The same parent-filtered entries as a conservative fallback when parent facts are absent.
    universal_with_parent_filter: Vec<DispatchEntryID>,
    /// Entries the top-down prefix automaton cannot answer, indexed separately so its successful
    /// path does not enumerate the old exact candidates merely to discard them.
    non_prefix_buckets: HashMap<DispatchKey, Vec<DispatchEntryID>>,
    non_prefix_universal_without_parent_filter: Vec<DispatchEntryID>,
    non_prefix_universal_by_parent: HashMap<DispatchKey, Vec<DispatchEntryID>>,
    non_prefix_universal_with_parent_filter: Vec<DispatchEntryID>,
    ancestors: Rc<AncestorDispatchTopology>,
    prefixes: PrefixAutomaton,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) struct AncestorDispatchTopologyID(*const AncestorDispatchTopology);

#[derive(Default)]
pub struct RuleDispatch {
    entries: Vec<DispatchEntry>,
    /// Direct cascade-order projection for every rule represented in this dispatch. Rule
    /// identities are program indices, so retained answers can restore an entry's order without
    /// searching the dispatch's much larger candidate table. Sparse pages keep a scope containing
    /// one late-created rule from allocating rows for every preceding rule in the document.
    cascade_order_rule_pages: Vec<Option<Box<[CascadeOrderRule; CASCADE_ORDER_RULE_PAGE_SIZE]>>>,
    cascade_orders_by_rule_entry: Vec<u32>,
    cascade_properties: Vec<u16>,
    cascade_entries: Vec<CascadeEntryData>,
    topology: Rc<RuleDispatchTopology>,
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

    pub(super) fn rebind_rules(template: &Self, rules: &[RuleID]) -> Self {
        assert_eq!(template.entries.len(), rules.len());
        let mut entries = template.entries.clone();
        for (entry, &rule) in entries.iter_mut().zip(rules) {
            entry.rule = rule;
            entry.cascade_order = 0;
        }
        Self {
            entries,
            cascade_order_rule_pages: Vec::new(),
            cascade_orders_by_rule_entry: Vec::new(),
            cascade_properties: Vec::new(),
            cascade_entries: Vec::new(),
            topology: Rc::clone(&template.topology),
        }
    }

    pub(super) fn rebind_rules_for_extension(template: &Self, rules: &[RuleID]) -> Self {
        let mut dispatch = Self::rebind_rules(template, rules);
        let topology = &template.topology;
        dispatch.topology = Rc::new(RuleDispatchTopology {
            buckets: topology.buckets.clone(),
            universal_without_parent_filter: topology.universal_without_parent_filter.clone(),
            universal_by_parent: topology.universal_by_parent.clone(),
            universal_with_parent_filter: topology.universal_with_parent_filter.clone(),
            non_prefix_buckets: topology.non_prefix_buckets.clone(),
            non_prefix_universal_without_parent_filter: topology.non_prefix_universal_without_parent_filter.clone(),
            non_prefix_universal_by_parent: topology.non_prefix_universal_by_parent.clone(),
            non_prefix_universal_with_parent_filter: topology.non_prefix_universal_with_parent_filter.clone(),
            ancestors: Rc::new((*topology.ancestors).clone()),
            prefixes: topology.prefixes.clone(),
        });
        dispatch.topology_mut().prefixes.prepare_to_extend();
        dispatch
    }

    #[cfg(test)]
    pub(super) fn shares_topology_with(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.topology, &other.topology)
    }

    pub(super) fn shares_ancestor_topology_with(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.topology.ancestors, &other.topology.ancestors)
    }

    pub(super) fn ancestor_topology_id(&self) -> AncestorDispatchTopologyID {
        AncestorDispatchTopologyID(Rc::as_ptr(&self.topology.ancestors))
    }

    pub(super) fn ancestor_dispatch_shape(&self) -> AncestorDispatchShape {
        let mut keys: Vec<_> = self
            .topology
            .ancestors
            .key_indices
            .iter()
            .map(|(&key, &index)| (index, key))
            .collect();
        keys.sort_unstable_by_key(|&(index, _)| index);
        AncestorDispatchShape(keys.into_iter().map(|(_, key)| key).collect())
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

    pub(super) fn insert(&mut self, key: DispatchKey, mut entry: DispatchEntry) -> DispatchEntryID {
        entry.required_ancestor_index = entry.required_ancestor.map(|required| {
            let topology = self.topology_mut();
            let ancestors = Rc::get_mut(&mut topology.ancestors).expect("a shared ancestor topology is immutable");
            let next = u32::try_from(ancestors.key_indices.len()).expect("ancestor requirement space exhausted");
            *ancestors.key_indices.entry(required).or_insert(next)
        });
        let id = DispatchEntryID::from_index(self.entries.len());
        self.entries.push(entry);
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
        selector_entry: u32,
        chain: &[super::selector::SelectorPrefixStep],
        entry: DispatchEntryID,
        structural_tests_admissible: bool,
    ) {
        // Registration can refuse a chain whose structural tests would overflow the automaton's
        // truth bit space or whose origin does not admit them; the entry then stays a candidate
        // for the exact evaluator.
        if self.topology_mut().prefixes.add_entry(
            programs,
            program,
            selector_entry,
            chain,
            entry,
            structural_tests_admissible,
        ) {
            self.entries[entry.index()].prefix_matched = true;
        }
    }

    pub(super) fn finish_prefixes(&mut self) {
        self.topology_mut().prefixes.finish();
        self.rebuild_non_prefix_index();
    }

    #[must_use]
    pub(super) fn prefixes(&self) -> &PrefixAutomaton {
        &self.topology.prefixes
    }

    #[must_use]
    pub(super) fn entry(&self, id: DispatchEntryID) -> DispatchEntry {
        self.entries[id.index()]
    }

    #[must_use]
    pub(super) fn entries(&self) -> &[DispatchEntry] {
        &self.entries
    }

    pub(super) fn cascade_orders_in_identity_order(
        &self,
    ) -> impl Iterator<Item = (RuleID, SelectorProgramID, u32, u32)> + '_ {
        self.cascade_order_rule_pages
            .iter()
            .enumerate()
            .flat_map(move |(page_index, page)| {
                page.as_deref().into_iter().flat_map(move |page| {
                    page.iter()
                        .enumerate()
                        .filter(|(_, rule)| rule.entry_count != 0)
                        .flat_map(move |(slot, rule)| {
                            (0..rule.entry_count).map(move |entry| {
                                let rule_index = page_index * CASCADE_ORDER_RULE_PAGE_SIZE + slot;
                                let order_index = rule.entry_start as usize + entry as usize;
                                (
                                    RuleID(u32::try_from(rule_index).expect("rule identity space exhausted")),
                                    rule.program,
                                    entry,
                                    self.cascade_orders_by_rule_entry[order_index],
                                )
                            })
                        })
                })
            })
    }

    fn index_universal_entry(&mut self, id: DispatchEntryID) {
        let entry = self.entries[id.index()];
        let topology = self.topology_mut();
        match entry.required_parent {
            Some(parent) => {
                topology.universal_by_parent.entry(parent).or_default().push(id);
                topology.universal_with_parent_filter.push(id);
            }
            None => topology.universal_without_parent_filter.push(id),
        }
    }

    fn rebuild_universal_index(&mut self) {
        let entries = self.bucket_ids(DispatchKey::Universal).to_vec();
        let topology = self.topology_mut();
        topology.universal_without_parent_filter.clear();
        topology.universal_by_parent.clear();
        topology.universal_with_parent_filter.clear();
        for id in entries {
            self.index_universal_entry(id);
        }
    }

    fn rebuild_non_prefix_index(&mut self) {
        let mut non_prefix_buckets = HashMap::default();
        for (&key, entries) in &self.topology.buckets {
            let entries: Vec<_> = entries
                .iter()
                .copied()
                .filter(|entry| !self.entries[entry.index()].prefix_matched)
                .collect();
            if !entries.is_empty() {
                non_prefix_buckets.insert(key, entries);
            }
        }

        let entries = non_prefix_buckets
            .get(&DispatchKey::Universal)
            .cloned()
            .unwrap_or_default();
        let topology = self.topology_mut();
        topology.non_prefix_buckets = non_prefix_buckets;
        topology.non_prefix_universal_without_parent_filter.clear();
        topology.non_prefix_universal_by_parent.clear();
        topology.non_prefix_universal_with_parent_filter.clear();
        for id in entries {
            let entry = self.entries[id.index()];
            match entry.required_parent {
                Some(parent) => {
                    let topology = self.topology_mut();
                    topology
                        .non_prefix_universal_by_parent
                        .entry(parent)
                        .or_default()
                        .push(id);
                    topology.non_prefix_universal_with_parent_filter.push(id);
                }
                None => self.topology_mut().non_prefix_universal_without_parent_filter.push(id),
            }
        }
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
        let mut ordered: Vec<(K, RuleID, SelectorProgramID, u32, DispatchEntryID)> = self
            .entries
            .iter()
            .copied()
            .enumerate()
            .map(|(index, entry)| {
                (
                    priority_of(entry),
                    entry.rule,
                    entry.program,
                    entry.entry,
                    DispatchEntryID::from_index(index),
                )
            })
            .collect();
        ordered.sort_unstable();

        let mut group_start = 0;
        while group_start < ordered.len() {
            let mut group_end = group_start + 1;
            while group_end < ordered.len()
                && ordered[group_end].0 == ordered[group_start].0
                && ordered[group_end].1 == ordered[group_start].1
                && ordered[group_end].2 == ordered[group_start].2
                && ordered[group_end].3 == ordered[group_start].3
            {
                group_end += 1;
            }
            let cascade_order = u32::try_from(group_end - 1).expect("dispatch entry space exhausted");
            for ordered_entry in &ordered[group_start..group_end] {
                self.entries[ordered_entry.4.index()].cascade_order = cascade_order;
            }
            group_start = group_end;
        }

        self.rebuild_cascade_order_projection();
    }

    pub(super) fn reuse_cascade_order(&mut self, template: &Self) {
        assert_eq!(self.entries.len(), template.entries.len());
        for (entry, template_entry) in self.entries.iter_mut().zip(&template.entries) {
            assert_eq!(entry.program, template_entry.program);
            assert_eq!(entry.entry, template_entry.entry);
            entry.cascade_order = template_entry.cascade_order;
        }
        self.rebuild_cascade_order_projection();
    }

    fn rebuild_cascade_order_projection(&mut self) {
        let mut entries_by_identity: Vec<_> = (0..self.entries.len()).map(DispatchEntryID::from_index).collect();
        entries_by_identity.sort_unstable_by_key(|&id| {
            let entry = self.entries[id.index()];
            (entry.rule, entry.program, entry.entry)
        });
        entries_by_identity.dedup_by_key(|id| {
            let entry = self.entries[id.index()];
            (entry.rule, entry.program, entry.entry)
        });
        self.cascade_order_rule_pages.clear();
        self.cascade_orders_by_rule_entry.clear();
        self.cascade_orders_by_rule_entry.reserve(entries_by_identity.len());
        for id in entries_by_identity {
            let entry = self.entries[id.index()];
            let rule_index = entry.rule.0 as usize;
            let page_index = rule_index / CASCADE_ORDER_RULE_PAGE_SIZE;
            if self.cascade_order_rule_pages.len() <= page_index {
                self.cascade_order_rule_pages.resize_with(page_index + 1, || None);
            }
            let page = self.cascade_order_rule_pages[page_index]
                .get_or_insert_with(|| Box::new([CascadeOrderRule::default(); CASCADE_ORDER_RULE_PAGE_SIZE]));
            let rule = &mut page[rule_index % CASCADE_ORDER_RULE_PAGE_SIZE];
            if rule.entry_count == 0 {
                rule.program = entry.program;
                rule.entry_start =
                    u32::try_from(self.cascade_orders_by_rule_entry.len()).expect("dispatch entry space exhausted");
            }
            assert_eq!(
                rule.program, entry.program,
                "one rule cannot have multiple selector programs"
            );
            assert_eq!(
                rule.entry_count, entry.entry,
                "a rule's selector entries must form a dense identity space"
            );
            rule.entry_count = rule.entry_count.checked_add(1).expect("selector entry space exhausted");
            self.cascade_orders_by_rule_entry.push(entry.cascade_order);
        }
        if Rc::strong_count(&self.topology) == 1 {
            self.rebuild_universal_index();
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
    pub fn assign_cascade_properties(
        &mut self,
        mut blocks_pruning: impl FnMut(DispatchEntry) -> bool,
        mut properties_of: impl FnMut(DispatchEntry) -> Option<Vec<u16>>,
    ) {
        self.cascade_properties.clear();
        self.cascade_entries.clear();
        self.cascade_entries
            .resize(self.entries.len(), CascadeEntryData::default());
        let mut configured = vec![false; self.entries.len()];
        for &entry in &self.entries {
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

    fn bucket_ids(&self, key: DispatchKey) -> &[DispatchEntryID] {
        self.topology.buckets.get(&key).map_or(&[], Vec::as_slice)
    }

    pub fn bucket(&self, key: DispatchKey) -> impl ExactSizeIterator<Item = DispatchEntry> + '_ {
        self.bucket_ids(key).iter().map(|&id| self.entries[id.index()])
    }

    #[must_use]
    pub fn entry_count(&self) -> usize {
        self.entries.len()
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
        workspace.begin(self.entries.len());
        let (buckets, universal_without_parent_filter, universal_by_parent, universal_with_parent_filter) =
            match entries {
                CandidateEntries::All => (
                    &self.topology.buckets,
                    &self.topology.universal_without_parent_filter,
                    &self.topology.universal_by_parent,
                    &self.topology.universal_with_parent_filter,
                ),
                CandidateEntries::NonPrefix => (
                    &self.topology.non_prefix_buckets,
                    &self.topology.non_prefix_universal_without_parent_filter,
                    &self.topology.non_prefix_universal_by_parent,
                    &self.topology.non_prefix_universal_with_parent_filter,
                ),
            };
        let subject_bloom = facts.dispatch_bloom_of(row, is_document_root);
        {
            let mut offer = |id: DispatchEntryID, attribute_value: Option<StyleAtomID>| {
                let entry = self.entries[id.index()];
                if !entry.required_attribute_value.is_none() && attribute_value != Some(entry.required_attribute_value)
                {
                    return;
                }
                if !workspace.admit(id) {
                    return;
                }
                if !match (entry.required_parent, parent) {
                    (None, _) => true,
                    (Some(_), ParentDispatchFacts::NoElementParent) => false,
                    (Some(required), ParentDispatchFacts::Known { row, is_document_root }) => {
                        facts.carries_dispatch_key(row, required, is_document_root)
                    }
                    (Some(_), ParentDispatchFacts::Unknown) => true,
                } {
                    return;
                }
                if entry.required_subject_bloom & subject_bloom != entry.required_subject_bloom {
                    return;
                }
                if !match (entry.required_ancestor_index, ancestors) {
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
                        if let Some(ids) = universal_by_parent.get(&key) {
                            for &id in ids {
                                offer(id, None);
                            }
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
                if let Some(ids) = buckets.get(&key) {
                    for &id in ids {
                        offer(id, attribute_value);
                    }
                }
            });
        }
        if descending_cascade_order {
            workspace.candidates.sort_unstable_by_key(|&id| {
                let entry = self.entries[id.index()];
                Reverse((self.cascade_pruning_blocker(entry), entry.cascade_order))
            });
        }
        workspace.candidates.iter().map(|&id| self.entries[id.index()])
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        let scope_bytes = capacity_bytes! {
            shallow [
                self.entries,
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
            ];
            skip [];
        };
        let topology_bytes = capacity_bytes! {
            shallow [
                self.topology.buckets,
                self.topology.universal_without_parent_filter,
                self.topology.universal_by_parent,
                self.topology.universal_with_parent_filter,
                self.topology.non_prefix_buckets,
                self.topology.non_prefix_universal_without_parent_filter,
                self.topology.non_prefix_universal_by_parent,
                self.topology.non_prefix_universal_with_parent_filter,
                self.topology.ancestors.key_indices,
            ];
            cached [];
            nested [
                self.topology
                .buckets
                .values()
                .map(|bucket| bucket.capacity() * size_of::<DispatchEntryID>())
                .sum::<usize>(),
                self.topology
                .universal_by_parent
                .values()
                .map(|bucket| bucket.capacity() * size_of::<DispatchEntryID>())
                .sum::<usize>(),
                self.topology
                .non_prefix_buckets
                .values()
                .map(|bucket| bucket.capacity() * size_of::<DispatchEntryID>())
                .sum::<usize>(),
                self.topology
                .non_prefix_universal_by_parent
                .values()
                .map(|bucket| bucket.capacity() * size_of::<DispatchEntryID>())
                .sum::<usize>(),
                self.topology.prefixes.capacity_bytes(),
            ];
            skip [];
        };
        scope_bytes + topology_bytes
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
    complete: [bool; ElementDeclarationKind::COUNT],
}

impl Default for ElementDeclarationRow {
    fn default() -> Self {
        Self {
            by_kind: Default::default(),
            complete: [true; ElementDeclarationKind::COUNT],
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
                .sum::<usize>()) as u64
    }

    fn is_empty(&self) -> bool {
        self.by_kind.iter().all(Option::is_none) && self.complete.iter().all(|&complete| complete)
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
            row.complete[kind.index()],
        )
    }

    fn set(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declared: Vec<DeclaredProperty>,
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
        row.by_kind[kind.index()] = (!declared.is_empty()).then(|| declared.into_boxed_slice());
        row.complete[kind.index()] = declarations_are_complete;
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
        row.complete[kind.index()] = true;
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
    /// Required primary arrangement. Element identity selects the current immutable packed row;
    /// mutations append one replacement per touched node when their publication batch settles.
    rows: StyleNodeFacts,
    pending_rows: HashMap<StyleNodeID, PendingFactRow>,
    selector_query_before_rows: HashMap<StyleNodeID, PendingFactRow>,
    metadata: Column<Option<ElementFactMetadata>>,
    /// Logical row bytes still reachable through the directory, and bytes carried only by stale
    /// rows. Their ratio decides when rebuilding costs less than retaining append garbage.
    primary_live_bytes: u64,
    primary_stale_bytes: u64,
    /// Candidate sets over the same facts. A rule arriving needs to find the elements it could
    /// match without walking the document, and this is what answers that.
    postings: FeaturePostings,
    memory: MemoryLease,
    memory_dirty: bool,
    /// One entry per distinct set of declared custom property names, and the sets each name is in.
    /// A theme is one set however many elements it decides for, which is what keeps the index
    /// proportional to the stylesheet rather than to the document times the stylesheet.
    custom_property_name_sets: super::intern_table::InternTable<CustomPropertyNameSetID, Vec<StyleAtomID>>,
    custom_property_name_set_vacancies: Vec<u32>,
    custom_property_set_ids_by_name: HashMap<StyleAtomID, Vec<u32>>,
    /// The text of each language atom. Nearly every element on a page resolves to the same handful
    /// of languages, so the tag is held once per language rather than once per element.
    language_texts: HashMap<StyleAtomID, Vec<u16>>,
    /// The text of each attribute-value atom, for the operators an atom comparison cannot answer.
    attribute_value_texts: HashMap<StyleAtomID, Vec<u16>>,
    /// The any-namespace atom of each attribute-name atom.
    ///
    /// An attribute is keyed by the name that is unique to it - its qualified atom, or its bare local
    /// name where it is in no namespace - because an element can carry the same local name in several
    /// namespaces at once and each of those is a fact of its own. `[*|x]` asks about all of them
    /// together, and the any-namespace atom is the identity they share, so it is held per name rather
    /// than per element: one name has one local form however many elements carry it.
    attribute_name_locals: HashMap<StyleAtomID, AttributeNameForms>,
    /// The longhand properties each element declares itself, by the kind of declaration they came
    /// from. An element-attached declaration beats every rule in its context, so the cascade needs
    /// to know which properties one covers just as it does for a rule.
    element_declared_properties: ElementDeclarationRows,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct PendingFactRow {
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

impl PendingFactRow {
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
        Self {
            rows: StyleNodeFacts::new(),
            pending_rows: HashMap::default(),
            selector_query_before_rows: HashMap::default(),
            metadata: Column::default(),
            primary_live_bytes: 0,
            primary_stale_bytes: 0,
            postings: FeaturePostings::default(),
            memory: MemoryLease::new(MemoryCategory::StyleNodeMapping),
            memory_dirty: false,
            custom_property_name_sets: super::intern_table::InternTable::default(),
            custom_property_name_set_vacancies: Vec::new(),
            custom_property_set_ids_by_name: HashMap::default(),
            language_texts: HashMap::default(),
            attribute_value_texts: HashMap::default(),
            attribute_name_locals: HashMap::default(),
            element_declared_properties: ElementDeclarationRows::default(),
        }
    }
}

impl ElementFactStore {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.rows.row_count() - self.rows.stale_rows() as usize
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
        !self.postings.missing.contains(&key) || rebuilt.insert(key, node, memory)
    }

    fn rebuild_missing_postings(&mut self, memory: &mut MemoryController) {
        if !self.postings.is_incomplete() {
            return;
        }

        let mut rebuilt = FeaturePostings::new();
        for node in self.rows.live_nodes() {
            let row = self.rows.row_of(node).expect("a live node must have a fact row");
            for (atom, key) in [
                (self.rows.tag_of(row), SelectorPostingKey::Tag(self.rows.tag_of(row))),
                (
                    self.rows.folded_tag_of(row),
                    SelectorPostingKey::Tag(self.rows.folded_tag_of(row)),
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
                if !self.rebuild_missing_posting(&mut rebuilt, PostingKey::Selector(key), node, memory) {
                    return;
                }
            }
            for &part in self.rows.parts_of(row) {
                let key = PostingKey::Selector(SelectorPostingKey::Part(part));
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return;
                }
            }
            for &state in self.rows.custom_states_of(row) {
                let key = PostingKey::Selector(SelectorPostingKey::CustomState(state));
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return;
                }
            }
            for &class in self.rows.classes_of(row) {
                let key = PostingKey::Selector(SelectorPostingKey::Class(class));
                if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                    return;
                }
            }
            for attribute in self.rows.attributes_of(row) {
                for name in self.attribute_name_keys(attribute.name) {
                    let key = PostingKey::Selector(SelectorPostingKey::AttributeName(name));
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return;
                    }
                }
            }
            if let Some(metadata) = self.metadata_of(node) {
                for &name in &metadata.animation_names {
                    let key = PostingKey::Dependency(DependencyPostingKey::AnimationName(name));
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return;
                    }
                }
                if metadata.custom_property_set != 0 {
                    let key =
                        PostingKey::Dependency(DependencyPostingKey::CustomPropertySet(metadata.custom_property_set));
                    if !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return;
                    }
                }
                for (uses, key) in [
                    (
                        metadata.uses_unnamed_custom_properties,
                        PostingKey::Dependency(DependencyPostingKey::AnyCustomProperty),
                    ),
                    (
                        metadata.uses_custom_functions,
                        PostingKey::Dependency(DependencyPostingKey::AnyCustomFunction),
                    ),
                ] {
                    if uses && !self.rebuild_missing_posting(&mut rebuilt, key, node, memory) {
                        return;
                    }
                }
            }
        }
        self.postings.take_rebuilt(rebuilt);
    }

    #[must_use]
    pub fn primary(&self) -> &StyleNodeFacts {
        assert!(
            !self.has_pending_input(),
            "cannot evaluate facts while pending input is uncommitted"
        );
        &self.rows
    }

    /// The committed arrangement while a staged transaction is being planned.
    #[must_use]
    pub fn committed(&self) -> &StyleNodeFacts {
        &self.rows
    }

    #[must_use]
    pub fn has_pending_input(&self) -> bool {
        !self.pending_rows.is_empty()
    }

    #[must_use]
    pub fn parts_of(&self, node: StyleNodeID) -> &[StyleAtomID] {
        if let Some(row) = self.pending_rows.get(&node) {
            return &row.parts;
        }
        self.rows.row_of(node).map_or(&[], |row| self.rows.parts_of(row))
    }

    #[must_use]
    pub fn custom_states_of(&self, node: StyleNodeID) -> &[StyleAtomID] {
        if let Some(row) = self.pending_rows.get(&node) {
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

    fn snapshot_row(&self, node: StyleNodeID) -> PendingFactRow {
        let Some(row) = self.rows.row_of(node) else {
            return PendingFactRow::default();
        };
        PendingFactRow {
            tag: self.rows.tag_of(row),
            folded_tag: self.rows.folded_tag_of(row),
            id: self.rows.id_of(row),
            language: self.rows.language_of(row),
            namespace: self.rows.namespace_of(row),
            part_exposure: self.rows.part_exposure_of(row),
            directionality: self.rows.directionality_of(row),
            heading_level: self.rows.heading_level_of(row),
            has_text_content: self.rows.has_text_content_of(row),
            states: self.rows.states_of(row),
            custom_states: self.rows.custom_states_of(row).to_vec(),
            parts: self.rows.parts_of(row).to_vec(),
            classes: self.rows.classes_of(row).to_vec(),
            attributes: self
                .rows
                .attributes_of(row)
                .iter()
                .map(|attribute| (attribute.name, attribute.value))
                .collect(),
        }
    }

    fn pending_row_mut(&mut self, node: StyleNodeID) -> &mut PendingFactRow {
        self.memory_dirty = true;
        if !self.pending_rows.contains_key(&node) {
            let row = self.snapshot_row(node);
            self.pending_rows.insert(node, row);
        }
        self.pending_rows.get_mut(&node).unwrap()
    }

    /// Give a node a row of its own without putting a feature in it.
    ///
    /// A shadow root is a node of the tree and a bearer of no features at all: it is featureless by
    /// definition, and nothing publishes anything about it. It still has to be materializable, or a
    /// selector that reads it - a relative anchor bound at the root, the leftmost step of a chain
    /// that lands there - asks for a row that never arrives and the whole match gives up.
    pub fn ensure_row(&mut self, node: StyleNodeID) {
        if self.rows.row_of(node).is_none() && !self.pending_rows.contains_key(&node) {
            self.memory_dirty = true;
            self.pending_rows.insert(node, PendingFactRow::default());
        }
    }

    pub fn set_tag(&mut self, node: StyleNodeID, tag: StyleAtomID, memory: &mut MemoryController) {
        let facts = self.pending_row_mut(node);
        let previous = std::mem::replace(&mut facts.tag, tag);
        if previous != tag {
            if !previous.is_none() {
                self.postings
                    .remove(PostingKey::Selector(SelectorPostingKey::Tag(previous)), node);
            }
            if !tag.is_none() {
                self.postings
                    .insert(PostingKey::Selector(SelectorPostingKey::Tag(tag)), node, memory);
            }
        }
    }

    /// Record the folded form of the element's name as a second name it is posted under. A folding
    /// equal to the name itself carries no information and is dropped, so the posting is never
    /// inserted twice for one element.
    pub fn set_folded_tag(&mut self, node: StyleNodeID, folded: StyleAtomID, memory: &mut MemoryController) {
        let facts = self.pending_row_mut(node);
        let folded = if folded == facts.tag { StyleAtomID::NONE } else { folded };
        let previous = std::mem::replace(&mut facts.folded_tag, folded);
        if previous != folded {
            if !previous.is_none() {
                self.postings
                    .remove(PostingKey::Selector(SelectorPostingKey::Tag(previous)), node);
            }
            if !folded.is_none() {
                self.postings
                    .insert(PostingKey::Selector(SelectorPostingKey::Tag(folded)), node, memory);
            }
        }
    }

    pub fn set_id(&mut self, node: StyleNodeID, id: StyleAtomID, memory: &mut MemoryController) {
        let facts = self.pending_row_mut(node);
        let previous = std::mem::replace(&mut facts.id, id);
        if previous != id {
            if !previous.is_none() {
                self.postings
                    .remove(PostingKey::Selector(SelectorPostingKey::Id(previous)), node);
            }
            if !id.is_none() {
                self.postings
                    .insert(PostingKey::Selector(SelectorPostingKey::Id(id)), node, memory);
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
                    attribute.name == name
                        || attribute.local == name
                        || attribute.folded_name == name
                        || attribute.folded_local == name
                })
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
        if let Some(row) = self.pending_rows.get(&node) {
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
        self.pending_row_mut(node).part_exposure = exposure;
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
        if let Some(row) = self.pending_rows.get(&node) {
            return row.directionality;
        }
        self.rows
            .row_of(node)
            .map_or(StyleAtomID::NONE, |row| self.rows.directionality_of(row))
    }

    pub fn set_has_text_content(&mut self, node: StyleNodeID, has_text_content: bool) {
        self.pending_row_mut(node).has_text_content = has_text_content;
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
        self.pending_row_mut(node).heading_level = level;
    }

    #[must_use]
    pub fn heading_level_of(&self, node: StyleNodeID) -> u8 {
        self.rows.row_of(node).map_or(0, |row| self.rows.heading_level_of(row))
    }

    /// Record what one language atom spells, so `:lang()` can compare ranges against it.
    pub fn set_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        if language.is_none() || self.language_texts.contains_key(&language) {
            return;
        }
        self.memory_dirty = true;
        self.language_texts.insert(language, text.to_vec());
    }

    pub fn set_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        self.pending_row_mut(node).language = language;
    }

    /// An element's namespace is fixed when it is created, so this is published once, on arrival,
    /// and is never an input that moves.
    pub fn set_namespace(&mut self, node: StyleNodeID, namespace: StyleAtomID) {
        self.pending_row_mut(node).namespace = namespace;
    }

    pub fn set_directionality(
        &mut self,
        node: StyleNodeID,
        directionality: StyleAtomID,
        memory: &mut MemoryController,
    ) {
        let facts = self.pending_row_mut(node);
        let previous = std::mem::replace(&mut facts.directionality, directionality);
        if previous != directionality {
            if !previous.is_none() {
                self.postings
                    .remove(PostingKey::Selector(SelectorPostingKey::Directionality(previous)), node);
            }
            if !directionality.is_none() {
                self.postings.insert(
                    PostingKey::Selector(SelectorPostingKey::Directionality(directionality)),
                    node,
                    memory,
                );
            }
        }
    }

    pub fn set_class(&mut self, node: StyleNodeID, class: StyleAtomID, present: bool, memory: &mut MemoryController) {
        let facts = self.pending_row_mut(node);
        match (present, facts.classes.binary_search(&class)) {
            (true, Err(index)) => {
                facts.classes.insert(index, class);
                self.postings
                    .insert(PostingKey::Selector(SelectorPostingKey::Class(class)), node, memory);
            }
            (false, Ok(index)) => {
                facts.classes.remove(index);
                self.postings
                    .remove(PostingKey::Selector(SelectorPostingKey::Class(class)), node);
            }
            _ => {}
        }
    }

    /// Replace the set of animation names an element references.
    ///
    /// This arrives from computed style rather than from the DOM, so it is published as a set rather
    /// than one name at a time: an element's `animation-name` is recomputed whole.
    pub fn set_animation_names(&mut self, node: StyleNodeID, names: &[StyleAtomID], memory: &mut MemoryController) {
        let mut sorted: Vec<StyleAtomID> = names.to_vec();
        sorted.sort_unstable_by_key(|name| name.0);
        sorted.dedup();
        let previous = self
            .metadata_of(node)
            .map_or_else(Vec::new, |metadata| metadata.animation_names.clone());
        if previous == sorted {
            return;
        }
        for &name in &previous {
            if !sorted.contains(&name) {
                self.postings
                    .remove(PostingKey::Dependency(DependencyPostingKey::AnimationName(name)), node);
            }
        }
        for name in &sorted {
            if !previous.contains(name) {
                self.postings.insert(
                    PostingKey::Dependency(DependencyPostingKey::AnimationName(*name)),
                    node,
                    memory,
                );
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
            true => self.postings.insert(
                PostingKey::Dependency(DependencyPostingKey::AnyCustomFunction),
                node,
                memory,
            ),
            false => {
                self.postings
                    .remove(PostingKey::Dependency(DependencyPostingKey::AnyCustomFunction), node);
                true
            }
        };
    }

    /// Whether this element used custom properties whose names could not all be enumerated.
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
            true => self.postings.insert(
                PostingKey::Dependency(DependencyPostingKey::AnyCustomProperty),
                node,
                memory,
            ),
            false => {
                self.postings
                    .remove(PostingKey::Dependency(DependencyPostingKey::AnyCustomProperty), node);
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
        self.metadata_mut(node).custom_property_set = set;
        if previous != 0 {
            self.postings.remove(
                PostingKey::Dependency(DependencyPostingKey::CustomPropertySet(previous)),
                node,
            );
        }
        if set != 0 {
            self.postings.insert(
                PostingKey::Dependency(DependencyPostingKey::CustomPropertySet(set)),
                node,
                memory,
            );
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
            .find(hash, |_identity, candidate| candidate == names)
        {
            return candidate.0;
        }
        let id = self.custom_property_name_set_vacancies.pop().unwrap_or_else(|| {
            u32::try_from(self.custom_property_name_sets.len() + 1).expect("custom property name set space exhausted")
        });
        self.custom_property_name_sets
            .insert(hash, CustomPropertyNameSetID(id), names.to_vec());
        for name in names {
            self.custom_property_set_ids_by_name.entry(*name).or_default().push(id);
        }
        id
    }

    /// The elements whose own cascade declares `name`, which is every element in any set holding it.
    pub fn custom_property_candidates(&self, name: StyleAtomID) -> Result<Vec<StyleNodeID>, PostingKey> {
        let Some(sets) = self.custom_property_set_ids_by_name.get(&name) else {
            return Ok(Vec::new());
        };
        let mut nodes = Vec::new();
        for &set in sets {
            match self
                .postings
                .lookup(PostingKey::Dependency(DependencyPostingKey::CustomPropertySet(set)))
            {
                Lookup::Known(posting) => nodes.extend(posting.candidates()),
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
        let keys = self.attribute_name_keys(name);
        let facts = self.pending_row_mut(node);
        let found = facts.attributes.binary_search_by_key(&name, |entry| entry.0);
        match (present, found) {
            (true, Ok(index)) => facts.attributes[index].1 = value,
            (true, Err(index)) => {
                facts.attributes.insert(index, (name, value));
                for key in keys {
                    self.postings.insert(
                        PostingKey::Selector(SelectorPostingKey::AttributeName(key)),
                        node,
                        memory,
                    );
                }
            }
            (false, Ok(index)) => {
                facts.attributes.remove(index);
                // A shared name stays true of the element while another of its attributes still
                // answers to it, so only the names nothing implies any more are dropped.
                for key in keys {
                    if key == name || !self.node_answers_to_attribute_name(node, key) {
                        self.postings
                            .remove(PostingKey::Selector(SelectorPostingKey::AttributeName(key)), node);
                    }
                }
            }
            (false, Err(_)) => {}
        }
    }

    /// Whether any attribute the node still carries is indexed under `key`.
    #[must_use]
    fn node_answers_to_attribute_name(&self, node: StyleNodeID, key: StyleAtomID) -> bool {
        self.pending_rows.get(&node).is_some_and(|facts| {
            facts
                .attributes
                .iter()
                .any(|entry| self.attribute_name_keys(entry.0).contains(&key))
        })
    }

    /// Record which longhand properties one of an element's own declarations covers.
    pub fn set_element_declared_properties(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declared: Vec<DeclaredProperty>,
        declarations_are_complete: bool,
    ) {
        self.memory_dirty = true;
        self.element_declared_properties
            .set(node, kind, declared, declarations_are_complete);
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
        self.attribute_name_locals.insert(name, forms);
    }

    /// The other names an attribute name answers to, all `NONE` if the name has not been published.
    #[must_use]
    pub fn attribute_name_forms(&self, name: StyleAtomID) -> AttributeNameForms {
        self.attribute_name_locals.get(&name).copied().unwrap_or_default()
    }

    /// Every atom an attribute of this name is indexed under, without repeats.
    #[must_use]
    pub fn attribute_name_keys(&self, name: StyleAtomID) -> Vec<StyleAtomID> {
        let forms = self.attribute_name_forms(name);
        let mut keys = vec![name];
        for other in [forms.local, forms.folded_name, forms.folded_local] {
            if !other.is_none() && !keys.contains(&other) {
                keys.push(other);
            }
        }
        keys
    }

    pub fn set_attribute_value_text(&mut self, value: StyleAtomID, text: &[u16]) {
        if value.is_none() || self.attribute_value_texts.contains_key(&value) {
            return;
        }
        self.memory_dirty = true;
        self.attribute_value_texts.insert(value, text.to_vec());
    }

    #[must_use]
    pub fn has_attribute_value_text(&self, value: StyleAtomID) -> bool {
        self.attribute_value_texts.contains_key(&value)
    }

    pub fn set_state(&mut self, node: StyleNodeID, fact: StateFact, value: bool) {
        let facts = self.pending_row_mut(node);
        if value {
            facts.states.insert(fact);
        } else {
            facts.states.remove(fact);
        }
    }

    pub fn set_parts(&mut self, node: StyleNodeID, parts: &[StyleAtomID]) {
        self.pending_row_mut(node).parts = parts.to_vec();
    }

    pub fn set_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID]) {
        self.pending_row_mut(node).custom_states = states.to_vec();
    }

    pub fn forget(&mut self, node: StyleNodeID) {
        self.memory_dirty = true;
        self.element_declared_properties.remove(node);
        self.pending_rows.remove(&node);
        let Some(row) = self.rows.row_of(node) else {
            return;
        };
        let row_bytes = self.rows.logical_bytes_of_row(row);
        let facts = self.snapshot_row(node);
        self.rows.forget_row(node);
        self.primary_live_bytes = self
            .primary_live_bytes
            .checked_sub(row_bytes)
            .expect("primary live fact byte count underflow");
        self.primary_stale_bytes = self
            .primary_stale_bytes
            .checked_add(row_bytes)
            .expect("primary fact byte count overflow");
        if !facts.tag.is_none() {
            self.postings
                .remove(PostingKey::Selector(SelectorPostingKey::Tag(facts.tag)), node);
        }
        if !facts.folded_tag.is_none() {
            self.postings
                .remove(PostingKey::Selector(SelectorPostingKey::Tag(facts.folded_tag)), node);
        }
        if !facts.id.is_none() {
            self.postings
                .remove(PostingKey::Selector(SelectorPostingKey::Id(facts.id)), node);
        }
        if !facts.directionality.is_none() {
            self.postings.remove(
                PostingKey::Selector(SelectorPostingKey::Directionality(facts.directionality)),
                node,
            );
        }
        for class in facts.classes {
            self.postings
                .remove(PostingKey::Selector(SelectorPostingKey::Class(class)), node);
        }
        for (name, _) in facts.attributes {
            for key in self.attribute_name_keys(name) {
                self.postings
                    .remove(PostingKey::Selector(SelectorPostingKey::AttributeName(key)), node);
            }
        }
        if let Some(metadata) = node
            .element_index()
            .and_then(|index| self.metadata.get_mut(index as usize))
            .and_then(Option::take)
        {
            for name in metadata.animation_names {
                self.postings
                    .remove(PostingKey::Dependency(DependencyPostingKey::AnimationName(name)), node);
            }
            if metadata.custom_property_set != 0 {
                self.postings.remove(
                    PostingKey::Dependency(DependencyPostingKey::CustomPropertySet(metadata.custom_property_set)),
                    node,
                );
            }
            if metadata.uses_unnamed_custom_properties {
                self.postings
                    .remove(PostingKey::Dependency(DependencyPostingKey::AnyCustomProperty), node);
            }
            if metadata.uses_custom_functions {
                self.postings
                    .remove(PostingKey::Dependency(DependencyPostingKey::AnyCustomFunction), node);
            }
        }
    }

    pub fn sweep_auxiliary_catalogs(&mut self) {
        self.memory_dirty = true;
        let mut live_languages = HashMap::default();
        let mut live_attribute_values = HashMap::default();
        let mut live_attribute_names = HashMap::default();
        let mut live_custom_property_sets = vec![false; self.custom_property_name_sets.len() + 1];
        for node in self.rows.live_nodes() {
            let row = self.rows.row_of(node).expect("a live node must have a fact row");
            let language = self.rows.language_of(row);
            if !language.is_none() {
                live_languages.insert(language, ());
            }
            for attribute in self.rows.attributes_of(row) {
                live_attribute_names.insert(attribute.name, ());
                if !attribute.value.is_none() {
                    live_attribute_values.insert(attribute.value, ());
                }
            }
        }
        for row in self
            .pending_rows
            .values()
            .chain(self.selector_query_before_rows.values())
        {
            if !row.language.is_none() {
                live_languages.insert(row.language, ());
            }
            for &(name, value) in &row.attributes {
                live_attribute_names.insert(name, ());
                if !value.is_none() {
                    live_attribute_values.insert(value, ());
                }
            }
        }
        for metadata in self.metadata.iter().flatten() {
            if metadata.custom_property_set != 0 {
                live_custom_property_sets[metadata.custom_property_set as usize] = true;
            }
        }
        self.language_texts
            .retain(|language, _| live_languages.contains_key(language));
        self.attribute_value_texts
            .retain(|value, _| live_attribute_values.contains_key(value));
        self.attribute_name_locals
            .retain(|name, _| live_attribute_names.contains_key(name));

        self.custom_property_set_ids_by_name = HashMap::default();
        self.custom_property_name_set_vacancies.clear();
        for (id, &is_live) in live_custom_property_sets.iter().enumerate().skip(1) {
            if !is_live {
                let identity = CustomPropertyNameSetID(id as u32);
                if !self.custom_property_name_sets[identity].is_empty() {
                    let hash = super::intern_table::content_hash(&self.custom_property_name_sets[identity]);
                    self.custom_property_name_sets.remove_identity(hash, identity);
                    self.custom_property_name_sets[identity] = Vec::new();
                }
                self.custom_property_name_set_vacancies.push(id as u32);
                continue;
            }
            let names = &self.custom_property_name_sets[CustomPropertyNameSetID(id as u32)];
            for name in names {
                self.custom_property_set_ids_by_name
                    .entry(*name)
                    .or_default()
                    .push(id as u32);
            }
        }
    }

    #[must_use]
    #[cfg(test)]
    pub fn covers(&self, node: StyleNodeID) -> bool {
        self.rows.row_of(node).is_some() || self.pending_rows.contains_key(&node)
    }

    /// Pack the facts of a bounded set of style nodes into one batch.
    ///
    /// A node the store has never heard a fact for is skipped rather than given defaults, so the
    /// evaluator reports a miss and the caller widens instead of matching against a fiction.
    ///
    /// Every column an operator can read is filled, not only the ones a compound tests: a row whose
    /// parameters were left at their defaults answers `:dir()` and `:state()` as though the element
    /// held neither, which is a wrong answer rather than a missing one.
    pub fn materialize(&self, nodes: impl Iterator<Item = StyleNodeID>, batch: &mut StyleNodeFacts) {
        batch.clear();
        for node in nodes {
            self.materialize_row(node, batch);
        }
    }

    /// Append the rows of `nodes` that the batch does not hold yet, keeping existing rows.
    ///
    /// One shared batch can serve a whole pass this way: consecutive asks overwhelmingly share
    /// their ancestor chains, and each row is packed at most once per pass instead of once per
    /// ask.
    pub fn materialize_missing(&self, nodes: impl Iterator<Item = StyleNodeID>, batch: &mut StyleNodeFacts) {
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
        let pending_payloads = self
            .pending_rows
            .values()
            .map(PendingFactRow::capacity_bytes)
            .sum::<u64>();
        let selector_query_before_payloads = self
            .selector_query_before_rows
            .values()
            .map(PendingFactRow::capacity_bytes)
            .sum::<u64>();
        let metadata_payloads = self
            .metadata
            .iter()
            .flatten()
            .map(ElementFactMetadata::capacity_bytes)
            .sum::<u64>();
        let custom_property_name_payloads = self
            .custom_property_name_sets
            .iter()
            .map(|names| names.capacity() * size_of::<StyleAtomID>())
            .sum::<usize>();
        let custom_property_name_index_payloads = self
            .custom_property_set_ids_by_name
            .values()
            .map(|ids| ids.capacity() * size_of::<u32>())
            .sum::<usize>();
        let language_payloads = self
            .language_texts
            .values()
            .map(|text| text.capacity() * size_of::<u16>())
            .sum::<usize>();
        let attribute_value_payloads = self
            .attribute_value_texts
            .values()
            .map(|text| text.capacity() * size_of::<u16>())
            .sum::<usize>();

        capacity_bytes! {
            shallow [
                self.pending_rows,
                self.selector_query_before_rows,
                self.custom_property_name_sets,
                self.custom_property_name_set_vacancies,
                self.custom_property_set_ids_by_name,
                self.language_texts,
                self.attribute_value_texts,
                self.attribute_name_locals,
            ];
            cached [];
            nested [
                pending_payloads,
                selector_query_before_payloads,
                metadata_payloads,
                custom_property_name_payloads,
                custom_property_name_index_payloads,
                language_payloads,
                attribute_value_payloads,
            ];
            skip [];
        }
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
                self.primary_stale_bytes,
                self.postings,
                self.memory,
                self.memory_dirty,
                self.pending_rows,
                self.selector_query_before_rows,
                self.custom_property_name_sets,
                self.custom_property_name_set_vacancies,
                self.custom_property_set_ids_by_name,
                self.language_texts,
                self.attribute_value_texts,
                self.attribute_name_locals,
            ];
        }
    }

    /// Commit every pending fact-row edit to the authoritative arrangement.
    pub fn commit_pending(&mut self, memory: &mut MemoryController) {
        if !self.memory_dirty {
            self.rebuild_missing_postings(memory);
            return;
        }
        let pending_rows = std::mem::take(&mut self.pending_rows);
        for (node, facts) in pending_rows {
            let replaced_bytes = self
                .rows
                .row_of(node)
                .map_or(0, |row| self.rows.logical_bytes_of_row(row));
            append_fact_row(
                node,
                &facts,
                &self.attribute_value_texts,
                &self.attribute_name_locals,
                &self.language_texts,
                &mut self.rows,
            );
            let row = self.rows.row_of(node).unwrap();
            let replacement_bytes = self.rows.logical_bytes_of_row(row);
            self.primary_live_bytes = self
                .primary_live_bytes
                .checked_sub(replaced_bytes)
                .and_then(|bytes| bytes.checked_add(replacement_bytes))
                .expect("primary live fact byte count overflow");
            self.primary_stale_bytes = self
                .primary_stale_bytes
                .checked_add(replaced_bytes)
                .expect("primary fact byte count overflow");
        }
        if self.primary_stale_bytes > self.primary_live_bytes {
            let live: Vec<(StyleNodeID, PendingFactRow)> = self
                .rows
                .live_nodes()
                .map(|node| (node, self.snapshot_row(node)))
                .collect();
            self.rows = StyleNodeFacts::new();
            for (node, facts) in live {
                append_fact_row(
                    node,
                    &facts,
                    &self.attribute_value_texts,
                    &self.attribute_name_locals,
                    &self.language_texts,
                    &mut self.rows,
                );
            }
            self.primary_stale_bytes = 0;
            debug_assert_eq!(
                self.primary_live_bytes,
                self.rows
                    .live_nodes()
                    .map(|node| self.rows.logical_bytes_of_row(self.rows.row_of(node).unwrap()))
                    .sum::<u64>()
            );
        }
        let current = self.capacity_bytes();
        self.memory.resize_required_to(memory, current);
        self.memory_dirty = false;
        self.rebuild_missing_postings(memory);
    }

    pub fn prepare_selector_query(&mut self, memory: &mut MemoryController) {
        let mut pending_nodes = Vec::with_capacity(self.pending_rows.len());
        for &node in self.pending_rows.keys() {
            pending_nodes.push(node);
        }
        for node in pending_nodes {
            if !self.selector_query_before_rows.contains_key(&node) {
                let before = self.snapshot_row(node);
                self.selector_query_before_rows.insert(node, before);
            }
        }
        self.commit_pending(memory);
    }

    /// Put the saved before rows back in the resident arrangement and stage the query-visible rows as the after side.
    pub fn restore_prepared_selector_query_input(&mut self, memory: &mut MemoryController) {
        if self.selector_query_before_rows.is_empty() {
            return;
        }

        let mut after_rows = std::mem::take(&mut self.pending_rows);
        for &node in self.selector_query_before_rows.keys() {
            after_rows.entry(node).or_insert_with(|| self.snapshot_row(node));
        }

        self.pending_rows = std::mem::take(&mut self.selector_query_before_rows);
        self.memory_dirty = true;
        self.commit_pending(memory);
        self.pending_rows = after_rows;
        self.memory_dirty = !self.pending_rows.is_empty();
        let current = self.capacity_bytes();
        self.memory.resize_required_to(memory, current);
    }

    /// Snapshot the committed rows which pending local facts will replace at the barrier.
    #[must_use]
    pub fn before_pending_facts(&self) -> StyleNodeFacts {
        let mut nodes = Vec::with_capacity(self.pending_rows.len());
        for &node in self.pending_rows.keys() {
            nodes.push(node);
        }
        nodes.sort_unstable();
        let mut before = StyleNodeFacts::new();
        for node in nodes {
            let facts = self.snapshot_row(node);
            append_fact_row(
                node,
                &facts,
                &self.attribute_value_texts,
                &self.attribute_name_locals,
                &self.language_texts,
                &mut before,
            );
        }
        before
    }

    /// Pack the final rows of the pending transaction without committing them.
    #[must_use]
    pub fn pending_facts(&self) -> StyleNodeFacts {
        let mut nodes = Vec::with_capacity(self.pending_rows.len());
        for &node in self.pending_rows.keys() {
            nodes.push(node);
        }
        nodes.sort_unstable();
        let mut after = StyleNodeFacts::new();
        for node in nodes {
            append_fact_row(
                node,
                &self.pending_rows[&node],
                &self.attribute_value_texts,
                &self.attribute_name_locals,
                &self.language_texts,
                &mut after,
            );
        }
        after
    }
}

fn append_fact_row(
    node: StyleNodeID,
    facts: &PendingFactRow,
    attribute_value_texts: &HashMap<StyleAtomID, Vec<u16>>,
    attribute_name_locals: &HashMap<StyleAtomID, AttributeNameForms>,
    language_texts: &HashMap<StyleAtomID, Vec<u16>>,
    batch: &mut StyleNodeFacts,
) {
    let attributes: Vec<AttributeFact> = facts
        .attributes
        .iter()
        .map(|&(name, value)| {
            let text = attribute_value_texts.get(&value).map_or(&[][..], Vec::as_slice);
            let (text_offset, text_length) = batch.push_text(text);
            let forms = attribute_name_locals.get(&name).copied().unwrap_or_default();
            AttributeFact {
                name,
                local: forms.local,
                folded_name: if forms.folded_name.is_none() {
                    name
                } else {
                    forms.folded_name
                },
                folded_local: if forms.folded_local.is_none() {
                    forms.local
                } else {
                    forms.folded_local
                },
                value,
                text_offset,
                text_length,
            }
        })
        .collect();
    batch.push_row(node, facts.tag, facts.id, facts.states, &facts.classes, &attributes);
    batch.set_row_folded_tag(facts.folded_tag);
    batch.set_row_namespace(facts.namespace);
    batch.set_row_part_exposure(facts.part_exposure);
    batch.set_row_language_tag(language_texts.get(&facts.language).map_or(&[], Vec::as_slice));
    batch.set_row_has_text_content(facts.has_text_content);
    let row = u32::try_from(batch.row_count() - 1).expect("fact batch row space exhausted");
    batch.set_row_parameters(
        row,
        facts.directionality,
        facts.language,
        facts.heading_level,
        &facts.custom_states,
        &facts.parts,
    );
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
        let key = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));

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
        let key = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));
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
    fn live_postings_receive_compact_transaction_identities() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let first = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));
        let second = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(2)));
        let third = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(3)));
        let node = StyleNodeID::element(1);

        postings.insert(first, node, &mut memory);
        postings.insert(second, node, &mut memory);
        postings.ensure_dense_ids();
        postings.remove(first, node);
        postings.insert(third, node, &mut memory);
        postings.ensure_dense_ids();
        let mut ids = [
            known_posting(&postings, second).id().index(),
            known_posting(&postings, third).id().index(),
        ];
        ids.sort_unstable();

        assert_eq!(ids, [0, 1]);
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
        facts.commit_pending(&mut memory);

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
        facts.commit_pending(&mut memory);
        facts.set_directionality(node, StyleAtomID(4), &mut memory);

        assert_eq!(facts.directionality_of(node), StyleAtomID(4));
    }

    #[test]
    fn forgetting_an_element_removes_every_attribute_name_alias_posting() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);
        let name = StyleAtomID(10);
        let forms = AttributeNameForms {
            local: StyleAtomID(11),
            folded_name: StyleAtomID(12),
            folded_local: StyleAtomID(13),
        };
        facts.note_attribute_name_forms(name, forms);
        facts.set_attribute(node, name, StyleAtomID(20), true, &mut memory);
        facts.commit_pending(&mut memory);

        for key in [name, forms.local, forms.folded_name, forms.folded_local] {
            assert!(
                known_posting(
                    &facts.postings,
                    PostingKey::Selector(SelectorPostingKey::AttributeName(key))
                )
                .contains(node)
            );
        }

        facts.forget(node);

        for key in [name, forms.local, forms.folded_name, forms.folded_local] {
            assert!(matches!(
                facts
                    .postings
                    .lookup(PostingKey::Selector(SelectorPostingKey::AttributeName(key))),
                Lookup::KnownAbsent
            ));
        }
    }

    #[test]
    fn resident_fact_rows_follow_dense_element_identity_slots() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let first = StyleNodeID::element(3);
        let later = StyleNodeID::element(64);

        facts.set_tag(first, StyleAtomID(10), &mut memory);
        facts.set_tag(later, StyleAtomID(20), &mut memory);
        facts.commit_pending(&mut memory);
        assert_eq!(facts.len(), 2);
        assert_eq!(facts.rows.row_by_element_index.len(), 65);
        assert_eq!(facts.tag_of_node(first), StyleAtomID(10));
        assert_eq!(facts.tag_of_node(later), StyleAtomID(20));

        facts.forget(first);
        assert_eq!(facts.len(), 1);
        assert!(!facts.covers(first));
        facts.ensure_row(first);
        facts.commit_pending(&mut memory);
        assert_eq!(facts.len(), 2);
        assert_eq!(facts.tag_of_node(first), StyleAtomID::NONE);
    }

    #[test]
    fn primary_fact_rows_append_repoint_and_compact() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(3);
        let first_class = StyleAtomID(20);
        let second_class = StyleAtomID(21);
        let part = StyleAtomID(30);
        let custom_state = StyleAtomID(31);

        facts.set_tag(node, StyleAtomID(10), &mut memory);
        facts.set_class(node, first_class, true, &mut memory);
        assert!(facts.has_pending_input());
        facts.commit_pending(&mut memory);
        assert!(!facts.has_pending_input());
        let initial_generation = facts.primary().generation();
        assert_eq!(facts.primary().row_count(), 1);
        assert_eq!(facts.primary().stale_rows(), 0);
        assert_eq!(facts.primary_stale_bytes, 0);

        facts.set_class(node, second_class, true, &mut memory);
        facts.set_state(node, StateFact::Hover, true);
        facts.set_parts(node, &[part]);
        facts.set_custom_states(node, &[custom_state]);
        facts.commit_pending(&mut memory);
        assert_eq!(facts.primary().generation(), initial_generation);
        assert_eq!(facts.primary().row_count(), 2);
        assert_eq!(facts.primary().stale_rows(), 1);
        assert!(facts.primary_stale_bytes > 0);
        assert!(facts.primary_stale_bytes <= facts.primary_live_bytes);
        assert_eq!(facts.classes_of_node(node), &[first_class, second_class]);
        let row = facts.primary().row_of(node).unwrap();
        assert_eq!(facts.primary().parts_of(row), &[part]);
        assert_eq!(facts.primary().custom_states_of(row), &[custom_state]);

        facts.set_class(node, first_class, false, &mut memory);
        facts.commit_pending(&mut memory);
        assert_ne!(facts.primary().generation(), initial_generation);
        assert_eq!(facts.primary().row_count(), 1);
        assert_eq!(facts.primary().stale_rows(), 0);
        assert_eq!(facts.primary_stale_bytes, 0);
        assert_eq!(facts.classes_of_node(node), &[second_class]);
        assert!(facts.states_of_node(node).contains(StateFact::Hover));
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
    fn element_fact_capacity_includes_auxiliary_catalogs_and_pending_rows() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(64);
        let initial = facts.capacity_bytes();

        facts.set_class(node, StyleAtomID(1), true, &mut memory);
        facts.set_parts(node, &[StyleAtomID(2), StyleAtomID(3)]);
        facts.set_custom_states(node, &[StyleAtomID(4), StyleAtomID(5)]);
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
        facts.commit_pending(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
            facts.capacity_bytes()
        );
    }

    #[test]
    fn detached_element_churn_reuses_auxiliary_catalog_storage() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut facts = ElementFactStore::new();
        let node = StyleNodeID::element(1);

        for index in 1..128_u32 {
            let language = StyleAtomID(index * 4);
            let attribute_name = StyleAtomID(index * 4 + 1);
            let attribute_value = StyleAtomID(index * 4 + 2);
            let custom_property = StyleAtomID(index * 4 + 3);
            facts.set_language_text(language, &[index as u16]);
            facts.set_language(node, language);
            facts.note_attribute_name_forms(attribute_name, AttributeNameForms::default());
            facts.set_attribute_value_text(attribute_value, &[index as u16]);
            facts.set_attribute(node, attribute_name, attribute_value, true, &mut memory);
            facts.set_custom_property_names(node, &[custom_property], &mut memory);
            facts.commit_pending(&mut memory);

            facts.forget(node);
            facts.sweep_auxiliary_catalogs();
            assert!(facts.language_texts.is_empty());
            assert!(facts.attribute_name_locals.is_empty());
            assert!(facts.attribute_value_texts.is_empty());
            assert!(facts.custom_property_name_sets.index_is_empty());
            assert!(facts.custom_property_set_ids_by_name.is_empty());
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

        facts.set_element_declared_properties(first, inline, vec![declared(1, false, 10), declared(2, true, 20)], true);
        facts.set_element_declared_properties(first, hints, vec![declared(3, false, 30)], false);
        facts.set_element_declared_properties(later, svg, vec![declared(4, false, 40)], true);
        facts.set_element_declared_properties(later, inline, Vec::new(), false);
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
        facts.set_element_declared_properties(first, inline, Vec::new(), true);
        assert!(facts.element_declared_properties.get(first, inline).0.is_empty());
        assert_eq!(
            facts.element_declared_properties.get(first, hints),
            (&[declared(3, false, 30)][..], false)
        );
        facts.set_element_declared_properties(later, inline, Vec::new(), true);
        assert_eq!(facts.element_declared_properties.get(later, inline), (&[][..], true));

        // Declaration rows can exist without a resident selector-fact row. Retirement still has to
        // clear them before the dense identity is reused.
        facts.forget(first);
        assert!(facts.element_declared_properties.get(first, hints).0.is_empty());
        assert_eq!(
            facts.element_declared_properties.get(later, svg),
            (&[declared(4, false, 40)][..], true)
        );
        facts.commit_pending(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
            facts.capacity_bytes()
        );
    }

    #[test]
    fn a_refused_posting_is_dropped_rather_than_left_partial() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        memory.set_tier3_limit_for_test(0);
        let mut postings = FeaturePostings::new();
        let key = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));

        // A backgrounded document has no Tier-3 budget at all.
        assert!(!postings.insert(key, StyleNodeID::element(1), &mut memory));
        assert!(matches!(postings.lookup(key), Lookup::Missing(gap) if gap == key));
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::FeaturePosting),
            postings.missing_capacity_bytes()
        );
    }

    #[test]
    fn a_refused_posting_growth_releases_its_previous_charge() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        let key = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));

        assert!(postings.insert(key, StyleNodeID::element(1), &mut memory));
        let admitted_bytes = memory.bytes_in_category(MemoryCategory::FeaturePosting);
        memory.set_tier3_limit_for_test(admitted_bytes);
        assert!((2..512).any(|index| !postings.insert(key, StyleNodeID::element(index), &mut memory)));

        assert!(matches!(postings.lookup(key), Lookup::Missing(gap) if gap == key));
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::FeaturePosting),
            postings.missing_capacity_bytes()
        );
    }

    #[test]
    fn evicting_every_posting_retains_only_the_missing_key_charge() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut postings = FeaturePostings::new();
        for feature in 1..20_u32 {
            for index in 1..50_u32 {
                postings.insert(
                    PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(feature))),
                    StyleNodeID::element(index),
                    &mut memory,
                );
            }
        }
        assert!(memory.bytes_in_category(MemoryCategory::FeaturePosting) > 0);
        postings.evict_all();
        assert_eq!(postings.feature_count(), 0);
        let key = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));
        assert!(matches!(postings.lookup(key), Lookup::Missing(gap) if gap == key));
        assert!(matches!(
            postings.lookup(PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(100)))),
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
        facts.commit_pending(&mut memory);
        facts.postings_mut().evict_all();

        memory.set_tier3_limit_for_test(0);
        facts.set_class(node, old_class, false, &mut memory);
        facts.set_class(node, new_class, true, &mut memory);
        facts.set_animation_names(node, &[new_animation], &mut memory);
        facts.commit_pending(&mut memory);
        let new_class_key = PostingKey::Selector(SelectorPostingKey::Class(new_class));
        assert!(matches!(facts.postings().lookup(new_class_key), Lookup::Missing(gap) if gap == new_class_key));

        memory.set_tier3_limit_for_test(u64::MAX);
        facts.commit_pending(&mut memory);
        let old_class_key = PostingKey::Selector(SelectorPostingKey::Class(old_class));
        let old_animation_key = PostingKey::Dependency(DependencyPostingKey::AnimationName(old_animation));
        let new_animation_key = PostingKey::Dependency(DependencyPostingKey::AnimationName(new_animation));
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
        let evicted = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(1)));
        let absent = PostingKey::Selector(SelectorPostingKey::Class(StyleAtomID(2)));
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
            store.commit_pending(&mut memory);
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
            store.commit_pending(&mut memory);
            assert_eq!(
                memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
                store.capacity_bytes()
            );
        }

        for index in 1..40_u32 {
            store.forget(StyleNodeID::element(index));
            store.commit_pending(&mut memory);
            assert_eq!(
                memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
                store.capacity_bytes()
            );
        }
        assert!(store.is_empty());

        // The retained column capacities remain charged after every live row is forgotten.
        store.commit_pending(&mut memory);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::StyleNodeMapping),
            store.capacity_bytes()
        );
    }

    #[test]
    fn cascade_order_projection_handles_duplicate_entries_and_sparse_rules() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32, selector_entry: u32, multi_key| DispatchEntry {
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
    fn a_candidate_probes_only_the_buckets_its_own_facts_name() {
        let mut dispatch = RuleDispatch::new();
        let entry = |rule: u32| DispatchEntry {
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
                folded_local: StyleAtomID::NONE,
                folded_name: StyleAtomID(30),
                local: StyleAtomID::NONE,
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
        facts.push_row(
            StyleNodeID::element(1),
            StyleAtomID(1),
            StyleAtomID::NONE,
            StateSet::default(),
            &[],
            &[
                AttributeFact {
                    folded_local: StyleAtomID::NONE,
                    folded_name: StyleAtomID::NONE,
                    local: shared_local_name,
                    name: StyleAtomID(31),
                    value: StyleAtomID(41),
                    text_offset: u32::MAX,
                    text_length: 0,
                },
                AttributeFact {
                    folded_local: StyleAtomID::NONE,
                    folded_name: StyleAtomID::NONE,
                    local: shared_local_name,
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
                    folded_local: StyleAtomID::NONE,
                    folded_name: StyleAtomID(40),
                    local: StyleAtomID::NONE,
                    name: StyleAtomID(40),
                    value: StyleAtomID::NONE,
                    text_offset: offset,
                    text_length: length,
                },
                AttributeFact {
                    folded_local: StyleAtomID::NONE,
                    folded_name: StyleAtomID(40),
                    local: StyleAtomID::NONE,
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
        let old_text: Vec<u16> = "old".encode_utf16().collect();
        let new_text: Vec<u16> = "new".encode_utf16().collect();
        let mut store = ElementFactStore::new();
        store.set_attribute_value_text(old, &old_text);
        store.set_attribute_value_text(new, &new_text);
        store.set_attribute(node, name, old, true, &mut memory);
        store.commit_pending(&mut memory);
        store.set_attribute(node, name, new, true, &mut memory);

        let before = store.before_pending_facts();
        let after = store.pending_facts();

        let old_attribute = before.attribute_of(before.row_of(node).unwrap(), name).unwrap();
        assert_eq!(old_attribute.value, old);
        assert_eq!(before.text_of(old_attribute), Some(old_text.as_slice()));
        let new_attribute = after.attribute_of(after.row_of(node).unwrap(), name).unwrap();
        assert_eq!(new_attribute.value, new);
        assert_eq!(after.text_of(new_attribute), Some(new_text.as_slice()));
    }

    #[test]
    fn states_pack_into_one_word() {
        let mut states = StateSet::default();
        states.insert(StateFact::Hover);
        states.insert(StateFact::Checked);
        assert!(states.contains(StateFact::Hover));
        assert!(states.contains(StateFact::Checked));
        assert!(!states.contains(StateFact::Focus));
        states.remove(StateFact::Hover);
        assert!(!states.contains(StateFact::Hover));
        assert_eq!(size_of::<StateSet>(), 8);
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
