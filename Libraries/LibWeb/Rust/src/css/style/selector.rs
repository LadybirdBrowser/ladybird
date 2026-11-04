/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a match keeps as it runs: where elements stand among their siblings, the answers of the
//! tests that read them, and what walks back across `~` found, with the attribute value tests.

use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::fast_hash::FastMap as HashMap;
use super::index::StyleAtomID;
use smallvec::SmallVec;
use std::hash::Hash;

use super::selector_evaluation::SelectorEvaluator;
use super::selector_evaluation::SelectorSubject;
use super::tree::StyleNodeID;
pub use crate::css::selector::Specificity;

/// An `an+b` positional test, optionally restricted to siblings sharing a type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct NthPosition {
    pub step: i32,
    pub offset: i32,
    /// `:nth-last-child()` and `:nth-last-of-type()` count from the end.
    pub from_end: bool,
    /// `:nth-of-type()` family: only siblings sharing the subject's type contribute.
    pub of_type: bool,
}

/// The single axis a simple relative selector traverses from its anchor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RelativeAxis {
    /// `:has(.error)`
    Descendant,
    /// `:has(> .item)`
    Child,
    /// `:has(+ .selected)`
    NextSibling,
    /// `:has(~ .selected)`
    FollowingSibling,
    /// `:has(+ .row .cell)`: the witness is somewhere under the anchor's next sibling.
    NextSiblingSubtree,
    /// `:has(~ .row .cell)`: the witness is somewhere under one of the following siblings.
    FollowingSiblingSubtree,
}

/// Why a match evaluation could not produce an exact answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Incomplete {
    /// The fact batch does not cover a style node the evaluation had to read. This is never a
    /// negative answer: the caller widens the batch or asks a different question.
    MissingFacts(StyleNodeID),
}

/// Which positional tests an evaluator answers from its workspace's shared sibling index.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum PositionalIndexPolicy {
    /// Every test, and every answer is memoized. A traversal that styles whole sequences asks
    /// many programs about the same children, which amortizes building each index.
    All,
    /// Only tests with a step, which have to count the whole sequence anyway, and no memo. A
    /// bounded test such as `:first-child` stops at its near end, so a narrow exact ask over
    /// scattered candidates answers it more cheaply than materializing every sequence it walks
    /// through - and asks the same test about the same node too rarely for a memo to pay for
    /// its insertion.
    SteppedOnly,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub(super) struct SiblingPositions {
    pub(super) from_start: u32,
    pub(super) from_end: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct SiblingSequenceMembership {
    sequence: u32,
    ordinal: u32,
}

const VALUE_PAGE_SHIFT: usize = 6;
const VALUE_PAGE_SIZE: usize = 1 << VALUE_PAGE_SHIFT;

#[derive(Clone)]
struct ValuePage<T: Copy + Default> {
    known: u64,
    values: [T; VALUE_PAGE_SIZE],
}

impl<T: Copy + Default> Default for ValuePage<T> {
    fn default() -> Self {
        Self {
            known: 0,
            values: [T::default(); VALUE_PAGE_SIZE],
        }
    }
}

impl<T: Copy + Default> PagedColumnPage for ValuePage<T> {
    type Value = T;

    const SHIFT: usize = VALUE_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<T> {
        (self.known & (1 << index) != 0).then_some(self.values[index])
    }

    fn insert(&mut self, index: usize, value: T) {
        self.known |= 1 << index;
        self.values[index] = value;
    }
}

type SiblingPositionColumn = PagedColumn<ValuePage<SiblingPositions>>;
type SiblingSequenceMembershipColumn = PagedColumn<ValuePage<SiblingSequenceMembership>>;

#[derive(Clone, Default)]
pub(super) struct SiblingSequenceGeometry {
    sequences: Vec<Box<[StyleNodeID]>>,
    memberships: SiblingSequenceMembershipColumn,
}

impl SiblingSequenceGeometry {
    fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.sequences)
            + self
                .sequences
                .iter()
                .map(|sequence| size_of_val(&**sequence))
                .sum::<usize>()
            + self.memberships.heap_size()
    }

    fn insert_sequence(&mut self, children: Vec<StyleNodeID>) {
        u32::try_from(children.len()).expect("sibling sequence space exhausted");
        let sequence = u32::try_from(self.sequences.len()).expect("sibling sequence index space exhausted");
        for (ordinal, &child) in children.iter().enumerate() {
            self.memberships.insert(
                child
                    .element_index()
                    .expect("only elements belong to sibling sequences") as usize,
                SiblingSequenceMembership {
                    sequence,
                    ordinal: u32::try_from(ordinal).expect("sibling sequence space exhausted"),
                },
            );
        }
        self.sequences.push(children.into_boxed_slice());
    }

    fn sequence_and_ordinal(&self, node: StyleNodeID) -> Option<(&[StyleNodeID], u32)> {
        let membership = self.memberships.get(node.element_index()? as usize)?;
        Some((&*self.sequences[membership.sequence as usize], membership.ordinal))
    }

    fn sibling_sequence(&self, node: StyleNodeID) -> Option<&[StyleNodeID]> {
        self.sequence_and_ordinal(node).map(|(sequence, _)| sequence)
    }

    pub(super) fn sibling_positions(&self, node: StyleNodeID) -> Option<SiblingPositions> {
        let (sequence, ordinal) = self.sequence_and_ordinal(node)?;
        let count = u32::try_from(sequence.len()).expect("sibling sequence space exhausted");
        Some(SiblingPositions {
            from_start: ordinal + 1,
            from_end: count - ordinal,
        })
    }
}

/// Caller-owned scratch lent exclusively to one evaluator at a time, which reads one tree: the
/// positions and answers it keeps are of that tree.
#[derive(Clone, Default)]
pub struct MatchScratch {
    sibling_geometry: SiblingSequenceGeometry,
    type_positions: SiblingPositionColumn,
    /// The answers of relative queries, by the anchor each was asked for.
    relative_answers: HashMap<RelativeAnswerKey, bool>,
    /// How many of those answers took a walk: each one here did, as an evaluation that finds one takes no walk.
    relative_walks: u64,
    /// The number of the table of what walks back across a subsequent-sibling combinator found, by
    /// what the walks start from.
    sibling_tables: HashMap<SiblingAnswerKey, u32>,
    /// The pages of each table, by the page of the element slots they hold, or `NO_SIBLING_PAGE`:
    /// a table holds pages for the elements its walks passed alone, which a pass styling a few
    /// elements of a large document keeps small.
    sibling_answers: Vec<Vec<u32>>,
    /// What the tables' walks found for each sibling they passed, by its element slot in its
    /// table's page: none yet, false or true.
    sibling_answer_pages: Vec<[u8; SIBLING_ANSWER_PAGE]>,
}

/// How many element slots a page of a sibling answer table holds.
const SIBLING_ANSWER_PAGE: usize = 128;
const NO_SIBLING_PAGE: u32 = u32::MAX;

/// The table of what walks back across one `~` found, among a workspace's.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) struct SiblingAnswerTable(u32);

/// What a walk back across `~` found depends on besides the facts of the tree: the compound left of
/// the `~`, and the bindings and part exposure it was evaluated under.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct SiblingAnswerKey {
    pub(super) selector: super::selector_evaluation::SelectorKey,
    pub(super) scope_shadow_root: Option<StyleNodeID>,
    pub(super) scope_root_instance: Option<StyleNodeID>,
    pub(super) part_exposure_scope: Option<super::tree::TreeScopeID>,
}

/// What a `:has()` argument list's answer for one anchor depends on besides the facts of the tree:
/// the list, and the bindings it was evaluated under, but for the anchor of an enclosing query,
/// which the query binds to its own.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct RelativeAnswerKey {
    list: super::selector_evaluation::SelectorListKey,
    anchor: StyleNodeID,
    scope_shadow_root: Option<StyleNodeID>,
    scope_root_instance: Option<StyleNodeID>,
    matching_host_argument: bool,
}

impl RelativeAnswerKey {
    /// The key of the `:has()` argument list `list`, asked for `anchor` under `bindings`.
    pub(super) fn of_list(
        list: super::selector_evaluation::SelectorListKey,
        anchor: StyleNodeID,
        bindings: &super::selector_evaluation::SelectorBindings<StyleNodeID>,
    ) -> Self {
        Self {
            list,
            anchor,
            scope_shadow_root: bindings.scope_shadow_root,
            scope_root_instance: bindings.scope_root_instance,
            matching_host_argument: bindings.matching_host_argument,
        }
    }
}

/// Where a subject's sibling sequences come from, for [`MatchScratch::indexed_sibling_position`]:
/// the parent of the element asked about and the children of a parent in the tree it reads, and
/// the type of a sibling as its facts tell it.
pub(super) struct SiblingSequenceSource<Children, TypeOf> {
    pub(super) parent: Option<StyleNodeID>,
    pub(super) children: Children,
    pub(super) type_of: TypeOf,
}

impl MatchScratch {
    /// The bytes the scratch keeps on the heap: the sibling sequences, positions and answers it
    /// measured.
    pub(super) fn heap_size(&self) -> usize {
        self.sibling_geometry.heap_size()
            + self.type_positions.heap_size()
            + crate::heap_size::map_bytes(&self.relative_answers)
            + crate::heap_size::map_bytes(&self.sibling_tables)
            + crate::heap_size::vec_bytes(&self.sibling_answers)
            + self
                .sibling_answers
                .iter()
                .map(crate::heap_size::vec_bytes)
                .sum::<usize>()
            + crate::heap_size::vec_bytes(&self.sibling_answer_pages)
    }

    /// Return a sibling position from this scratch, building that sequence on its first ask.
    ///
    /// A broad matching batch asks many positional selectors about the same children. Counting from
    /// an end for every `(selector, child)` pair is quadratic in the sequence length and repeated
    /// across rules.
    pub(super) fn indexed_sibling_position<Children, TypeOf>(
        &mut self,
        position: NthPosition,
        node: StyleNodeID,
        source: SiblingSequenceSource<Children, TypeOf>,
    ) -> Result<Option<i64>, Incomplete>
    where
        Children: FnOnce(StyleNodeID) -> Vec<StyleNodeID>,
        TypeOf: FnMut(StyleNodeID) -> Result<(StyleAtomID, StyleAtomID), Incomplete>,
    {
        let SiblingSequenceSource {
            parent,
            children,
            mut type_of,
        } = source;
        let answer = |cached: SiblingPositions| {
            Ok(Some(i64::from(match position.from_end {
                true => cached.from_end,
                false => cached.from_start,
            })))
        };
        if let Some(cached) = self.sibling_position(node, position.of_type) {
            return answer(cached);
        }

        let Some(parent) = parent else {
            if position.of_type {
                type_of(node)?;
            }
            return Ok(Some(1));
        };
        if self.sibling_geometry.sibling_sequence(node).is_none() {
            self.sibling_geometry.insert_sequence(children(parent));
        }
        let siblings = self.sibling_geometry.sibling_sequence(node).unwrap();
        if siblings.is_empty() {
            return Ok(None);
        }

        if position.of_type {
            let mut type_ids = HashMap::default();
            let mut sibling_types = Vec::with_capacity(siblings.len());
            let mut type_positions: SmallVec<[SiblingPositions; 1]> = SmallVec::new();
            for &sibling in siblings {
                let sibling_type = type_of(sibling)?;
                let next_type_id = u32::try_from(type_positions.len()).expect("sibling type space exhausted");
                let type_id = *type_ids.entry(sibling_type).or_insert_with(|| {
                    type_positions.push(SiblingPositions {
                        from_start: 0,
                        from_end: 0,
                    });
                    next_type_id
                });
                sibling_types.push(type_id);
                type_positions[type_id as usize].from_end += 1;
            }
            let siblings = self.sibling_geometry.sibling_sequence(node).unwrap();
            let positions = &mut self.type_positions;
            for (&sibling, type_id) in siblings.iter().zip(sibling_types) {
                let position = &mut type_positions[type_id as usize];
                position.from_start += 1;
                positions.insert(
                    sibling.element_index().expect("only elements have sibling positions") as usize,
                    *position,
                );
                position.from_end -= 1;
            }
        }

        match self.sibling_position(node, position.of_type) {
            Some(cached) => answer(cached),
            None => Ok(None),
        }
    }

    pub(super) fn sibling_position(&self, node: StyleNodeID, of_type: bool) -> Option<SiblingPositions> {
        match of_type {
            true => self.type_positions.get(node.element_index()? as usize),
            false => self.sibling_geometry.sibling_positions(node),
        }
    }

    pub(super) fn relative_walks(&self) -> u64 {
        self.relative_walks
    }

    pub(super) fn relative_answer(&self, key: &RelativeAnswerKey) -> Option<bool> {
        self.relative_answers.get(key).copied()
    }

    pub(super) fn insert_relative_answer(&mut self, key: RelativeAnswerKey, answer: bool) {
        self.relative_walks += 1;
        self.relative_answers.insert(key, answer);
    }

    /// The table of the walks `key` says.
    pub(super) fn sibling_table(&mut self, key: SiblingAnswerKey) -> SiblingAnswerTable {
        let next = u32::try_from(self.sibling_answers.len()).expect("sibling answer tables exhausted");
        let table = *self.sibling_tables.entry(key).or_insert(next);
        if table == next {
            self.sibling_answers.push(Vec::new());
        }
        SiblingAnswerTable(table)
    }

    pub(super) fn sibling_answer(&self, table: SiblingAnswerTable, node: StyleNodeID) -> Option<bool> {
        let slot = node.element_slot();
        let page = *self.sibling_answers[table.0 as usize].get(slot / SIBLING_ANSWER_PAGE)?;
        if page == NO_SIBLING_PAGE {
            return None;
        }
        match self.sibling_answer_pages[page as usize][slot % SIBLING_ANSWER_PAGE] {
            1 => Some(false),
            2 => Some(true),
            _ => None,
        }
    }

    pub(super) fn insert_sibling_answer(&mut self, table: SiblingAnswerTable, node: StyleNodeID, answer: bool) {
        let slot = node.element_slot();
        let pages = &mut self.sibling_answers[table.0 as usize];
        if pages.len() <= slot / SIBLING_ANSWER_PAGE {
            pages.resize(slot / SIBLING_ANSWER_PAGE + 1, NO_SIBLING_PAGE);
        }
        let page = &mut pages[slot / SIBLING_ANSWER_PAGE];
        if *page == NO_SIBLING_PAGE {
            *page = u32::try_from(self.sibling_answer_pages.len()).expect("sibling answer pages exhausted");
            self.sibling_answer_pages.push([0; SIBLING_ANSWER_PAGE]);
        }
        self.sibling_answer_pages[*page as usize][slot % SIBLING_ANSWER_PAGE] = 1 + u8::from(answer);
    }
}

/// What matching the entries of the rule index asks of an evaluator, against whichever elements it
/// is matched against.
impl<S> SelectorEvaluator<S>
where
    S: SelectorSubject<Node = StyleNodeID, Incomplete = Incomplete>,
{
    /// Evaluate the rules attached to the shadow tree of `shadow_root` from here on, or the
    /// document's for none.
    pub(super) fn set_scope_shadow_root(&mut self, shadow_root: Option<StyleNodeID>) {
        self.bindings.scope_shadow_root = shadow_root;
    }
}

/// `index` is a 1-based position; the test is whether it equals `step * n + offset` for some
/// non-negative integer `n`.
pub(super) fn matches_an_plus_b(step: i32, offset: i32, index: i64) -> bool {
    let step = i64::from(step);
    let offset = i64::from(offset);
    if step == 0 {
        return index == offset;
    }
    let difference = index - offset;
    difference % step == 0 && difference / step >= 0
}

fn fold(unit: u16) -> u16 {
    if (u16::from(b'A')..=u16::from(b'Z')).contains(&unit) {
        return unit + u16::from(b'a' - b'A');
    }
    unit
}

pub(crate) fn attribute_value_matches<T: Copy + Into<u16>>(
    operator: crate::css::selector::AttributeMatchType,
    value: &[T],
    literal: &[u16],
    insensitive: bool,
) -> bool {
    use crate::css::selector::AttributeMatchType;
    match operator {
        AttributeMatchType::HasAttribute => true,
        AttributeMatchType::ExactValue => equals(value, literal, insensitive),
        AttributeMatchType::ContainsWord => {
            !literal.is_empty()
                && value
                    .split(|unit| matches!((*unit).into(), 0x20 | 0x09 | 0x0A | 0x0C | 0x0D))
                    .any(|token| equals(token, literal, insensitive))
        }
        AttributeMatchType::StartsWithSegment => {
            equals(value, literal, insensitive)
                || (value.len() > literal.len()
                    && value[literal.len()].into() == u16::from(b'-')
                    && starts_with(value, literal, insensitive))
        }
        AttributeMatchType::StartsWithString => !literal.is_empty() && starts_with(value, literal, insensitive),
        AttributeMatchType::EndsWithString => {
            !literal.is_empty()
                && value.len() >= literal.len()
                && equals(&value[value.len() - literal.len()..], literal, insensitive)
        }
        AttributeMatchType::ContainsString => {
            !literal.is_empty()
                && value.len() >= literal.len()
                && (0..=value.len() - literal.len())
                    .any(|start| equals(&value[start..start + literal.len()], literal, insensitive))
        }
    }
}

fn equals<T: Copy + Into<u16>>(value: &[T], literal: &[u16], insensitive: bool) -> bool {
    if !insensitive {
        return value.len() == literal.len() && value.iter().zip(literal).all(|(&left, &right)| left.into() == right);
    }
    value.len() == literal.len()
        && value
            .iter()
            .zip(literal)
            .all(|(&left, &right)| fold(left.into()) == fold(right))
}

fn starts_with<T: Copy + Into<u16>>(value: &[T], literal: &[u16], insensitive: bool) -> bool {
    value.len() >= literal.len() && equals(&value[..literal.len()], literal, insensitive)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_plus_b_handles_the_boundary_forms() {
        assert!(matches_an_plus_b(0, 1, 1));
        assert!(!matches_an_plus_b(0, 1, 2));
        assert!(matches_an_plus_b(2, 0, 2));
        assert!(!matches_an_plus_b(2, 0, 3));
        assert!(matches_an_plus_b(2, 1, 1));
        assert!(matches_an_plus_b(-1, 3, 3));
        assert!(matches_an_plus_b(-1, 3, 1));
        assert!(!matches_an_plus_b(-1, 3, 4));
        assert!(matches_an_plus_b(1, 0, 5));
    }
}
