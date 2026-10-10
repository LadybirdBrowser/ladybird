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
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::rc::Rc;

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
    /// Every other kind of value: `undefined`, `null`, booleans, symbols
    /// and big integers.
    const OTHER: Self = Self(1 << 4);
    const ALL: Self = Self((1 << 5) - 1);
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
            Location::ElementCount { .. } => Locations::ELEMENT_COUNTS,
        }
    }

    /// The object the location belongs to, if it is one's.
    fn object(self) -> Option<NodeId> {
        match self {
            Location::Named { object, .. } | Location::ElementCount { object, .. } => Some(object),
            Location::GlobalBinding { .. } | Location::Binding { .. } => None,
        }
    }
}

/// What is known at one point of the program.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(super) struct Facts {
    /// The kinds values may be (immutable), where that is not every kind.
    kinds: Rc<BTreeMap<NodeId, Kinds>>,
    /// Values known to have passed a check against a constant, with that
    /// constant (immutable).
    checked_values: Rc<BTreeSet<(NodeId, u64)>>,
    /// Objects known to have elements of a kind (killed by writes of element
    /// counts, except for typed arrays, which keep their kind).
    elements: Rc<BTreeMap<NodeId, ElementsKind>>,
    /// For objects, the shapes they may have (killed by writes of shapes,
    /// except for stable shapes, which are dependency facts).
    shapes: Rc<BTreeMap<NodeId, Vec<ShapeCheck>>>,
    /// Objects whose shapes are known from before other code ran, which
    /// still hold only if the code is still valid (see `Op::AssumeValid`).
    unvalidated: Rc<BTreeSet<NodeId>>,
    /// Prototype chains known to be valid (killed by writes of shapes).
    valid_prototype_chains: Rc<BTreeSet<CellId>>,
    /// The known contents of locations, killed by writes of their
    /// locations (see `Location::locations()`).
    contents: Rc<BTreeMap<Location, NodeId>>,
    /// Objects compiled code allocated that nothing but accesses of their
    /// properties has seen yet, so nothing else that runs can change their
    /// shapes or properties.
    unexposed: Rc<BTreeSet<NodeId>>,
    /// Whether the code is known to be valid (see `Op::AssumeValid`): no
    /// other code ran since an `AssumeValid`.
    code_valid: bool,
    /// For values, the latest node that refines them (see `Op::Refine`) on
    /// every path here, or the value itself where it is what it is known to
    /// be by its definition: what replaces a check that is known to pass,
    /// so that the nodes using it stay behind the check that passed.
    refinements: Rc<BTreeMap<NodeId, NodeId>>,
}

impl Facts {
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
            _ => self.kinds(value),
        }
    }

    fn narrow_kinds(&mut self, value: NodeId, kinds: Kinds) {
        let narrowed = self.kinds(value).intersect(kinds);
        if narrowed == Kinds::ALL {
            Rc::make_mut(&mut self.kinds).remove(&value);
        } else {
            Rc::make_mut(&mut self.kinds).insert(value, narrowed);
        }
    }

    /// Whether branches on `condition` tell about the kind of the value
    /// they test (see `decides()`).
    pub fn is_about_kinds(condition: BranchCondition) -> bool {
        Kinds::of_condition(condition).is_some()
    }

    /// Records that `value` is of `kind`, narrowing what was known.
    pub fn set_value_kind(&mut self, value: NodeId, kind: ValueKind) {
        self.narrow_kinds(value, Kinds::of(kind));
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
        Rc::make_mut(&mut self.checked_values).insert((value, expected));
    }

    pub fn elements_kind(&self, value: NodeId) -> Option<ElementsKind> {
        self.elements.get(&value).copied()
    }

    pub fn set_elements_kind(&mut self, value: NodeId, kind: ElementsKind) {
        Rc::make_mut(&mut self.elements).insert(value, kind);
    }

    pub fn shapes(&self, value: NodeId) -> Option<&[ShapeCheck]> {
        self.shapes.get(&value).map(Vec::as_slice)
    }

    /// Whether what is known about the shapes of `value` holds only if the
    /// code is still valid.
    pub fn is_unvalidated(&self, value: NodeId) -> bool {
        self.unvalidated.contains(&value)
    }

    /// The code is known to be valid: what is known about shapes holds.
    /// Returns the shapes from `stable_shapes` that relies on. (A merge with
    /// a path that ran no other code since it checked them can bring other
    /// shapes, which need nothing.)
    pub fn validate(&mut self, stable_shapes: &[CellId]) -> Vec<CellId> {
        if self.unvalidated.is_empty() {
            return Vec::new();
        }
        let mut shapes = Vec::new();
        for value in std::mem::take(Rc::make_mut(&mut self.unvalidated)) {
            for check in self.shapes.get(&value).into_iter().flatten() {
                if stable_shapes.contains(&check.shape) && !shapes.contains(&check.shape) {
                    shapes.push(check.shape);
                }
            }
        }
        shapes
    }

    /// Records that `value` has one of `shapes`, narrowing what was known
    /// (unless that holds only if the code is still valid).
    pub fn set_shapes(&mut self, value: NodeId, shapes: &[ShapeCheck]) {
        let unvalidated = self.unvalidated.contains(&value);
        if unvalidated {
            Rc::make_mut(&mut self.unvalidated).remove(&value);
        }
        let narrowed = match self.shapes.get(&value) {
            Some(known) if !unvalidated => shapes.iter().filter(|shape| known.contains(shape)).copied().collect(),
            _ => shapes.to_vec(),
        };
        Rc::make_mut(&mut self.shapes).insert(value, narrowed);
    }

    pub fn is_prototype_chain_valid(&self, validity: CellId) -> bool {
        self.valid_prototype_chains.contains(&validity)
    }

    pub fn set_prototype_chain_valid(&mut self, validity: CellId) {
        Rc::make_mut(&mut self.valid_prototype_chains).insert(validity);
    }

    pub fn contents(&self, location: Location) -> Option<NodeId> {
        self.contents.get(&location).copied()
    }

    pub fn set_contents(&mut self, location: Location, value: NodeId) {
        Rc::make_mut(&mut self.contents).insert(location, value);
    }

    /// `object` gets a new shape: the values that may be it, which are those
    /// that may have a shape it may have, may have the new shape too.
    pub fn forget_shapes_of(&mut self, object: NodeId) {
        match self.shapes.get(&object).cloned() {
            Some(known) => {
                Rc::make_mut(&mut self.shapes).retain(|_, shapes| !shapes.iter().any(|shape| known.contains(shape)));
            }
            None => Rc::make_mut(&mut self.shapes).clear(),
        }
    }

    /// A store to `offset` of some object, which may alias any object.
    pub fn forget_named_properties_at(&mut self, offset: u32) {
        Rc::make_mut(&mut self.contents)
            .retain(|location, _| !matches!(location, Location::Named { offset: known, .. } if *known == offset));
    }

    /// A store to binding `index` of some environment, which may be any
    /// environment with such a binding, a global one too.
    pub fn forget_bindings_at(&mut self, index: u32) {
        Rc::make_mut(&mut self.contents).retain(|location, _| match location {
            Location::GlobalBinding { index: known, .. } | Location::Binding { index: known, .. } => *known != index,
            _ => true,
        });
    }

    pub fn is_unexposed(&self, object: NodeId) -> bool {
        self.unexposed.contains(&object)
    }

    pub fn set_unexposed(&mut self, object: NodeId) {
        Rc::make_mut(&mut self.unexposed).insert(object);
    }

    /// Something other than accesses of its properties sees `object`.
    pub fn expose(&mut self, object: NodeId) {
        if self.unexposed.contains(&object) {
            Rc::make_mut(&mut self.unexposed).remove(&object);
        }
    }

    pub fn refinement(&self, value: NodeId) -> Option<NodeId> {
        self.refinements.get(&value).copied()
    }

    pub fn set_refinement(&mut self, value: NodeId, refinement: NodeId) {
        Rc::make_mut(&mut self.refinements).insert(value, refinement);
    }

    /// A branch establishes facts about `value`, which only the branch's
    /// refinement of it can stand for.
    pub fn forget_refinement(&mut self, value: NodeId) {
        if self.refinements.contains_key(&value) {
            Rc::make_mut(&mut self.refinements).remove(&value);
        }
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
        let unexposed = &self.unexposed;
        let killed = |location: &Location| {
            location.locations().intersects(writes)
                && !location.object().is_some_and(|object| unexposed.contains(&object))
        };
        if self.contents.keys().any(killed) {
            Rc::make_mut(&mut self.contents).retain(|location, _| !killed(location));
        }
        if writes.intersects(Locations::ELEMENT_COUNTS) {
            Rc::make_mut(&mut self.elements).retain(|_, kind| matches!(kind, ElementsKind::TypedArray(_)));
        }
        if writes.intersects(Locations::SHAPES) {
            let mut kept_stable = Vec::new();
            Rc::make_mut(&mut self.shapes).retain(|value, shapes| {
                if unexposed.contains(value) {
                    return true;
                }
                let stable = shapes
                    .iter()
                    .all(|check| check.dictionary_generation.is_none() && stable_shapes.contains(&check.shape));
                if stable {
                    kept_stable.push(*value);
                }
                stable
            });
            Rc::make_mut(&mut self.unvalidated).extend(kept_stable);
            let shapes = &self.shapes;
            Rc::make_mut(&mut self.unvalidated).retain(|value| shapes.contains_key(value));
            Rc::make_mut(&mut self.valid_prototype_chains).clear();
        }
    }

    /// What holds at a merge of paths where `facts` hold.
    pub fn merge<'a>(mut facts: impl Iterator<Item = &'a Facts>) -> Facts {
        let Some(first) = facts.next() else {
            return Facts::default();
        };
        let mut merged = first.clone();
        for other in facts {
            narrow_unless_shared(&mut merged.kinds, &other.kinds, |kinds| {
                kinds.retain(|value, kinds| {
                    let Some(other) = other.kinds.get(value) else {
                        return false;
                    };
                    *kinds = kinds.union(*other);
                    true
                });
            });
            narrow_unless_shared(&mut merged.checked_values, &other.checked_values, |values| {
                values.retain(|value| other.checked_values.contains(value));
            });
            narrow_unless_shared(&mut merged.elements, &other.elements, |elements| {
                elements.retain(|value, kind| other.elements.get(value) == Some(kind));
            });
            narrow_unless_shared(
                &mut merged.valid_prototype_chains,
                &other.valid_prototype_chains,
                |chains| {
                    chains.retain(|cell| other.valid_prototype_chains.contains(cell));
                },
            );
            narrow_unless_shared(&mut merged.contents, &other.contents, |contents| {
                contents.retain(|location, value| other.contents.get(location) == Some(value));
            });
            narrow_unless_shared(&mut merged.unexposed, &other.unexposed, |unexposed| {
                unexposed.retain(|object| other.unexposed.contains(object));
            });
            narrow_unless_shared(&mut merged.unvalidated, &other.unvalidated, |unvalidated| {
                unvalidated.extend(other.unvalidated.iter().copied());
            });
            merged.code_valid &= other.code_valid;
            narrow_unless_shared(&mut merged.refinements, &other.refinements, |refinements| {
                refinements.retain(|key, value| other.refinements.get(key) == Some(value));
            });
            narrow_unless_shared(&mut merged.shapes, &other.shapes, |shapes| {
                shapes.retain(|value, shapes| {
                    let Some(other_shapes) = other.shapes.get(value) else {
                        return false;
                    };
                    for shape in other_shapes {
                        if !shapes.contains(shape) {
                            shapes.push(*shape);
                        }
                    }
                    true
                });
            });
        }
        let shapes = &merged.shapes;
        if merged.unvalidated.iter().any(|value| !shapes.contains_key(value)) {
            Rc::make_mut(&mut merged.unvalidated).retain(|value| shapes.contains_key(value));
        }
        merged
    }
}

/// Merges `other` into the facts `merged` with `merge`, unless both are the
/// same facts, which then hold on both paths as they are.
fn narrow_unless_shared<T: Clone>(merged: &mut Rc<T>, other: &Rc<T>, merge: impl FnOnce(&mut T)) {
    if !Rc::ptr_eq(merged, other) {
        merge(Rc::make_mut(merged));
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
            shape: CellId(id),
            dictionary_generation: None,
        }
    }

    fn named(object: NodeId, offset: u32) -> Location {
        Location::Named { object, offset }
    }

    #[test]
    fn merges_intersect_knowledge_and_union_shape_sets() {
        let (a, b, c) = (NodeId(1), NodeId(2), NodeId(3));
        let mut left = Facts::default();
        left.set_object(a);
        left.set_object(b);
        left.set_value_kind(c, ValueKind::Int32);
        left.set_shapes(a, &[shape(10)]);
        left.set_shapes(b, &[shape(20)]);
        left.set_contents(named(a, 0), c);
        let mut right = Facts::default();
        right.set_object(a);
        right.set_value_kind(c, ValueKind::Double);
        right.set_shapes(a, &[shape(11)]);
        right.set_contents(named(a, 0), c);

        let merged = Facts::merge([&left, &right].into_iter());
        assert!(merged.is_object(a));
        assert!(!merged.is_object(b));
        assert_eq!(merged.value_kind(c), Some(ValueKind::Number));
        assert_eq!(merged.shapes(a), Some(&[shape(10), shape(11)][..]));
        assert_eq!(merged.shapes(b), None);
        assert_eq!(merged.contents(named(a, 0)), Some(c));
    }

    #[test]
    fn narrowing_and_killing() {
        let (a, b, count) = (NodeId(1), NodeId(2), NodeId(3));
        let mut facts = Facts::default();
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
            environment: CellId(7),
            index: 0,
        };
        let mut facts = Facts::default();
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
        ] {
            graph.add_node(crate::ir::Node {
                op: crate::ir::Op::LoadSlot { slot: 0 },
                inputs: Vec::new(),
                repr: Some(repr),
                frame_state: None,
                pc: 0,
            });
        }
        let value = NodeId(1);
        let mut facts = Facts::default();
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
        let object = NodeId(2);
        facts.set_passed(object, BranchCondition::ElementsKind(ElementsKind::Packed));
        assert!(facts.is_object(object));
        assert_eq!(
            facts.decides(&graph, object, BranchCondition::ElementsKind(ElementsKind::Holey)),
            Some(true)
        );
    }
}
