/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Atom identities, local feature keys and values, and dispatch keys shared by every reader of
//! style node facts.

use super::input_types::StateFact;

define_id! {
    /// Document-local identity of an interned selector-mentioned feature. Obeys the same generation
    /// and non-reuse rules as [`super::tree::StyleNodeID`].
    #[repr(transparent)]
    default pub struct StyleAtomID(pub);
}

impl StyleAtomID {
    /// Reserved: no atom. Document-local atoms start at 1.
    pub const NONE: Self = Self(0);

    #[must_use]
    pub fn is_none(self) -> bool {
        self == Self::NONE
    }

    /// The atoms a list of raw atom words holds, read in place.
    #[must_use]
    pub fn slice_of_raw(raw: &[u32]) -> &[Self] {
        // SAFETY: `StyleAtomID` is a transparent `u32`, and every `u32` is a valid one.
        unsafe { ak::slice_from_raw_parts(raw.as_ptr().cast(), raw.len()) }
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
#[repr(C)]
pub struct AttributeFact {
    pub name: StyleAtomID,
    pub value: StyleAtomID,
}

const _: () = assert!(size_of::<AttributeFact>() == size_of::<[u32; 2]>());
const _: () = assert!(align_of::<AttributeFact>() == align_of::<[u32; 2]>());

impl AttributeFact {
    /// The attributes a list of raw (name, value) atom words holds, read in place.
    #[must_use]
    pub fn slice_of_raw(raw: &[[u32; 2]]) -> &[Self] {
        // SAFETY: `AttributeFact` is two `StyleAtomID`s, each a transparent `u32`, laid out as `[u32; 2]`, as the
        //         asserts above check, and any two `u32`s make a valid one.
        unsafe { ak::slice_from_raw_parts(raw.as_ptr().cast(), raw.len()) }
    }
}

/// The other names one attribute-name atom answers to.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AttributeNameForms {
    pub local: StyleAtomID,
    pub folded_name: StyleAtomID,
    pub folded_local: StyleAtomID,
}

/// One compact key for selector dispatch and relational arrival keys.
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
    Directionality(StyleAtomID),
    Root,
    State(StateFact),
    Heading,
    Universal,
    Structural,
    Language,
}

impl FeatureKey {
    pub(crate) fn atom(self) -> Option<StyleAtomID> {
        match self {
            Self::Part(atom)
            | Self::CustomState(atom)
            | Self::TagName(atom)
            | Self::Id(atom)
            | Self::Class(atom)
            | Self::AttributeName(atom)
            | Self::Directionality(atom) => Some(atom),
            Self::Root | Self::State(_) | Self::Heading | Self::Universal | Self::Structural | Self::Language => None,
        }
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

#[cfg(test)]
mod tests {
    use super::*;

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
    fn attribute_facts_keep_only_their_name_and_value_atoms() {
        assert_eq!(size_of::<AttributeFact>(), 8);
    }

    #[test]
    fn selector_feature_keys_are_eight_bytes() {
        assert_eq!(size_of::<FeatureKey>(), 8);
    }
}
