/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The ancestor Bloom filter: a counting filter over the ids, classes and tag names of the elements
//! above the one being matched, maintained as the traversal descends and climbs. A selector whose
//! combinator chain requires an ancestor key the filter lacks cannot match, so its ancestor walk is
//! never run.

use super::super::*;
use crate::css::selector_arena::IdOrClass;
use smallvec::SmallVec;

const KEY_BITS: u32 = 12;
const SLOT_COUNT: usize = 1 << KEY_BITS;
const KEY_MASK: u32 = (1 << KEY_BITS) - 1;

/// The most ancestor keys one selector checks against the filter. More would cost more probes than
/// the rejections they add.
const MAX_REQUIRED_HASHES: usize = 4;

/// The bits of a hash, all of which the filters read.
const HASH_BITS: u32 = 2 * KEY_BITS;
const HASH_MASK: u32 = (1 << HASH_BITS) - 1;

/// A filter hash of `HASH_BITS` bits, never zero, so that zero can stand for no hash.
fn hash_of(kind: u32, atom: StyleAtomID) -> u32 {
    let mixed = (u64::from(atom.0) | (u64::from(kind) << 32)).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    ((mixed >> 32) as u32 & HASH_MASK).max(1)
}

pub(super) fn id_hash(atom: StyleAtomID) -> u32 {
    hash_of(1, atom)
}

pub(super) fn class_hash(atom: StyleAtomID) -> u32 {
    hash_of(2, atom)
}

pub(super) fn tag_hash(atom: StyleAtomID) -> u32 {
    hash_of(3, atom)
}

pub(super) fn attribute_hash(atom: StyleAtomID) -> u32 {
    hash_of(4, atom)
}

/// The ancestor keys a selector entry requires, as filter hashes, in a fixed number of slots so a
/// rule bucket holds them inline: three in the low bits of the words, and the fourth spread over
/// their high bytes. An unused slot is zero.
#[derive(Clone, Copy, Default)]
pub(super) struct AncestorHashes {
    packed: [u32; 3],
}

const _: () = assert!(size_of::<AncestorHashes>() == 12);
const _: () = assert!(HASH_BITS == 24 && MAX_REQUIRED_HASHES == 4);

impl AncestorHashes {
    /// The keys every ancestor compound reached through `A B` and `A > B` from `compound`
    /// necessarily carries, past `A + B` and `A ~ B` too, since siblings share their ancestors.
    /// Only features standing directly in such a compound count, and those of the one selector of
    /// a `:is()`, `:where()` or `&` holding a single one: anything under a negation, a disjunction or
    /// a relative query may hold elsewhere or not at all. Only names written in ASCII lowercase
    /// count, which every document carries as written, and attribute names only where no namespace
    /// is named.
    pub(super) fn required_left_of(compound: crate::css::selector::Selector<'_>) -> Self {
        let mut hashes: SmallVec<[u32; MAX_REQUIRED_HASHES]> = SmallVec::new();
        collect_ancestor_hashes(compound, false, &mut hashes);
        let slot = |index: usize| hashes.get(index).copied().unwrap_or(0);
        let fourth = slot(3);
        Self {
            packed: std::array::from_fn(|index| slot(index) | ((fourth >> (8 * index)) & 0xFF) << HASH_BITS),
        }
    }

    /// Whether no key is required of the ancestors.
    pub(super) fn is_empty(&self) -> bool {
        self.hashes().next().is_none()
    }

    /// The words the hashes are packed in, for a holder to keep those not zero.
    pub(super) fn packed_words(&self) -> [u32; 3] {
        self.packed
    }

    pub(super) fn from_packed_words(packed: [u32; 3]) -> Self {
        Self { packed }
    }

    /// The hashes, without the unused slots.
    fn hashes(&self) -> impl Iterator<Item = u32> {
        let [first, second, third] = self.packed;
        let fourth = (first >> HASH_BITS) | (second >> HASH_BITS) << 8 | (third >> HASH_BITS) << 16;
        [first & HASH_MASK, second & HASH_MASK, third & HASH_MASK, fourth]
            .into_iter()
            .filter(|&hash| hash != 0)
    }
}

/// Add the keys of the compounds of `selector` an ancestor of its subject matches to `hashes`, from
/// the compound left of the subject's, or from the subject's own where `from_subject`, as for the
/// one selector of a `:is()` standing in a compound an ancestor matches.
fn collect_ancestor_hashes(
    selector: crate::css::selector::Selector<'_>,
    from_subject: bool,
    hashes: &mut SmallVec<[u32; MAX_REQUIRED_HASHES]>,
) {
    use crate::css::selector::Combinator;
    let mut current = selector;
    let mut collects = from_subject;
    loop {
        if collects && !collect_compound_hashes(current, hashes) {
            return;
        }
        let Some((combinator, left)) = current.left() else {
            return;
        };
        current = left;
        collects = match combinator {
            Combinator::Descendant | Combinator::ImmediateChild => true,
            // A sibling's own keys say nothing of the subject's ancestors.
            Combinator::NextSibling | Combinator::SubsequentSibling => false,
            _ => return,
        };
    }
}

/// Add the keys of the first compound of `compound`, one an ancestor matches, to `hashes`, or
/// answer false where the walk goes no further: the compound names the host, which stands above a
/// shadow tree's elements in no tree the filter walks.
fn collect_compound_hashes(
    compound: crate::css::selector::Selector<'_>,
    hashes: &mut SmallVec<[u32; MAX_REQUIRED_HASHES]>,
) -> bool {
    use crate::css::selector::{NamespaceType, PseudoClassType, SimpleSelector};
    use crate::css::selector_arena::Nesting;
    let simple_selectors = compound.first_compound().simple_selectors;
    if simple_selectors.iter().any(|simple| {
        super::super::component_matching::mentions(&simple, &|simple| {
            matches!(simple, SimpleSelector::PseudoClass(pseudo_class) if pseudo_class.pseudo_class == PseudoClassType::Host)
        })
    }) {
        return false;
    }
    let push = |hash: u32, hashes: &mut SmallVec<[u32; MAX_REQUIRED_HASHES]>| {
        if hashes.len() < MAX_REQUIRED_HASHES && !hashes.contains(&hash) {
            hashes.push(hash);
        }
    };
    for simple in simple_selectors.iter() {
        let nested = match &simple {
            SimpleSelector::Id(name) if name.is_ascii_lowercase => {
                push(id_hash(name.name), hashes);
                None
            }
            SimpleSelector::Class(name) if name.is_ascii_lowercase => {
                push(class_hash(name.name), hashes);
                None
            }
            SimpleSelector::TagName(name) if name.name_is_ascii_lowercase => {
                push(tag_hash(name.name), hashes);
                None
            }
            SimpleSelector::Attribute(attribute)
                if attribute.qualified_name.name_is_ascii_lowercase
                    && !matches!(
                        attribute.qualified_name.namespace_type,
                        NamespaceType::Named | NamespaceType::Any
                    ) =>
            {
                push(attribute_hash(attribute.qualified_name.name), hashes);
                None
            }
            SimpleSelector::PseudoClass(pseudo_class)
                if matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where) =>
            {
                pseudo_class.argument_selector_list.single()
            }
            SimpleSelector::Nesting(Nesting::Rule(parent)) => parent.single(),
            _ => None,
        };
        // NB: The one selector of a `:is()` describes the element its compound does, from its own
        //     subject leftwards.
        if let Some(nested) = nested {
            collect_ancestor_hashes(nested, true, hashes);
        }
    }
    true
}

/// A counting Bloom filter of the keys of the current element's ancestors.
pub(super) struct AncestorFilter {
    counters: Box<[u8; SLOT_COUNT]>,
}

impl AncestorFilter {
    /// The bytes the filter holds on the heap.
    pub(super) fn heap_size(&self) -> usize {
        size_of_val(&*self.counters)
    }

    pub(super) fn new() -> Self {
        Self {
            counters: Box::new([0; SLOT_COUNT]),
        }
    }

    fn slots(hash: u32) -> (usize, usize) {
        ((hash & KEY_MASK) as usize, ((hash >> KEY_BITS) & KEY_MASK) as usize)
    }

    /// Hold every key, rejecting nothing.
    pub(super) fn fill(&mut self) {
        self.counters.fill(u8::MAX);
    }

    pub(super) fn insert(&mut self, hash: u32) {
        let (first, second) = Self::slots(hash);
        self.counters[first] = self.counters[first].saturating_add(1);
        self.counters[second] = self.counters[second].saturating_add(1);
    }

    /// Removes a hash inserted before. A saturated counter stays saturated, which only costs the
    /// filter some rejections.
    pub(super) fn remove(&mut self, hash: u32) {
        let (first, second) = Self::slots(hash);
        for slot in [first, second] {
            if self.counters[slot] != u8::MAX {
                self.counters[slot] -= 1;
            }
        }
    }

    pub(super) fn might_contain_all(&self, hashes: &AncestorHashes) -> bool {
        hashes.hashes().all(|hash| {
            let (first, second) = Self::slots(hash);
            self.counters[first] != 0 && self.counters[second] != 0
        })
    }
}

/// A Bloom filter of the keys of the earlier siblings of the element being matched, which a
/// subsequent-sibling combinator rejects on as a descendant combinator does on the ancestor filter.
#[derive(Clone, Copy, Default)]
pub(super) struct SiblingFilter {
    bits: [u64; 4],
}

impl SiblingFilter {
    fn bits_of(hash: u32) -> [(usize, u64); 2] {
        [hash & 0xff, (hash >> 8) & 0xff].map(|bit| ((bit / 64) as usize, 1_u64 << (bit % 64)))
    }

    /// Hold every key, rejecting nothing.
    pub(super) fn fill(&mut self) {
        self.bits = [u64::MAX; 4];
    }

    pub(super) fn insert(&mut self, hash: u32) {
        for (word, bit) in Self::bits_of(hash) {
            self.bits[word] |= bit;
        }
    }

    pub(super) fn might_contain(&self, hash: u32) -> bool {
        Self::bits_of(hash)
            .iter()
            .all(|&(word, bit)| self.bits[word] & bit != 0)
    }
}

/// The filter hash of a key every element the compound left of the subject of `selector` matches carries, where the
/// subject is right of `~`: what the filter of an element's earlier siblings rejects the selector on. Only names
/// written in ASCII lowercase count, which every document carries as written.
pub(super) fn earlier_sibling_key(selector: crate::css::selector::Selector<'_>) -> Option<u32> {
    let (combinator, left) = selector.left()?;
    if combinator != crate::css::selector::Combinator::SubsequentSibling || left.is_relative_anchor() {
        return None;
    }
    let mut id = None;
    for name in left.ids_and_classes() {
        match name {
            IdOrClass::Class(class) if class.is_ascii_lowercase => return Some(class_hash(class.name)),
            IdOrClass::Id(name) if name.is_ascii_lowercase => id = id.or(Some(id_hash(name.name))),
            _ => {}
        }
    }
    id
}

#[cfg(test)]
mod tests {
    use super::*;

    fn required_hash_count(text: &str) -> usize {
        let parsed = crate::css::selector_parser::parse_selector_list(
            &text.encode_utf16().collect::<Vec<u16>>()[..],
            &[],
            crate::css::selector_parser::SelectorType::Standalone,
            crate::css::selector_parser::SelectorParsingMode::Standard,
        )
        .expect("a test writes valid selectors");
        let selectors = crate::css::selector::SelectorListRef::pack(&parsed.selectors);
        let selector = selectors.iter().next().expect("a test writes a selector");
        AncestorHashes::required_left_of(selector).hashes().count()
    }

    #[test]
    fn ancestors_are_required_to_carry_attribute_names_and_the_keys_of_a_lone_is() {
        for (selector, count) in [
            ("[data-x] .a", 1),
            ("[data-X] .a", 0),
            ("[*|data-x] .a", 0),
            (".p > [data-x=y] .a", 2),
            (":is(.p .q) .a", 2),
            (":where(.p) > .a", 1),
            (":is(.p, .q) .a", 0),
            (":not(.p) .a", 0),
            (".p :is(.q + .r) .a", 2),
            (".s + .a", 0),
            (":is(.p) .a:is(.q .r)", 1),
        ] {
            assert_eq!(required_hash_count(selector), count, "{selector}");
        }
    }
}
