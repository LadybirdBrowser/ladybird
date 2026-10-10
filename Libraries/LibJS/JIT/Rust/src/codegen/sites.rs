/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The sites of compiled code (see `code::Site`): where the translator finds
//! the values of a node's frame state, from the register allocation.

use crate::code::FrameState as CodeFrameState;
use crate::code::Repr;
use crate::code::Site;
use crate::code::SiteKind;
use crate::code::ValueLocation;
use crate::code::VirtualObjectDescriptor;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::regalloc::Allocation;
use crate::regalloc::FPR_BASE;
use crate::regalloc::Location;

/// Builds the site of kind `kind` for the frame state of `node`, converting
/// spill slots to frame offsets with `stack_slot_offset`.
pub(crate) fn site(
    graph: &Graph,
    allocation: &Allocation,
    node: NodeId,
    kind: SiteKind,
    stack_slot_offset: &dyn Fn(u32) -> i32,
) -> Site {
    let frame_state = graph.node(node).frame_state.expect("the node has a frame state");
    let chain = graph.frame_state_chain(frame_state);
    let objects = graph.virtual_objects(frame_state);
    let mut exit_values = allocation.node(node).exit_values.iter();
    let mut location_of = |value: NodeId| {
        let (slot, location) = *exit_values.next().expect("every frame state value has a location");
        if let Some(index) = objects.iter().position(|object| *object == value) {
            return (
                slot,
                ValueLocation::VirtualObject(u32::try_from(index).expect("objects fit in u32")),
            );
        }
        let repr = graph
            .node(value)
            .repr
            .expect("frame state values have a representation");
        let location = match location {
            // NB: Stack walks and the runtime read the values of inlined call
            //     sites from the JIT frame while the call runs, without the
            //     registers. Values live across calls have stack slots.
            Location::Register(_) if kind == SiteKind::Call => {
                let slot = allocation
                    .node(value)
                    .spill_slot
                    .expect("values live across calls are spilled");
                ValueLocation::Stack(stack_slot_offset(slot), repr)
            }
            // NB: Exits read float64 values from the floating point registers.
            Location::Register(register) => ValueLocation::Register(register % FPR_BASE, repr),
            Location::Stack(slot) => ValueLocation::Stack(stack_slot_offset(slot), repr),
            Location::Constant(bits) => match crate::ir::value::virtual_arguments_kind(bits) {
                Some(mapped) if repr == Repr::Tagged => ValueLocation::ArgumentsObject { mapped },
                _ => ValueLocation::Constant(crate::ir::value::boxed_constant(bits, repr)),
            },
        };
        (slot, location)
    };
    let frames = chain
        .into_iter()
        .map(|frame_state| {
            let frame_state = graph.frame_state(frame_state);
            let values = frame_state
                .values
                .iter()
                .map(|(_, value)| location_of(*value))
                .collect();
            CodeFrameState {
                executable: frame_state.executable,
                pc: frame_state.pc,
                mode: frame_state.mode,
                values,
                in_frame: frame_state.in_frame.clone(),
                passed_argument_count: frame_state.passed_argument_count,
            }
        })
        .collect();
    // NB: The properties of the virtual objects follow the frames' values.
    let objects = objects
        .iter()
        .map(|object| {
            let Op::VirtualObject { shape } = graph.node(*object).op else {
                unreachable!("virtual objects are VirtualObject nodes");
            };
            let properties = graph
                .node(*object)
                .inputs
                .iter()
                .map(|property| location_of(*property).1)
                .collect();
            VirtualObjectDescriptor { shape, properties }
        })
        .collect();
    let inlined_frame_bytes = match graph.node(node).op {
        Op::CallDirect {
            inlined_frame_bytes, ..
        }
        | Op::CallNative {
            inlined_frame_bytes, ..
        } if kind == SiteKind::Call => inlined_frame_bytes,
        _ => 0,
    };
    Site {
        kind,
        frames,
        objects,
        inlined_frame_bytes,
    }
}
