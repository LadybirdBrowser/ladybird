/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Flow-sensitive facts, which let check elimination drop redundant checks
//! and branches and reuse loads.
//!
//! Facts are about SSA values (their kind, shapes, elements kind and the
//! constants checks compared them with) and about the contents of locations
//! (a named property of an object, a global binding, an element count).
//! Each fact holds until a node writes the locations it depends on (see
//! `Facts::apply()`), with two exceptions:
//! - Immutable facts hold forever: the kind of a value, the constant a check
//!   compared it with, and the elements kind of a typed array.
//! - Dependency facts, the shapes of objects that are stable (see
//!   `Dependency::StableShape`), survive other code that runs, while the
//!   compiled code stays valid: they are unvalidated until the next
//!   `Op::AssumeValid`.
//!
//! Objects compiled code allocated that nothing but accesses of their
//! properties has seen yet keep their facts too: no other code can reach
//! them.

use crate::code::Repr;
use crate::ir::BranchCondition;
use crate::ir::ElementsKind;
use crate::ir::Graph;
use crate::ir::Locations;
use crate::ir::NodeId;
use crate::ir::NodeProperties;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::snapshot::CellId;
use ak::Zone;
use ak::ZoneMap;
use ak::ZoneVec;

/// A list of facts in a zone, which facts at several points may share: FROZEN, a list that nothing changes anymore,
/// or OWNED, a list that only one `Facts` sees and changes in place. A change of a frozen list changes a copy of it.
#[derive(Debug)]
enum Part<'w, T> {
    Frozen(&'w [T]),
    Owned(ZoneVec<'w, T>),
}

impl<'w, T: Copy> Part<'w, T> {
    fn new() -> Self {
        Part::Frozen(&[])
    }

    fn as_slice(&self) -> &[T] {
        match self {
            Part::Frozen(values) => values,
            Part::Owned(values) => values,
        }
    }

    /// The list, to change it: a copy in `zone` if it is frozen.
    fn make_mut(&mut self, zone: &'w Zone) -> &mut ZoneVec<'w, T> {
        if let Part::Frozen(values) = *self {
            *self = Part::Owned(ZoneVec::from_slice_in(values, zone));
        }
        match self {
            Part::Owned(values) => values,
            Part::Frozen(_) => unreachable!("the list was copied"),
        }
    }

    /// Freezes the list, and returns another handle of it.
    fn share(&mut self) -> Self {
        let values: &'w [T] = match core::mem::replace(self, Part::new()) {
            Part::Frozen(values) => values,
            Part::Owned(values) => values.into_slice(),
        };
        *self = Part::Frozen(values);
        Part::Frozen(values)
    }

    /// Whether both are handles of one frozen list. Owned lists are never the same as another.
    fn is_same(&self, other: &Self) -> bool {
        match (self, other) {
            (Part::Frozen(values), Part::Frozen(other)) => core::ptr::eq(*values, *other),
            _ => false,
        }
    }

    fn take(&mut self) -> Self {
        core::mem::replace(self, Part::new())
    }

    fn clear(&mut self) {
        match self {
            Part::Owned(values) => values.clear(),
            Part::Frozen(_) => *self = Part::new(),
        }
    }

    /// A copy of the list in the zone of `moved`, with `convert` applied to each value. A frozen list that was moved
    /// before is not copied again: its copy is in `moved`, so the lists that facts share stay shared.
    fn move_to<'n, U: Copy>(&self, moved: &mut MovedLists<'n, U>, convert: impl FnMut(&T) -> U) -> Part<'n, U> {
        match self {
            Part::Frozen(values) => Part::Frozen(moved.copy(values, convert)),
            Part::Owned(values) => Part::Owned(ZoneVec::from_iter_in(values.iter().map(convert), moved.zone)),
        }
    }

    /// `move_to()` of a list whose values stay as they are, which copies them in one piece.
    fn move_copied<'n>(&self, moved: &mut MovedLists<'n, T>) -> Part<'n, T> {
        match self {
            Part::Frozen(values) => Part::Frozen(moved.copy_as_is(values)),
            Part::Owned(values) => Part::Owned(ZoneVec::from_slice_in(values, moved.zone)),
        }
    }
}

/// The copies of the frozen lists of one type that a move of facts made, by the address and the length of each list
/// they copy.
struct MovedLists<'n, U> {
    zone: &'n Zone,
    copies: ZoneMap<'n, (usize, usize), &'n [U]>,
}

impl<'n, U: Copy> MovedLists<'n, U> {
    fn new_in(zone: &'n Zone) -> Self {
        Self {
            zone,
            copies: ZoneMap::new_in(zone),
        }
    }

    /// The copy of `values` in the zone, with `convert` applied to each value: the one made before, if any.
    fn copy<T>(&mut self, values: &[T], convert: impl FnMut(&T) -> U) -> &'n [U] {
        if values.is_empty() {
            return &[];
        }
        let key = (values.as_ptr() as usize, values.len());
        if let Some(copy) = self.copies.get(&key) {
            return copy;
        }
        let copy: &'n [U] = self.zone.alloc_slice_from_iter(values.iter().map(convert));
        self.copies.insert(key, copy);
        copy
    }

    /// `copy()` of values that stay as they are.
    fn copy_as_is(&mut self, values: &[U]) -> &'n [U] {
        if values.is_empty() {
            return &[];
        }
        let key = (values.as_ptr() as usize, values.len());
        if let Some(copy) = self.copies.get(&key) {
            return copy;
        }
        let copy: &'n [U] = self.zone.alloc_slice_copy(values);
        self.copies.insert(key, copy);
        copy
    }
}

/// What a move of facts to another zone copied so far, so that the copies of the lists that the facts share are shared
/// too (see `Facts::move_to()`).
pub(super) struct Moved<'n> {
    zone: &'n Zone,
    kinds: MovedLists<'n, (NodeId, Kinds)>,
    checked_values: MovedLists<'n, (NodeId, u64)>,
    elements: MovedLists<'n, (NodeId, ElementsKind)>,
    shapes: MovedLists<'n, (NodeId, &'n [ShapeCheck])>,
    shape_lists: MovedLists<'n, ShapeCheck>,
    nodes: MovedLists<'n, NodeId>,
    cells: MovedLists<'n, CellId>,
    contents: MovedLists<'n, (Location, NodeId)>,
    refinements: MovedLists<'n, (NodeId, NodeId)>,
    branch_outcomes: MovedLists<'n, (BranchKey, bool)>,
}

impl<'n> Moved<'n> {
    /// Nothing moved yet, to `zone`, which also holds what this keeps.
    pub fn new_in(zone: &'n Zone) -> Self {
        Self {
            zone,
            kinds: MovedLists::new_in(zone),
            checked_values: MovedLists::new_in(zone),
            elements: MovedLists::new_in(zone),
            shapes: MovedLists::new_in(zone),
            shape_lists: MovedLists::new_in(zone),
            nodes: MovedLists::new_in(zone),
            cells: MovedLists::new_in(zone),
            contents: MovedLists::new_in(zone),
            refinements: MovedLists::new_in(zone),
            branch_outcomes: MovedLists::new_in(zone),
        }
    }
}

/// Sparse facts in key order. Copies and filtering walk one contiguous list
/// instead of separately allocated tree nodes.
#[derive(Debug)]
struct FactMap<'w, K, V>(Part<'w, (K, V)>);

impl<'w, K: Ord + Copy, V: Copy> FactMap<'w, K, V> {
    fn new() -> Self {
        Self(Part::new())
    }

    fn as_slice(&self) -> &[(K, V)] {
        self.0.as_slice()
    }

    fn find(&self, key: &K) -> Result<usize, usize> {
        self.as_slice().binary_search_by(|(known, _)| known.cmp(key))
    }

    fn get(&self, key: &K) -> Option<&V> {
        self.find(key).ok().map(|index| &self.as_slice()[index].1)
    }

    fn contains_key(&self, key: &K) -> bool {
        self.get(key).is_some()
    }

    fn insert(&mut self, zone: &'w Zone, key: K, value: V) {
        let entries = self.0.make_mut(zone);
        // Definitions usually arrive in increasing node order.
        if entries.last().is_none_or(|(last, _)| *last < key) {
            entries.push((key, value));
            return;
        }
        match entries.binary_search_by(|(known, _)| known.cmp(&key)) {
            Ok(index) => entries[index].1 = value,
            Err(index) => entries.insert(index, (key, value)),
        }
    }

    fn remove(&mut self, zone: &'w Zone, key: &K) {
        if let Ok(index) = self.find(key) {
            self.0.make_mut(zone).remove(index);
        }
    }

    fn keys(&self) -> impl Iterator<Item = &K> {
        self.as_slice().iter().map(|(key, _)| key)
    }

    fn retain(&mut self, zone: &'w Zone, mut keep: impl FnMut(&K, &mut V) -> bool) {
        self.0.make_mut(zone).retain_mut(|(key, value)| keep(key, value));
    }

    fn is_empty(&self) -> bool {
        self.as_slice().is_empty()
    }

    fn clear(&mut self) {
        self.0.clear();
    }

    fn share(&mut self) -> Self {
        Self(self.0.share())
    }
}

impl<K: PartialEq + Copy, V: PartialEq + Copy> PartialEq for FactMap<'_, K, V> {
    fn eq(&self, other: &Self) -> bool {
        self.0.is_same(&other.0) || self.0.as_slice() == other.0.as_slice()
    }
}

/// A sparse set of facts in key order (see `FactMap`).
#[derive(Debug)]
struct FactSet<'w, K>(Part<'w, K>);

impl<'w, K: Ord + Copy> FactSet<'w, K> {
    fn new() -> Self {
        Self(Part::new())
    }

    fn as_slice(&self) -> &[K] {
        self.0.as_slice()
    }

    fn contains(&self, key: &K) -> bool {
        self.as_slice().binary_search(key).is_ok()
    }

    fn insert(&mut self, zone: &'w Zone, key: K) {
        if let Err(index) = self.as_slice().binary_search(&key) {
            self.0.make_mut(zone).insert(index, key);
        }
    }

    fn remove(&mut self, zone: &'w Zone, key: &K) {
        if let Ok(index) = self.as_slice().binary_search(key) {
            self.0.make_mut(zone).remove(index);
        }
    }

    /// The elements, in key order.
    fn iter(&self) -> impl Iterator<Item = &K> {
        self.as_slice().iter()
    }

    fn retain(&mut self, zone: &'w Zone, keep: impl FnMut(&K) -> bool) {
        self.0.make_mut(zone).retain(keep);
    }

    fn is_empty(&self) -> bool {
        self.as_slice().is_empty()
    }

    fn clear(&mut self) {
        self.0.clear();
    }

    fn take(&mut self) -> Self {
        Self(self.0.take())
    }

    fn share(&mut self) -> Self {
        Self(self.0.share())
    }
}

impl<K: PartialEq + Copy> PartialEq for FactSet<'_, K> {
    fn eq(&self, other: &Self) -> bool {
        self.0.is_same(&other.0) || self.0.as_slice() == other.0.as_slice()
    }
}

/// A branch on a condition that tests nothing but the values of its inputs,
/// which never change, so that a branch decides every later one on the same
/// values: the condition, and the inputs, where constants are their values.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct BranchKey {
    condition: u64,
    inputs: [BranchInput; 2],
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum BranchInput {
    Absent,
    Value(NodeId),
    Constant(u64),
}

impl BranchKey {
    /// The key of a branch on `condition` of `inputs` (as the analysis knows
    /// them), if the condition tests their values alone.
    pub fn of(graph: &Graph, condition: BranchCondition, inputs: &[NodeId]) -> Option<Self> {
        let condition = match condition {
            BranchCondition::Bool => 0,
            BranchCondition::Int32(comparison) => 1 << 32 | comparison as u64,
            BranchCondition::Float64(comparison) => 2 << 32 | comparison as u64,
            BranchCondition::TaggedEquals { equal } => 3 << 32 | u64::from(equal),
            BranchCondition::Builtin(builtin) => 4 << 32 | u64::from(builtin),
            BranchCondition::BindingMutable { index } => 5 << 32 | u64::from(index),
            _ => return None,
        };
        let input = |index: usize| match inputs.get(index) {
            None => BranchInput::Absent,
            Some(input) => graph
                .constant_value(*input)
                .map_or(BranchInput::Value(*input), BranchInput::Constant),
        };
        if inputs.len() > 2 {
            return None;
        }
        Some(Self {
            condition,
            inputs: [input(0), input(1)],
        })
    }
}

/// What kind of value a value is known to be, which never changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueKind {
    Int32,
    /// A double that is not an int32.
    Double,
    /// An int32 or a double.
    Number,
    String,
    Object,
}

/// The kinds a value may be, as a set: what the facts about a value's kind
/// are. Branches on its kind narrow it on both edges, and merges unite it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Kinds(u8);

impl Kinds {
    const INT32: Self = Self(1 << 0);
    const DOUBLE: Self = Self(1 << 1);
    const STRING: Self = Self(1 << 2);
    const OBJECT: Self = Self(1 << 3);
    /// Every other kind of value: `undefined`, `null`, symbols and big
    /// integers.
    const OTHER: Self = Self(1 << 4);
    const BOOLEAN: Self = Self(1 << 5);
    const ALL: Self = Self((1 << 6) - 1);
    const NUMBER: Self = Self(Self::INT32.0 | Self::DOUBLE.0);

    fn of(kind: ValueKind) -> Self {
        match kind {
            ValueKind::Int32 => Self::INT32,
            ValueKind::Double => Self::DOUBLE,
            ValueKind::Number => Self::NUMBER,
            ValueKind::String => Self::STRING,
            ValueKind::Object => Self::OBJECT,
        }
    }

    /// The kind every value of the set is, if there is one.
    fn kind(self) -> Option<ValueKind> {
        Some(match self {
            Self::INT32 => ValueKind::Int32,
            Self::DOUBLE => ValueKind::Double,
            Self::NUMBER => ValueKind::Number,
            Self::STRING => ValueKind::String,
            Self::OBJECT => ValueKind::Object,
            _ => return None,
        })
    }

    /// The kinds that pass a branch on `condition` (of the value alone), and
    /// those that fail it, where the condition is about kinds.
    fn of_condition(condition: BranchCondition) -> Option<(Self, Self)> {
        let exactly = |kinds: Self| Some((kinds, Self(Self::ALL.0 & !kinds.0)));
        match condition {
            BranchCondition::Object => exactly(Self::OBJECT),
            BranchCondition::String => exactly(Self::STRING),
            BranchCondition::Int32Value => exactly(Self::INT32),
            BranchCondition::Int32OrBoolean => exactly(Self(Self::INT32.0 | Self::BOOLEAN.0)),
            BranchCondition::Double => exactly(Self::DOUBLE),
            // NB: These also depend on more than the kind: values of the
            //     kind may fail them.
            BranchCondition::NonNegativeInt32 => Some((Self::INT32, Self::ALL)),
            BranchCondition::ElementsKind(_) => Some((Self::OBJECT, Self::ALL)),
            // NB: Only objects can be like `undefined` (`document.all`).
            BranchCondition::Nullish | BranchCondition::Undefined => {
                Some((Self(Self::OTHER.0 | Self::OBJECT.0), Self::ALL))
            }
            _ => None,
        }
    }

    fn intersect(self, other: Self) -> Self {
        Self(self.0 & other.0)
    }

    fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

/// A location whose contents a fact knows.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(super) enum Location {
    /// The named property at `offset` of `object`.
    Named { object: NodeId, offset: u32 },
    /// Binding `index` of the global declarative environment `environment`.
    GlobalBinding { environment: CellId, index: u32 },
    /// Binding `index` of the declarative environment at `environment`, a
    /// `Repr::Pointer`, which may be a global declarative environment.
    Binding { environment: NodeId, index: u32 },
    /// An element count of `object`.
    ElementCount { count: ElementCount, object: NodeId },
    /// The cached storage offset of a typed array.
    TypedArrayDataOffset { object: NodeId },
    /// The element at `index` of the elements of `kind` of `storage`: the
    /// object whose elements they are (a typed array for its data address
    /// where the address is known to be its), or else the data address.
    Element {
        storage: NodeId,
        index: NodeId,
        kind: ElementsKind,
    },
}

/// The element counts of objects, which the op loading each names.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(super) enum ElementCount {
    /// `Op::LoadElementsLength`.
    Length,
    /// `Op::LoadElementsCapacity`.
    Capacity,
    /// `Op::LoadTypedArrayLength`.
    TypedArrayLength,
}

impl ElementCount {
    /// The count `op` loads, if it loads one.
    pub fn loaded_by(op: &Op) -> Option<Self> {
        match op {
            Op::LoadElementsLength => Some(Self::Length),
            Op::LoadElementsCapacity => Some(Self::Capacity),
            Op::LoadTypedArrayLength => Some(Self::TypedArrayLength),
            _ => None,
        }
    }
}

impl Location {
    /// The abstract locations whose writes change the contents.
    fn locations(self) -> Locations {
        match self {
            Location::Named { .. } => Locations::NAMED_SLOTS,
            Location::GlobalBinding { .. } => Locations::GLOBAL_BINDINGS,
            Location::Binding { .. } => Locations::BINDINGS.union(Locations::GLOBAL_BINDINGS),
            Location::ElementCount { .. } | Location::TypedArrayDataOffset { .. } => Locations::ELEMENT_COUNTS,
            Location::Element { .. } => Locations::ELEMENTS_AND_COUNTS,
        }
    }

    /// The object the location belongs to, if it is one's.
    fn object(self) -> Option<NodeId> {
        match self {
            Location::Named { object, .. }
            | Location::ElementCount { object, .. }
            | Location::TypedArrayDataOffset { object } => Some(object),
            // NB: Typed arrays may share their buffers, so every write of
            //     elements may change the elements of any storage.
            Location::GlobalBinding { .. } | Location::Binding { .. } | Location::Element { .. } => None,
        }
    }
}

/// What is known at one point of the program. Its lists are in a work zone, and the facts at other points may share
/// them (see `share()`).
#[derive(Debug)]
pub(super) struct Facts<'w> {
    /// Where changed lists go.
    zone: &'w Zone,
    /// The kinds values may be (immutable), where that is not every kind.
    kinds: FactMap<'w, NodeId, Kinds>,
    /// Values known to have passed a check against a constant, with that
    /// constant (immutable).
    checked_values: FactSet<'w, (NodeId, u64)>,
    /// Objects known to have elements of a kind (killed by writes of element
    /// counts, except for typed arrays, which keep their kind).
    elements: FactMap<'w, NodeId, ElementsKind>,
    /// For objects, the shapes they may have (killed by writes of shapes,
    /// except for stable shapes, which are dependency facts).
    shapes: FactMap<'w, NodeId, &'w [ShapeCheck]>,
    /// Objects whose shapes are known from before other code ran, which
    /// still hold only if the code is still valid (see `Op::AssumeValid`).
    unvalidated: FactSet<'w, NodeId>,
    /// Prototype chains known to be valid (killed by writes of shapes).
    valid_prototype_chains: FactSet<'w, CellId>,
    /// The known contents of locations, killed by writes of their
    /// locations (see `Location::locations()`).
    contents: FactMap<'w, Location, NodeId>,
    /// Objects compiled code allocated that nothing but accesses of their
    /// properties has seen yet, so nothing else that runs can change their
    /// shapes or properties.
    unexposed: FactSet<'w, NodeId>,
    /// Whether the code is known to be valid (see `Op::AssumeValid`): no
    /// other code ran since an `AssumeValid`.
    code_valid: bool,
    /// For values, the latest node that refines them (see `Op::Refine`) on
    /// every path here, or the value itself where it is what it is known to
    /// be by its definition: what replaces a check that is known to pass,
    /// so that the nodes using it stay behind the check that passed.
    refinements: FactMap<'w, NodeId, NodeId>,
    /// Whether branches on conditions of the values they test alone (see
    /// `BranchKey`) held, where they went the same way on every path here.
    branch_outcomes: FactMap<'w, BranchKey, bool>,
}

impl PartialEq for Facts<'_> {
    fn eq(&self, other: &Self) -> bool {
        let Facts {
            zone: _,
            kinds,
            checked_values,
            elements,
            shapes,
            unvalidated,
            valid_prototype_chains,
            contents,
            unexposed,
            code_valid,
            refinements,
            branch_outcomes,
        } = self;
        *kinds == other.kinds
            && *checked_values == other.checked_values
            && *elements == other.elements
            && *shapes == other.shapes
            && *unvalidated == other.unvalidated
            && *valid_prototype_chains == other.valid_prototype_chains
            && *contents == other.contents
            && *unexposed == other.unexposed
            && *code_valid == other.code_valid
            && *refinements == other.refinements
            && *branch_outcomes == other.branch_outcomes
    }
}

impl<'w> Facts<'w> {
    /// Knowing nothing, with the lists that change in `zone`.
    pub fn new_in(zone: &'w Zone) -> Self {
        Self {
            zone,
            kinds: FactMap::new(),
            checked_values: FactSet::new(),
            elements: FactMap::new(),
            shapes: FactMap::new(),
            unvalidated: FactSet::new(),
            valid_prototype_chains: FactSet::new(),
            contents: FactMap::new(),
            unexposed: FactSet::new(),
            code_valid: false,
            refinements: FactMap::new(),
            branch_outcomes: FactMap::new(),
        }
    }

    /// The same facts, for another point. Both share the lists they have now, which a change of either copies first.
    pub fn share(&mut self) -> Self {
        let Facts {
            zone,
            kinds,
            checked_values,
            elements,
            shapes,
            unvalidated,
            valid_prototype_chains,
            contents,
            unexposed,
            code_valid,
            refinements,
            branch_outcomes,
        } = self;
        Self {
            zone,
            kinds: kinds.share(),
            checked_values: checked_values.share(),
            elements: elements.share(),
            shapes: shapes.share(),
            unvalidated: unvalidated.share(),
            valid_prototype_chains: valid_prototype_chains.share(),
            contents: contents.share(),
            unexposed: unexposed.share(),
            code_valid: *code_valid,
            refinements: refinements.share(),
            branch_outcomes: branch_outcomes.share(),
        }
    }

    /// The same facts, in the zone of `moved`, with lists that are shared where these are.
    pub fn move_to<'n>(&self, moved: &mut Moved<'n>) -> Facts<'n> {
        let Facts {
            zone: _,
            kinds,
            checked_values,
            elements,
            shapes,
            unvalidated,
            valid_prototype_chains,
            contents,
            unexposed,
            code_valid,
            refinements,
            branch_outcomes,
        } = self;
        let shape_lists = &mut moved.shape_lists;
        Facts {
            zone: moved.zone,
            kinds: FactMap(kinds.0.move_copied(&mut moved.kinds)),
            checked_values: FactSet(checked_values.0.move_copied(&mut moved.checked_values)),
            elements: FactMap(elements.0.move_copied(&mut moved.elements)),
            shapes: FactMap(shapes.0.move_to(&mut moved.shapes, |(value, shapes)| {
                (*value, shape_lists.copy(shapes, |shape| *shape))
            })),
            unvalidated: FactSet(unvalidated.0.move_copied(&mut moved.nodes)),
            valid_prototype_chains: FactSet(valid_prototype_chains.0.move_copied(&mut moved.cells)),
            contents: FactMap(contents.0.move_copied(&mut moved.contents)),
            unexposed: FactSet(unexposed.0.move_copied(&mut moved.nodes)),
            code_valid: *code_valid,
            refinements: FactMap(refinements.0.move_copied(&mut moved.refinements)),
            branch_outcomes: FactMap(branch_outcomes.0.move_copied(&mut moved.branch_outcomes)),
        }
    }

    pub fn value_kind(&self, value: NodeId) -> Option<ValueKind> {
        self.kinds.get(&value)?.kind()
    }

    fn kinds(&self, value: NodeId) -> Kinds {
        self.kinds.get(&value).copied().unwrap_or(Kinds::ALL)
    }

    /// The kinds `value` of `graph` can be: those of what unboxed values box
    /// to, and of what boxes of them hold, which the graph tells, and what
    /// the facts know of other values.
    fn kinds_in(&self, graph: &Graph, value: NodeId) -> Kinds {
        let node = graph.node(value);
        match (node.repr, &node.op) {
            (Some(Repr::Int32), _) => Kinds::INT32,
            (Some(Repr::Float64), _) => Kinds::NUMBER,
            (_, Op::BoxInt32) => self.kinds(value).intersect(Kinds::INT32),
            (_, Op::BoxFloat64) => self.kinds(value).intersect(Kinds::NUMBER),
            (_, Op::BoxBool) => self.kinds(value).intersect(Kinds::BOOLEAN),
            _ => self.kinds(value),
        }
    }

    fn narrow_kinds(&mut self, value: NodeId, kinds: Kinds) {
        let known = self.kinds(value);
        let narrowed = known.intersect(kinds);
        if narrowed == known && (narrowed != Kinds::ALL || !self.kinds.contains_key(&value)) {
            return;
        }
        if narrowed == Kinds::ALL {
            self.kinds.remove(self.zone, &value);
        } else {
            self.kinds.insert(self.zone, value, narrowed);
        }
    }

    /// Whether branches on `condition` tell about the kind of the value
    /// they test (see `decides()`).
    pub fn is_about_kinds(condition: BranchCondition) -> bool {
        Kinds::of_condition(condition).is_some()
    }

    /// Whether a branch like the one `key` names held, if it went the same
    /// way on every path here.
    pub fn branch_outcome(&self, key: &BranchKey) -> Option<bool> {
        self.branch_outcomes.get(key).copied()
    }

    /// Records whether a branch like the one `key` names held.
    pub fn set_branch_outcome(&mut self, key: BranchKey, holds: bool) {
        if self.branch_outcome(&key) != Some(holds) {
            self.branch_outcomes.insert(self.zone, key, holds);
        }
    }

    /// Records that `value` is of `kind`, narrowing what was known.
    pub fn set_value_kind(&mut self, value: NodeId, kind: ValueKind) {
        self.narrow_kinds(value, Kinds::of(kind));
    }

    /// Records that the phi `phi` is of a kind one of its inputs is, given
    /// as each input with the facts where it comes from.
    pub fn set_phi_kinds<'a>(
        &mut self,
        graph: &Graph,
        phi: NodeId,
        inputs: impl Iterator<Item = (&'a Facts<'w>, NodeId)>,
    ) where
        'w: 'a,
    {
        let kinds = inputs.fold(Kinds(0), |kinds, (facts, input)| {
            kinds.union(facts.kinds_in(graph, input))
        });
        if kinds != Kinds(0) {
            self.narrow_kinds(phi, kinds);
        }
    }

    pub fn is_object(&self, value: NodeId) -> bool {
        self.value_kind(value) == Some(ValueKind::Object)
    }

    pub fn set_object(&mut self, value: NodeId) {
        self.set_value_kind(value, ValueKind::Object);
    }

    /// Whether `value` passes a branch on `condition` (of `value` alone), if
    /// what is known about it decides that.
    pub fn decides(&self, graph: &Graph, value: NodeId, condition: BranchCondition) -> Option<bool> {
        if let BranchCondition::ElementsKind(kind) = condition
            && let Some(known) = self.elements_kind(value)
        {
            return elements_kind_implies(known, kind);
        }
        let (passing, failing) = Kinds::of_condition(condition)?;
        let kinds = self.kinds_in(graph, value);
        if kinds.intersect(passing) == Kinds(0) {
            return Some(false);
        }
        // NB: Only conditions about the kind alone always pass for it.
        if kinds.intersect(failing) == Kinds(0) {
            return Some(true);
        }
        None
    }

    /// Records what passing a branch on `condition` (of `value` alone) tells
    /// about `value`.
    pub fn set_passed(&mut self, value: NodeId, condition: BranchCondition) {
        if let Some((passing, _)) = Kinds::of_condition(condition) {
            self.narrow_kinds(value, passing);
        }
        if let BranchCondition::ElementsKind(kind) = condition {
            self.set_elements_kind(value, kind);
        }
    }

    /// Records what failing a branch on `condition` (of `value` alone) tells
    /// about `value`: that it is no kind that always passes it.
    pub fn set_failed(&mut self, value: NodeId, condition: BranchCondition) {
        if let Some((_, failing)) = Kinds::of_condition(condition) {
            self.narrow_kinds(value, failing);
        }
    }

    pub fn is_checked_value(&self, value: NodeId, expected: u64) -> bool {
        self.checked_values.contains(&(value, expected))
    }

    pub fn set_checked_value(&mut self, value: NodeId, expected: u64) {
        self.checked_values.insert(self.zone, (value, expected));
    }

    pub fn elements_kind(&self, value: NodeId) -> Option<ElementsKind> {
        self.elements.get(&value).copied()
    }

    pub fn set_elements_kind(&mut self, value: NodeId, kind: ElementsKind) {
        if self.elements_kind(value) != Some(kind) {
            self.elements.insert(self.zone, value, kind);
        }
    }

    pub fn shapes(&self, value: NodeId) -> Option<&'w [ShapeCheck]> {
        self.shapes.get(&value).copied()
    }

    /// Whether what is known about the shapes of `value` holds only if the
    /// code is still valid.
    pub fn is_unvalidated(&self, value: NodeId) -> bool {
        self.unvalidated.contains(&value)
    }

    /// The code is known to be valid: what is known about shapes holds.
    /// Adds the shapes from `stable_shapes` that relies on to `relied`, those
    /// it does not hold yet. (A merge with a path that ran no other code
    /// since it checked them can bring other shapes, which need nothing.)
    pub fn validate(&mut self, stable_shapes: &[CellId], relied: &mut ZoneVec<'_, CellId>) {
        if self.unvalidated.is_empty() {
            return;
        }
        let unvalidated = self.unvalidated.take();
        for value in unvalidated.iter() {
            for check in self.shapes(*value).into_iter().flatten() {
                if stable_shapes.contains(&check.shape) && !relied.contains(&check.shape) {
                    relied.push(check.shape);
                }
            }
        }
    }

    /// Records that `value` has one of `shapes`, narrowing what was known
    /// (unless that holds only if the code is still valid).
    pub fn set_shapes(&mut self, value: NodeId, shapes: &[ShapeCheck]) {
        let unvalidated = self.unvalidated.contains(&value);
        if unvalidated {
            self.unvalidated.remove(self.zone, &value);
        }
        if self.shapes(value) == Some(shapes) {
            return;
        }
        let narrowed: &'w [ShapeCheck] = match self.shapes(value) {
            Some(known) if !unvalidated => self
                .zone
                .alloc_slice_from_iter(shapes.iter().filter(|shape| known.contains(shape)).copied()),
            _ => self.zone.alloc_slice_copy(shapes),
        };
        self.shapes.insert(self.zone, value, narrowed);
    }

    pub fn is_prototype_chain_valid(&self, validity: CellId) -> bool {
        self.valid_prototype_chains.contains(&validity)
    }

    pub fn set_prototype_chain_valid(&mut self, validity: CellId) {
        self.valid_prototype_chains.insert(self.zone, validity);
    }

    pub fn contents(&self, location: Location) -> Option<NodeId> {
        self.contents.get(&location).copied()
    }

    pub fn set_contents(&mut self, location: Location, value: NodeId) {
        if self.contents(location) != Some(value) {
            self.contents.insert(self.zone, location, value);
        }
    }

    /// `object` gets a new shape: the values that may be it, which are those
    /// that may have a shape it may have, may have the new shape too.
    pub fn forget_shapes_of(&mut self, object: NodeId) {
        if self.shapes.is_empty() {
            return;
        }
        match self.shapes(object) {
            Some(known) => {
                self.shapes
                    .retain(self.zone, |_, shapes| !shapes.iter().any(|shape| known.contains(shape)));
            }
            None => self.shapes.clear(),
        }
    }

    /// A store to `offset` of some object, which may alias any object.
    pub fn forget_named_properties_at(&mut self, offset: u32) {
        if !self
            .contents
            .keys()
            .any(|location| matches!(location, Location::Named { offset: known, .. } if *known == offset))
        {
            return;
        }
        self.contents.retain(
            self.zone,
            |location, _| !matches!(location, Location::Named { offset: known, .. } if *known == offset),
        );
    }

    /// A store to binding `index` of some environment, which may be any
    /// environment with such a binding, a global one too.
    pub fn forget_bindings_at(&mut self, index: u32) {
        if !self.contents.keys().any(|location| matches!(location, Location::GlobalBinding { index: known, .. } | Location::Binding { index: known, .. } if *known == index)) {
            return;
        }
        self.contents.retain(self.zone, |location, _| match location {
            Location::GlobalBinding { index: known, .. } | Location::Binding { index: known, .. } => *known != index,
            _ => true,
        });
    }

    pub fn is_unexposed(&self, object: NodeId) -> bool {
        self.unexposed.contains(&object)
    }

    pub fn set_unexposed(&mut self, object: NodeId) {
        self.unexposed.insert(self.zone, object);
    }

    /// Something other than accesses of its properties sees `object`.
    pub fn expose(&mut self, object: NodeId) {
        self.unexposed.remove(self.zone, &object);
    }

    pub fn refinement(&self, value: NodeId) -> Option<NodeId> {
        self.refinements.get(&value).copied()
    }

    pub fn set_refinement(&mut self, value: NodeId, refinement: NodeId) {
        if self.refinement(value) != Some(refinement) {
            self.refinements.insert(self.zone, value, refinement);
        }
    }

    /// A branch establishes facts about `value`, which only the branch's
    /// refinement of it can stand for.
    pub fn forget_refinement(&mut self, value: NodeId) {
        self.refinements.remove(self.zone, &value);
    }

    pub fn is_code_valid(&self) -> bool {
        self.code_valid
    }

    /// Records that the code is known to be valid (see `Op::AssumeValid`).
    pub fn assume_code_valid(&mut self) {
        self.code_valid = true;
    }

    /// A node with `properties` ran: kills the facts about the locations it
    /// may write, except those of objects it cannot see. Objects keep shapes
    /// from `stable_shapes` (unvalidated) while the code stays valid.
    pub fn apply(&mut self, properties: &NodeProperties, stable_shapes: &[CellId]) {
        let writes = properties.writes;
        if properties.runs_code {
            self.code_valid = false;
        }
        if writes.is_empty() {
            return;
        }
        let zone = self.zone;
        let unexposed = &self.unexposed;
        let killed = |location: &Location| {
            location.locations().intersects(writes)
                && !location.object().is_some_and(|object| unexposed.contains(&object))
        };
        if self.contents.keys().any(killed) {
            self.contents.retain(zone, |location, _| !killed(location));
        }
        if writes.intersects(Locations::ELEMENT_COUNTS) && !self.elements.is_empty() {
            self.elements
                .retain(zone, |_, kind| matches!(kind, ElementsKind::TypedArray(_)));
        }
        if writes.intersects(Locations::SHAPES) && !self.shapes.is_empty() {
            let is_stable = |shapes: &[ShapeCheck]| {
                shapes
                    .iter()
                    .all(|check| check.dictionary_generation.is_none() && stable_shapes.contains(&check.shape))
            };
            let mut killed_any = false;
            for (value, shapes) in self.shapes.as_slice() {
                if unexposed.contains(value) {
                    continue;
                }
                if is_stable(shapes) {
                    self.unvalidated.insert(zone, *value);
                } else {
                    killed_any = true;
                }
            }
            if killed_any {
                self.shapes
                    .retain(zone, |value, shapes| unexposed.contains(value) || is_stable(shapes));
            }
        }
        if writes.intersects(Locations::SHAPES) {
            let shapes = &self.shapes;
            if self.unvalidated.iter().any(|value| !shapes.contains_key(value)) {
                self.unvalidated.retain(zone, |value| shapes.contains_key(value));
            }
        }
        if writes.intersects(Locations::SHAPES) && !self.valid_prototype_chains.is_empty() {
            self.valid_prototype_chains.clear();
        }
    }

    /// What holds at a merge of paths where `facts` hold. Facts that know
    /// nothing, in `zone`, if there are none.
    pub fn merge(zone: &'w Zone, mut facts: impl Iterator<Item = Facts<'w>>) -> Facts<'w> {
        let Some(first) = facts.next() else {
            return Facts::new_in(zone);
        };
        let mut merged = first;
        let zone = merged.zone;
        for other in facts {
            narrow_unless_shared(&mut merged.kinds.0, &other.kinds.0, zone, |kinds| {
                kinds.retain_mut(|(value, kinds)| {
                    let Some(other) = other.kinds.get(value) else {
                        return false;
                    };
                    *kinds = kinds.union(*other);
                    true
                });
            });
            narrow_unless_shared(&mut merged.checked_values.0, &other.checked_values.0, zone, |values| {
                values.retain(|value| other.checked_values.contains(value));
            });
            narrow_unless_shared(&mut merged.elements.0, &other.elements.0, zone, |elements| {
                elements.retain(|(value, kind)| other.elements.get(value) == Some(kind));
            });
            narrow_unless_shared(
                &mut merged.valid_prototype_chains.0,
                &other.valid_prototype_chains.0,
                zone,
                |chains| {
                    chains.retain(|cell| other.valid_prototype_chains.contains(cell));
                },
            );
            narrow_unless_shared(&mut merged.contents.0, &other.contents.0, zone, |contents| {
                contents.retain(|(location, value)| other.contents.get(location) == Some(value));
            });
            narrow_unless_shared(&mut merged.unexposed.0, &other.unexposed.0, zone, |unexposed| {
                unexposed.retain(|object| other.unexposed.contains(object));
            });
            if !merged.unvalidated.0.is_same(&other.unvalidated.0) {
                for value in other.unvalidated.iter() {
                    merged.unvalidated.insert(zone, *value);
                }
            }
            merged.code_valid &= other.code_valid;
            narrow_unless_shared(
                &mut merged.branch_outcomes.0,
                &other.branch_outcomes.0,
                zone,
                |outcomes| {
                    outcomes.retain(|(key, holds)| other.branch_outcomes.get(key) == Some(holds));
                },
            );
            narrow_unless_shared(&mut merged.refinements.0, &other.refinements.0, zone, |refinements| {
                refinements.retain(|(key, value)| other.refinements.get(key) == Some(value));
            });
            narrow_unless_shared(&mut merged.shapes.0, &other.shapes.0, zone, |shapes| {
                shapes.retain_mut(|(value, shapes)| {
                    let Some(other_shapes) = other.shapes.get(value) else {
                        return false;
                    };
                    if other_shapes.iter().any(|shape| !shapes.contains(shape)) {
                        let mut united = ZoneVec::from_slice_in(shapes, zone);
                        for shape in *other_shapes {
                            if !united.contains(shape) {
                                united.push(*shape);
                            }
                        }
                        *shapes = united.into_slice();
                    }
                    true
                });
            });
        }
        let shapes = &merged.shapes;
        if merged.unvalidated.iter().any(|value| !shapes.contains_key(value)) {
            merged.unvalidated.retain(zone, |value| shapes.contains_key(value));
        }
        merged
    }
}

/// Merges `other` into the facts `merged` with `merge`, unless both are the
/// same facts, which then hold on both paths as they are.
fn narrow_unless_shared<'w, T: Copy>(
    merged: &mut Part<'w, T>,
    other: &Part<'w, T>,
    zone: &'w Zone,
    merge: impl FnOnce(&mut ZoneVec<'w, T>),
) {
    if !merged.is_same(other) {
        merge(merged.make_mut(zone));
    }
}

/// Whether an object whose elements are known to be of kind `known` has
/// elements of `kind`, if that is known: holey accesses take packed
/// elements too, and holey elements may be packed.
pub(super) fn elements_kind_implies(known: ElementsKind, kind: ElementsKind) -> Option<bool> {
    match (known, kind) {
        _ if known == kind => Some(true),
        (ElementsKind::Packed, ElementsKind::Holey) => Some(true),
        (ElementsKind::Holey, ElementsKind::Packed) => None,
        _ => Some(false),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn shape(id: u64) -> ShapeCheck {
        ShapeCheck {
            shape: CellId::from_address(id),
            dictionary_generation: None,
        }
    }

    fn named(object: NodeId, offset: u32) -> Location {
        Location::Named { object, offset }
    }

    fn facts() -> Facts<'static> {
        Facts::new_in(crate::test_zone())
    }

    /// Where the elements of a part are.
    fn storage<T: Copy>(part: &Part<'_, T>) -> *const T {
        part.as_slice().as_ptr()
    }

    #[test]
    fn merges_intersect_knowledge_and_union_shape_sets() {
        let (a, b, c) = (NodeId(1), NodeId(2), NodeId(3));
        let mut left = facts();
        left.set_object(a);
        left.set_object(b);
        left.set_value_kind(c, ValueKind::Int32);
        left.set_shapes(a, &[shape(10)]);
        left.set_shapes(b, &[shape(20)]);
        left.set_contents(named(a, 0), c);
        let mut right = facts();
        right.set_object(a);
        right.set_value_kind(c, ValueKind::Double);
        right.set_shapes(a, &[shape(11)]);
        right.set_contents(named(a, 0), c);

        let merged = Facts::merge(crate::test_zone(), [left, right].into_iter());
        assert!(merged.is_object(a));
        assert!(!merged.is_object(b));
        assert_eq!(merged.value_kind(c), Some(ValueKind::Number));
        assert_eq!(merged.shapes(a), Some(&[shape(10), shape(11)][..]));
        assert_eq!(merged.shapes(b), None);
        assert_eq!(merged.contents(named(a, 0)), Some(c));
    }

    #[test]
    fn fact_maps_keep_order_through_updates_and_filtering() {
        let zone = crate::test_zone();
        let mut facts = FactMap::new();
        for (key, value) in [(3, 30), (1, 10), (2, 20), (3, 31), (4, 40)] {
            facts.insert(zone, key, value);
        }
        assert_eq!(facts.as_slice(), [(1, 10), (2, 20), (3, 31), (4, 40)]);
        facts.remove(zone, &2);
        facts.remove(zone, &9);
        facts.retain(zone, |key, value| {
            *value += 1;
            *key != 3
        });
        assert_eq!(facts.as_slice(), [(1, 11), (4, 41)]);
        assert_eq!(facts.get(&4), Some(&41));
        assert_eq!(facts.get(&2), None);
        assert_eq!(facts.share(), facts);
    }

    #[test]
    fn unchanged_facts_keep_shared_storage() {
        let value = NodeId(1);
        let mut facts = facts();
        facts.set_object(value);
        facts.set_shapes(value, &[shape(1)]);
        facts.set_contents(named(value, 0), value);
        let shared = facts.share();
        facts.set_object(value);
        facts.set_shapes(value, &[shape(1)]);
        facts.set_contents(named(value, 0), value);
        assert!(facts.kinds.0.is_same(&shared.kinds.0));
        assert!(facts.shapes.0.is_same(&shared.shapes.0));
        assert!(facts.contents.0.is_same(&shared.contents.0));
        // A change copies the part, which is no longer the same.
        facts.set_contents(named(value, 1), value);
        assert!(!facts.contents.0.is_same(&shared.contents.0));
        assert_eq!(shared.contents(named(value, 1)), None);
    }

    #[test]
    fn merging_consumes_unique_storage() {
        let value = NodeId(1);
        let mut first = facts();
        first.set_object(value);
        let first_storage = storage(&first.kinds.0);
        let mut second = facts();
        second.set_object(value);
        let merged = Facts::merge(crate::test_zone(), [first, second].into_iter());
        assert_eq!(storage(&merged.kinds.0), first_storage);
        assert!(merged.is_object(value));
    }

    #[test]
    fn shape_writes_remove_unvalidated_values_without_shapes() {
        let value = NodeId(1);
        let mut facts = facts();
        facts.unvalidated.insert(crate::test_zone(), value);
        let write = NodeProperties {
            writes: Locations::SHAPES,
            ..crate::ir::Op::Unreachable.properties()
        };
        facts.apply(&write, &[]);
        assert!(!facts.is_unvalidated(value));
    }

    #[test]
    fn narrowing_and_killing() {
        let (a, b, count) = (NodeId(1), NodeId(2), NodeId(3));
        let mut facts = facts();
        facts.set_shapes(a, &[shape(1), shape(2)]);
        facts.set_shapes(a, &[shape(2), shape(3)]);
        assert_eq!(facts.shapes(a), Some(&[shape(2)][..]));
        facts.set_contents(named(a, 1), b);
        facts.set_contents(named(b, 2), a);
        facts.forget_named_properties_at(1);
        assert_eq!(facts.contents(named(a, 1)), None);
        assert_eq!(facts.contents(named(b, 2)), Some(a));
        let elements = Location::ElementCount {
            count: ElementCount::Length,
            object: a,
        };
        facts.set_contents(elements, count);
        facts.set_object(a);
        facts.set_elements_kind(a, ElementsKind::Packed);
        // Stores of elements kill no counts, and nothing kills kinds.
        let store = NodeProperties {
            writes: Locations::ELEMENTS,
            ..crate::ir::Op::Unreachable.properties()
        };
        facts.apply(&store, &[]);
        assert_eq!(facts.contents(elements), Some(count));
        assert_eq!(facts.elements_kind(a), Some(ElementsKind::Packed));
        let everything = NodeProperties {
            writes: Locations::ALL,
            runs_code: true,
            ..crate::ir::Op::Unreachable.properties()
        };
        facts.apply(&everything, &[]);
        assert!(facts.is_object(a));
        assert_eq!(facts.shapes(a), None);
        assert_eq!(facts.elements_kind(a), None);
        assert_eq!(facts.contents(named(b, 2)), None);
        assert_eq!(facts.contents(elements), None);
    }

    #[test]
    fn stores_to_a_binding_forget_that_binding_of_every_environment() {
        let (first, second, value) = (NodeId(1), NodeId(2), NodeId(3));
        let binding = |environment, index| Location::Binding { environment, index };
        let global = Location::GlobalBinding {
            environment: CellId::from_address(7),
            index: 0,
        };
        let mut facts = facts();
        facts.set_contents(binding(first, 0), value);
        facts.set_contents(binding(second, 1), value);
        facts.set_contents(global, value);
        facts.forget_bindings_at(0);
        assert_eq!(facts.contents(binding(first, 0)), None);
        assert_eq!(facts.contents(global), None);
        assert_eq!(facts.contents(binding(second, 1)), Some(value));
    }

    #[test]
    fn value_kinds_decide_branches() {
        let mut graph = Graph::default();
        for repr in [
            Repr::Tagged,
            Repr::Tagged,
            Repr::Tagged,
            Repr::Tagged,
            Repr::Tagged,
            Repr::Int32,
            Repr::Tagged,
            Repr::Tagged,
        ] {
            graph.add_node(crate::ir::Node {
                op: crate::ir::Op::LoadSlot { slot: 0 },
                inputs: ak::ZoneVec::new_in(crate::test_zone()),
                repr: Some(repr),
                frame_state: None,
                pc: 0,
            });
        }
        let value = NodeId(1);
        let mut facts = facts();
        assert_eq!(facts.decides(&graph, value, BranchCondition::Object), None);
        facts.set_passed(value, BranchCondition::Int32Value);
        assert_eq!(facts.decides(&graph, value, BranchCondition::Int32Value), Some(true));
        assert_eq!(facts.decides(&graph, value, BranchCondition::Double), Some(false));
        assert_eq!(facts.decides(&graph, value, BranchCondition::Object), Some(false));
        let number = NodeId(3);
        facts.set_value_kind(number, ValueKind::Number);
        assert_eq!(facts.decides(&graph, number, BranchCondition::Int32Value), None);
        // A number that is no int32 is a double.
        facts.set_failed(number, BranchCondition::Int32Value);
        assert_eq!(facts.value_kind(number), Some(ValueKind::Double));
        assert_eq!(facts.decides(&graph, number, BranchCondition::Double), Some(true));
        // What fails a test fails it again.
        let other = NodeId(4);
        facts.set_failed(other, BranchCondition::String);
        assert_eq!(facts.decides(&graph, other, BranchCondition::String), Some(false));
        assert_eq!(facts.value_kind(other), None);
        // Values whose condition depends on more than their kind are not
        // known to pass it.
        facts.set_passed(other, BranchCondition::NonNegativeInt32);
        assert_eq!(facts.decides(&graph, other, BranchCondition::NonNegativeInt32), None);
        // Unboxed values are of the kind of what they box to.
        assert_eq!(
            facts.decides(&graph, NodeId(5), BranchCondition::Int32Value),
            Some(true)
        );
        assert_eq!(
            facts.decides(&graph, NodeId(5), BranchCondition::Int32OrBoolean),
            Some(true)
        );
        // An int32 or a boolean is neither nullish nor an object, and a value
        // that is neither is no int32.
        let integer_or_boolean = NodeId(6);
        facts.set_passed(integer_or_boolean, BranchCondition::Int32OrBoolean);
        assert_eq!(
            facts.decides(&graph, integer_or_boolean, BranchCondition::Nullish),
            Some(false)
        );
        assert_eq!(
            facts.decides(&graph, integer_or_boolean, BranchCondition::Object),
            Some(false)
        );
        assert_eq!(
            facts.decides(&graph, integer_or_boolean, BranchCondition::Int32Value),
            None
        );
        let neither = NodeId(7);
        facts.set_failed(neither, BranchCondition::Int32OrBoolean);
        assert_eq!(facts.decides(&graph, neither, BranchCondition::Int32Value), Some(false));
        let object = NodeId(2);
        facts.set_passed(object, BranchCondition::ElementsKind(ElementsKind::Packed));
        assert!(facts.is_object(object));
        assert_eq!(
            facts.decides(&graph, object, BranchCondition::ElementsKind(ElementsKind::Holey)),
            Some(true)
        );
    }
}
