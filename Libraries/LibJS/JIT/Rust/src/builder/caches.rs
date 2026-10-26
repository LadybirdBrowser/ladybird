/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The interpreter's property lookup caches, as compiled code sees them.
//!
//! Every cache entry the interpreter fills is a guard and an action: it
//! applies to objects of one shape (for keyed accesses, also with one key),
//! while the prototype chain it relies on is unchanged, and then reads,
//! writes or adds a property at a known place. `CacheEntry` is that form,
//! and the graph builder speculates from it alone: the entries a site saw
//! become a shape check (or a shape switch, for entries with different
//! actions) and the actions, as IR.
//!
//! The caches have a fixed format, so the form is small and closed, and the
//! interpreter, the runtime's cache probes and this translation all read the
//! same entries.

use super::GraphBuilder;
use super::SlotState;
use crate::bytecode::Operand;
use crate::ir::AccessorPart;
use crate::ir::Block;
use crate::ir::BranchCondition;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::snapshot::AccessorFunctionSnapshot;
use crate::snapshot::CellId;
use crate::snapshot::Intrinsic;
use crate::snapshot::PropertyCacheEntrySnapshot;
use crate::snapshot::PropertyCacheEntryType;
use crate::snapshot::PropertyCacheKind;

/// Where the property an entry describes is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Holder {
    /// In the object the access is on.
    Receiver,
    /// In `prototype`, while the `WatchpointSet` cell `validity` is
    /// valid (and it was valid when the snapshot was taken if `valid`).
    Prototype {
        prototype: CellId,
        validity: CellId,
        valid: bool,
    },
}

/// What an access does where an entry applies.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Action {
    /// Reads the property at `offset` of the holder.
    Get { holder: Holder, offset: u32 },
    /// Writes the existing property at `offset` of the holder.
    Set { holder: Holder, offset: u32 },
    /// Adds the property at `offset` to the object, which gets `shape`, while
    /// nothing in the prototype chain (checked with the validity cell, if
    /// there is one) would see the addition.
    Add {
        offset: u32,
        shape: CellId,
        prototype_chain: Option<(CellId, bool)>,
    },
}

/// One cache entry: its guards and its action.
#[derive(Debug, Clone, Copy)]
pub(super) struct CacheEntry {
    /// The shape the object must have.
    pub shape: ShapeCheck,
    /// Whether the shape was stable (see `Dependency::StableShape`).
    pub stable: bool,
    /// For keyed accesses, the key the access must have: the encoded key
    /// value and its cell.
    pub key: Option<(u64, CellId)>,
    pub action: Action,
    /// Whether the property held an accessor (see
    /// `PropertyCacheEntrySnapshot::holds_accessor`): reads and writes of
    /// it as data would exit.
    pub holds_accessor: bool,
    /// Whether a `Set` writes a data property the object has.
    pub writes_data_property: bool,
    /// Whether the property holds data while its holder keeps its shape (see
    /// `PropertyCacheEntrySnapshot::property_holds_data`).
    pub property_holds_data: bool,
    /// The function of the accessor the property held, which an access may
    /// call inline.
    pub accessor_function: Option<AccessorFunctionSnapshot>,
    /// For `Get`s from a prototype: the object the property held at compile
    /// time, if it held one, and which intrinsic that is.
    pub prototype_property: Option<(CellId, Option<Intrinsic>)>,
    /// The snapshot executable of the function `prototype_property` holds,
    /// if calls of it may be inlined.
    pub prototype_property_inline_executable: Option<u32>,
    /// The target through which code may call the function
    /// `prototype_property` holds directly.
    pub prototype_property_direct_call: Option<crate::snapshot::DirectCallTarget>,
    /// The watchpoint set that stays valid while the prototype holds
    /// `prototype_property`, if there is one.
    pub prototype_property_watchpoint: Option<CellId>,
}

impl CacheEntry {
    /// The entry in its guard and action form, if compiled code can apply
    /// it: entries of missing properties, and entries without a shape, are
    /// left to the interpreter's code.
    pub fn of(entry: &PropertyCacheEntrySnapshot) -> Option<CacheEntry> {
        let prototype_holder = || match (entry.prototype, entry.prototype_chain_validity) {
            (Some(prototype), Some(validity)) => Some(Holder::Prototype {
                prototype,
                validity,
                valid: entry.prototype_chain_valid,
            }),
            _ => None,
        };
        let (shape, action) = match entry.entry_type {
            PropertyCacheEntryType::GetOwnProperty if entry.prototype.is_none() => (
                entry.shape?,
                Action::Get {
                    holder: Holder::Receiver,
                    offset: entry.property_offset,
                },
            ),
            PropertyCacheEntryType::GetPropertyInPrototypeChain => (
                entry.shape?,
                Action::Get {
                    holder: prototype_holder()?,
                    offset: entry.property_offset,
                },
            ),
            PropertyCacheEntryType::ChangeOwnProperty if entry.prototype.is_none() => (
                entry.shape?,
                Action::Set {
                    holder: Holder::Receiver,
                    offset: entry.property_offset,
                },
            ),
            PropertyCacheEntryType::ChangePropertyInPrototypeChain => (
                entry.shape?,
                Action::Set {
                    holder: prototype_holder()?,
                    offset: entry.property_offset,
                },
            ),
            PropertyCacheEntryType::AddOwnProperty => (
                entry.shape?,
                Action::Add {
                    offset: entry.property_offset,
                    shape: entry.new_shape?,
                    prototype_chain: entry
                        .prototype_chain_validity
                        .map(|validity| (validity, entry.prototype_chain_valid)),
                },
            ),
            _ => return None,
        };
        Some(CacheEntry {
            shape: ShapeCheck {
                shape,
                dictionary_generation: entry.shape_is_dictionary.then_some(entry.shape_dictionary_generation),
            },
            // NB: Additions take objects off the shape.
            stable: entry.shape_is_stable && !matches!(action, Action::Add { .. }),
            key: entry.key.map(|cell| (entry.key_value, cell)),
            action,
            holds_accessor: entry.holds_accessor,
            writes_data_property: entry.writes_data_property,
            property_holds_data: entry.property_holds_data,
            accessor_function: entry.accessor_function,
            prototype_property: entry
                .prototype_property
                .map(|cell| (cell, entry.prototype_property_intrinsic)),
            prototype_property_inline_executable: entry.prototype_property_inline_executable,
            prototype_property_direct_call: entry.prototype_property_direct_call,
            prototype_property_watchpoint: entry.prototype_property_watchpoint,
        })
    }

    /// Whether the entry reads or writes a data property in place.
    pub fn is_data_access(&self) -> bool {
        match self.action {
            Action::Get { .. } => !self.holds_accessor,
            Action::Set {
                holder: Holder::Receiver,
                ..
            } => self.writes_data_property && !self.holds_accessor,
            _ => false,
        }
    }

    /// Where the accessor whose `part` the entry calls is, if compiled code
    /// may call it there: in the receiver (no prototype) or in a prototype,
    /// at an offset. Setters are only called in prototypes.
    pub fn accessor_location(&self, part: AccessorPart) -> Option<(Option<CellId>, u32)> {
        let prototype = |holder: Holder| match holder {
            Holder::Receiver => None,
            Holder::Prototype { prototype, .. } => Some(prototype),
        };
        match (self.action, part) {
            (Action::Get { holder, offset }, AccessorPart::Getter) => Some((prototype(holder), offset)),
            (
                Action::Set {
                    holder: Holder::Prototype { prototype, .. },
                    offset,
                },
                AccessorPart::Setter,
            ) => Some((Some(prototype), offset)),
            _ => None,
        }
    }

    /// Whether the entry calls the `part` of an accessor of a function the
    /// compiler may inline.
    pub fn calls_accessor(&self, part: AccessorPart) -> bool {
        self.accessor_location(part).is_some()
            && self
                .accessor_function
                .is_some_and(|function| function.inline_executable.is_some())
    }
}

impl GraphBuilder<'_> {
    /// The entries of property lookup cache `cache` in their guard and
    /// action form, if the cache is monomorphic or polymorphic and compiled
    /// code can apply every entry.
    pub(super) fn cache_entries(&self, cache: u32) -> Option<Vec<CacheEntry>> {
        let cache = self.function.executable.property_caches.get(cache as usize)?;
        if !matches!(
            cache.kind,
            PropertyCacheKind::Monomorphic | PropertyCacheKind::Polymorphic
        ) || cache.entries.is_empty()
        {
            return None;
        }
        cache.entries.iter().map(CacheEntry::of).collect()
    }

    /// The entry of megamorphic property lookup cache `cache` for the shape
    /// the code knows the object in `object` has (see
    /// `AbstractFrame::fresh_shapes`), if the snapshot captured one.
    pub(super) fn megamorphic_entries_for_known_shape(&self, cache: u32, object: Operand) -> Option<Vec<CacheEntry>> {
        let cache = self.function.executable.property_caches.get(cache as usize)?;
        if cache.kind != PropertyCacheKind::Megamorphic {
            return None;
        }
        let SlotState::Value { node, .. } = self.peek(object) else {
            return None;
        };
        let shape = self.fresh_shape(node)?;
        let entry = cache
            .entries
            .iter()
            .filter_map(CacheEntry::of)
            .find(|entry| entry.shape.shape == shape)?;
        Some(vec![entry])
    }

    /// The shape guard of an entry. Records whether the shape was stable,
    /// which check elimination may rely on.
    pub(super) fn shape_guard(&mut self, entry: &CacheEntry) -> ShapeCheck {
        if entry.stable && !self.graph.stable_shapes.contains(&entry.shape.shape) {
            self.graph.stable_shapes.push(entry.shape.shape);
        }
        entry.shape
    }

    /// Applies the shape guards of `entries` to `object`, then the action
    /// `apply` builds for each: one shape check and one action where all
    /// entries have the same action, or a shape switch with one case per
    /// entry. Continues after them, and returns the phi of what `apply`
    /// returned, if it returned values.
    pub(super) fn translate_entries(
        &mut self,
        object: NodeId,
        entries: &[CacheEntry],
        apply: impl FnMut(&mut Self, &CacheEntry, NodeId) -> Option<NodeId>,
    ) -> Option<NodeId> {
        let entries = self.entries_for_known_shape(object, entries);
        let shapes = entries.iter().map(|entry| self.shape_guard(entry)).collect::<Vec<_>>();
        // NB: The action applies the first entry's, so entries that differ in whether the property holds data do not
        //     share one.
        let one_action = entries.iter().all(|entry| {
            entry.action == entries[0].action && entry.property_holds_data == entries[0].property_holds_data
        });
        self.dispatch_on_shapes(object, entries, &shapes, one_action, apply)
    }

    /// The entries that apply to `object`: only the one for its shape, where
    /// the code allocated it and knows the shape it has now (see
    /// `AbstractFrame::fresh_shapes`), which needs no shape switch. Its shape
    /// is still checked, which check elimination removes where it can tell.
    pub(super) fn entries_for_known_shape<'e>(&self, object: NodeId, entries: &'e [CacheEntry]) -> &'e [CacheEntry] {
        let Some(shape) = self.fresh_shape(object) else {
            return entries;
        };
        entries
            .iter()
            .position(|entry| entry.shape.shape == shape)
            .map_or(entries, |index| &entries[index..=index])
    }

    /// Checks that `object` has one of `shapes` and applies the action of
    /// the first entry if `one_action`, or switches on the shapes to the
    /// action of each entry otherwise. The actions get the object refined
    /// by the check.
    pub(super) fn dispatch_on_shapes(
        &mut self,
        object: NodeId,
        entries: &[CacheEntry],
        shapes: &[ShapeCheck],
        one_action: bool,
        mut apply: impl FnMut(&mut Self, &CacheEntry, NodeId) -> Option<NodeId>,
    ) -> Option<NodeId> {
        if one_action {
            let object = self.check_shapes(object, shapes);
            return apply(self, &entries[0], object);
        }
        self.shape_switch_on(object, shapes, |builder, index, object| {
            apply(builder, &entries[index], object)
        })
    }

    /// Branches on the shape of `object` to one block per shape in `shapes`,
    /// runs `case` with its index and the object refined to have the shape
    /// in each, and continues in a block joining them. Returns the phi of
    /// what `case` returned, if it returned values.
    pub(super) fn shape_switch_on(
        &mut self,
        object: NodeId,
        shapes: &[ShapeCheck],
        mut case: impl FnMut(&mut Self, usize, NodeId) -> Option<NodeId>,
    ) -> Option<NodeId> {
        let source = self.block;
        let frame = self.frame.clone();
        let frame_state = self.eager_frame_state();
        let mut cases = Vec::with_capacity(shapes.len());
        for shape in shapes {
            let block = self.graph.add_block(Block {
                predecessors: vec![source],
                ..Block::default()
            });
            self.order.push(block);
            self.embed(shape.shape);
            cases.push((*shape, block));
        }
        let address = self.cell_address(object);
        let switch = self.set_control(Op::ShapeSwitch { cases: cases.clone() }, vec![object, address]);
        self.graph.nodes[switch.index()].frame_state = Some(frame_state);

        let join = self.graph.add_block(Block::default());
        let mut results = Vec::new();
        let mut case_shapes = Vec::with_capacity(cases.len());
        for (index, (shape, block)) in cases.iter().enumerate() {
            self.block = *block;
            self.frame = frame.clone();
            let refined = self.refine(BranchCondition::Shape(*shape), true, vec![object, address]);
            results.push(case(self, index, refined));
            debug_assert_eq!(self.frame.slots, frame.slots, "cases do not change frame slots");
            self.set_control(Op::Jump { target: join }, Vec::new());
            self.graph.blocks[join.index()].predecessors.push(self.block);
            case_shapes.push(std::mem::take(&mut self.frame.fresh_shapes));
        }
        self.order.push(join);
        self.block = join;
        self.frame = frame;
        // NB: Cases may change shapes, like additions do.
        self.frame.fresh_shapes = super::AbstractFrame::common_fresh_shapes(case_shapes.iter().map(Vec::as_slice));

        let results = results.into_iter().collect::<Option<Vec<_>>>()?;
        if results.iter().all(|result| *result == results[0]) {
            return Some(results[0]);
        }
        Some(self.add_phi(results))
    }
}
