/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The sites of compiled code (see `artifact::SiteView`): where the translator finds the values of a node's frame
//! state, from the register allocation.

use ak::Zone;
use ak::ZoneVec;

use crate::CompileFailure;
use crate::artifact::ArtifactWriter;
use crate::artifact::Frame;
use crate::asm::Gpr;
use crate::asm::relocation::EmbeddedCells;
use crate::code::Repr;
use crate::code::SiteKind;
use crate::code::ValueLocation;
use crate::code::VirtualObjectDescriptorKind;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::VirtualObjectKind;
use crate::regalloc::Allocation;
use crate::regalloc::FPR_BASE;
use crate::regalloc::Location;
use crate::snapshot::CellId;

/// In place of the record of a frame state: it has none.
const NO_RECORD: u32 = u32::MAX;

/// What the sites of a compile are written with, in the work zone.
pub(crate) struct SiteScratch<'a> {
    /// The record of each frame state of the graph in the tables of the code, by the index of the frame state.
    frame_records: &'a mut [u32],
    /// The frame states of a chain that have no record.
    pending: ZoneVec<'a, u32>,
    /// The values of a frame at a site, as the slot in the high half and the word of the location in the low half.
    values: ZoneVec<'a, u64>,
}

impl<'a> SiteScratch<'a> {
    // NB: A compile whose graph has more frame states, or a frame state with more values or callers, fails.
    const MAX_FRAME_STATES: usize = 1 << 24;
    const MAX_FRAME_VALUES: usize = 1 << 22;
    const MAX_FRAME_DEPTH: usize = 1 << 16;

    /// What the sites of the code of `graph` are written with, which knows no records yet.
    pub(crate) fn new_in(graph: &Graph, work: &'a Zone) -> Result<Self, CompileFailure> {
        if graph.frame_states.len() > Self::MAX_FRAME_STATES {
            return Err(CompileFailure::TooLarge);
        }
        Ok(Self {
            frame_records: work.alloc_slice_fill(graph.frame_states.len(), NO_RECORD),
            pending: ZoneVec::new_in(work),
            values: ZoneVec::new_in(work),
        })
    }

    /// The record of `frame_state`, which gets one with its callers if it has none.
    fn frame_record(
        &mut self,
        graph: &Graph,
        frame_state: FrameStateId,
        artifact: &mut ArtifactWriter<'_>,
    ) -> Result<u32, CompileFailure> {
        self.pending.clear();
        let mut parent = None;
        let mut next = Some(frame_state);
        while let Some(id) = next {
            let record = self.frame_records[id.index()];
            if record != NO_RECORD {
                parent = Some(record);
                break;
            }
            if self.pending.len() == Self::MAX_FRAME_DEPTH {
                return Err(CompileFailure::TooLarge);
            }
            self.pending.push(id.0);
            next = graph.frame_state(id).parent;
        }
        // NB: The record of a frame names that of its caller, so callers come first.
        for index in (0..self.pending.len()).rev() {
            let id = FrameStateId(self.pending[index]);
            let state = graph.frame_state(id);
            let frame = Frame {
                executable: state.executable,
                pc: state.pc,
                mode: state.mode,
                passed_argument_count: state.passed_argument_count,
                legacy_arguments: state.legacy_arguments.clone(),
                parent,
            };
            let record = artifact.add_frame(&frame, state.values.iter().map(|(slot, _)| *slot), &state.in_frame)?;
            self.frame_records[id.index()] = record;
            parent = Some(record);
        }
        Ok(parent.expect("a chain has a frame state"))
    }
}

/// Where a site of kind `kind` finds the values of the frame state of `node`, one after the other in the order of
/// `Graph::frame_state_values()`.
struct Locations<'a> {
    graph: &'a Graph<'a>,
    allocation: &'a Allocation<'a>,
    kind: SiteKind,
    /// See `Graph::virtual_objects()`.
    objects: &'a [NodeId],
    exit_values: core::slice::Iter<'a, (u32, Location)>,
    /// Converts spill slots to frame offsets.
    stack_slot_offset: &'a dyn Fn(u32) -> i32,
    /// Registers that are in the saves area at the site, with the frame pointer offsets of their words there.
    saves: &'a [(Gpr, i32)],
    embedded: &'a EmbeddedCells<'a>,
}

impl Locations<'_> {
    /// The slot and the location of `value`, the next value. A constant that is a cell the code does not embed fails
    /// the compile.
    fn next(&mut self, value: NodeId) -> Result<(u32, ValueLocation), CompileFailure> {
        let (slot, location) = *self.exit_values.next().expect("every frame state value has a location");
        if let Some(index) = self.objects.iter().position(|object| *object == value) {
            return Ok((
                slot,
                ValueLocation::VirtualObject(u32::try_from(index).expect("objects fit in u32")),
            ));
        }
        let repr = self
            .graph
            .node(value)
            .repr
            .expect("frame state values have a representation");
        let location = match location {
            // NB: Stack walks and the runtime read the values of inlined call
            //     sites from the JIT frame while the call runs, without the
            //     registers. Values live across calls have stack slots.
            Location::Register(_) if self.kind == SiteKind::Call => match self.allocation.node(value).spill_slot {
                Some(slot) => ValueLocation::Stack((self.stack_slot_offset)(slot), repr),
                None => ValueLocation::Frame(
                    crate::regalloc::frame_home(self.graph, value).expect("values live across calls are spilled"),
                ),
            },
            // NB: Exits read float64 values from the floating point registers.
            Location::Register(register) => ValueLocation::Register(register % FPR_BASE, repr),
            Location::Stack(slot) => ValueLocation::Stack((self.stack_slot_offset)(slot), repr),
            Location::Frame(slot) => ValueLocation::Frame(slot),
            Location::Constant(bits, _) => match crate::ir::value::virtual_arguments_kind(bits) {
                Some(mapped) if repr == Repr::Tagged => ValueLocation::ArgumentsObject { mapped },
                _ => self.constant_location(crate::ir::value::boxed_constant(bits, repr), repr)?,
            },
        };
        // NB: Float64 values are in floating point registers, which are not saved.
        if let ValueLocation::Register(register, repr) = location
            && repr != Repr::Float64
            && let Some((_, offset)) = self.saves.iter().find(|(saved, _)| saved.0 == register)
        {
            return Ok((slot, ValueLocation::Stack(*offset, repr)));
        }
        Ok((slot, location))
    }

    /// Where the tables find the boxed constant `bits` of a value of `repr`: by its index if it is a cell, which its
    /// repr says (a pointer that is not null, or a value whose tag says so).
    fn constant_location(&self, bits: u64, repr: Repr) -> Result<ValueLocation, CompileFailure> {
        let encoding = self.embedded.encoding();
        let (cell, tag) = match repr {
            Repr::Pointer if bits != 0 => (CellId::from_address(bits), 0),
            Repr::Pointer => return Ok(ValueLocation::Constant(0)),
            _ => match encoding.cell_of_value(bits) {
                Some(cell) => (cell, encoding.tag_of_value(bits)),
                None => return Ok(ValueLocation::Constant(bits)),
            },
        };
        let cell = self.embedded.index_of(cell).ok_or(CompileFailure::CodeGeneration)?;
        Ok(ValueLocation::Cell { cell, tag })
    }
}

/// What a site is written from.
pub(crate) struct SiteSource<'a> {
    pub graph: &'a Graph<'a>,
    pub allocation: &'a Allocation<'a>,
    /// The node whose frame state the site has.
    pub node: NodeId,
    pub kind: SiteKind,
    /// Converts spill slots to frame offsets.
    pub stack_slot_offset: &'a dyn Fn(u32) -> i32,
    /// Registers that are in the saves area at the site and not where they were allocated, with the frame pointer
    /// offsets of their words there.
    pub saves: &'a [(Gpr, i32)],
    /// Where the site keeps the lists it makes while it is written.
    pub work: &'a Zone,
    pub embedded: &'a EmbeddedCells<'a>,
}

impl SiteSource<'_> {
    /// Adds the site to the tables of the code, as the next site.
    pub(crate) fn write(
        &self,
        scratch: &mut SiteScratch<'_>,
        artifact: &mut ArtifactWriter<'_>,
    ) -> Result<(), CompileFailure> {
        let graph = self.graph;
        let frame_state = graph.node(self.node).frame_state.expect("the node has a frame state");
        let frame = scratch.frame_record(graph, frame_state, artifact)?;
        let objects = graph.virtual_objects(frame_state, self.work);
        if !objects.is_empty() && !artifact.frame_has_objects(frame) {
            let mut descriptors = ZoneVec::new_in(self.work);
            for object in &objects {
                let Op::VirtualObject { kind, allocation } = graph.node(*object).op else {
                    unreachable!("virtual objects are VirtualObject nodes");
                };
                let kind = match kind {
                    VirtualObjectKind::Object { shape } => VirtualObjectDescriptorKind::Object {
                        shape: self.embedded.index_of(shape).ok_or(CompileFailure::CodeGeneration)?,
                    },
                    VirtualObjectKind::Closure { sample_slot } => VirtualObjectDescriptorKind::Closure { sample_slot },
                };
                let property_count = u32::try_from(graph.node(*object).inputs.len()).expect("inputs fit in u32");
                descriptors.push((kind, allocation.0, property_count));
            }
            artifact.set_frame_objects(frame, descriptors.into_iter())?;
        }
        let mut locations = self.locations(&objects);
        let start = artifact.location_count();
        let mut next = Some(frame_state);
        while let Some(id) = next {
            let state = graph.frame_state(id);
            scratch.values.clear();
            for (_, value) in &state.values {
                let (slot, location) = locations.next(*value)?;
                let word = artifact.location(location)?;
                if scratch.values.len() == SiteScratch::MAX_FRAME_VALUES {
                    return Err(CompileFailure::TooLarge);
                }
                scratch.values.push(u64::from(slot) << 32 | u64::from(word));
            }
            // NB: The locations of a frame are in the order of its slots, which are in the high halves.
            scratch.values.sort_unstable();
            for value in &*scratch.values {
                artifact.add_location(*value as u32)?;
            }
            next = state.parent;
        }
        // NB: The properties of the virtual objects follow the frames' values.
        for object in &objects {
            for property in &graph.node(*object).inputs {
                let (_, location) = locations.next(*property)?;
                let word = artifact.location(location)?;
                artifact.add_location(word)?;
            }
        }
        let inlined_frame_bytes = match graph.node(self.node).op {
            Op::CallDirect {
                inlined_frame_bytes, ..
            }
            | Op::CallNative {
                inlined_frame_bytes, ..
            } if self.kind == SiteKind::Call => inlined_frame_bytes,
            _ => 0,
        };
        artifact.add_site(self.kind, frame, start, inlined_frame_bytes)?;
        Ok(())
    }

    /// Makes `values` the slots of the values of the compiled function's frame at the site and where the values are,
    /// in slot order.
    pub(crate) fn root_frame_values(
        &self,
        values: &mut ZoneVec<'_, (u32, ValueLocation)>,
    ) -> Result<(), CompileFailure> {
        let frame_state = self
            .graph
            .node(self.node)
            .frame_state
            .expect("the node has a frame state");
        let objects = self.graph.virtual_objects(frame_state, self.work);
        let mut locations = self.locations(&objects);
        values.clear();
        let mut next = Some(frame_state);
        while let Some(id) = next {
            let state = self.graph.frame_state(id);
            values.clear();
            for (_, value) in &state.values {
                values.push(locations.next(*value)?);
            }
            next = state.parent;
        }
        values.sort_unstable_by_key(|(slot, _)| *slot);
        Ok(())
    }

    fn locations<'a>(&'a self, objects: &'a [NodeId]) -> Locations<'a> {
        Locations {
            graph: self.graph,
            allocation: self.allocation,
            kind: self.kind,
            objects,
            exit_values: self.allocation.node(self.node).exit_values.iter(),
            stack_slot_offset: self.stack_slot_offset,
            saves: self.saves,
            embedded: self.embedded,
        }
    }
}
