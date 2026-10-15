/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Named property accesses that speculation does not cover, built as IR on
//! the probes of the interpreter's property lookup caches: `GetById` and
//! `PutById` probe the cache of the instruction, `GetLength` branches on
//! arrays, other objects (through the cache) and strings, and
//! `IteratorClose` probes the cache for the return method of the iterator.
//! What the probes do not handle, like accessors, takes the instruction's
//! slow path.

use super::Flow;
use super::GraphBuilder;
use super::checks::SlowPaths;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::code::Dependency;
use crate::code::Repr;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::NodeId;
use crate::ir::Op;

impl GraphBuilder<'_> {
    /// Builds `GetById` (`Handling::Expanded`): the property the cache has
    /// for the value (see `Op::ProbePropertyCache`).
    pub(super) fn build_get_by_id(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let Instruction::GetById { dst, base, cache, .. } = *instruction else {
            unreachable!("only GetById is built here");
        };
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let value = self.read(base)?;
        let property = self.probe_property_cache(&mut slow_paths, value, cache);
        let result = self.join_slow_paths(slow_paths, Some(property));
        self.write(dst, result.expect("GetById has a value"))?;
        Ok(Flow::Continue)
    }

    /// Builds `PutById` (`Handling::Expanded`): stores and additions the
    /// cache has for objects.
    pub(super) fn build_put_by_id(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let Instruction::PutById { base, src, cache, .. } = *instruction else {
            unreachable!("only PutById is built here");
        };
        let mut slow_paths = self.start_slow_paths(None)?;
        let object = self.read(base)?;
        let value = self.read(src)?;
        let executable = self.function.index;
        let stored = self.emit(
            Op::ProbePropertyStore { executable, cache },
            vec![object, value],
            Some(Repr::Bool),
        );
        self.branch_to_slow_path(&mut slow_paths, BranchCondition::Bool, vec![stored], true);
        self.join_slow_paths(slow_paths, None);
        Ok(Flow::Continue)
    }

    /// Builds `IteratorClose` (`Handling::Expanded`): an iterator without a
    /// return method closes without doing anything, for every completion
    /// type, since the instruction after a close for a throw completion
    /// throws it again.
    pub(super) fn build_iterator_close(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let Instruction::IteratorClose {
            iterator_object, cache, ..
        } = *instruction
        else {
            unreachable!("only IteratorClose is built here");
        };
        let mut slow_paths = self.start_slow_paths(None)?;
        let iterator = self.read(iterator_object)?;

        // NB: In fast array mode, the iterator operand holds the index of the
        //     next element (see `try_build_get_iterator()`), and a watchpoint
        //     set of the realm tells if array iterators have a return method.
        if let Some(return_watchpoint_set) = self.runtime.array_iterator_return_watchpoint_set {
            let (fast_array_mode, iterator_object) =
                self.branch_both_ways(BranchCondition::NonNegativeInt32, vec![iterator]);
            self.block = fast_array_mode;
            self.embed(return_watchpoint_set);
            self.assume(Dependency::WatchpointSetValid(return_watchpoint_set));
            self.end_hot_path(&mut slow_paths, Vec::new());
            self.block = iterator_object;
        }

        let return_method = self.probe_property_cache(&mut slow_paths, iterator, cache);
        self.branch_to_slow_path(&mut slow_paths, BranchCondition::Nullish, vec![return_method], true);
        self.join_slow_paths(slow_paths, None);
        Ok(Flow::Continue)
    }

    /// The property that cache `cache` of the instruction has for the value
    /// `object`, taking a slow path where it has none.
    fn probe_property_cache(&mut self, slow_paths: &mut SlowPaths, object: NodeId, cache: u32) -> NodeId {
        // NB: The probe looks up the properties of strings, numbers and
        //     booleans in their prototypes, whose addresses it embeds.
        let snapshot = &self.function.executable;
        let prototypes = [
            snapshot.string_prototype,
            snapshot.number_prototype,
            snapshot.boolean_prototype,
        ];
        for prototype in prototypes.into_iter().flatten() {
            self.embed(prototype);
        }
        let executable = self.function.index;
        let property = self.emit(
            Op::ProbePropertyCache { executable, cache },
            vec![object],
            Some(Repr::Tagged),
        );
        self.branch_if_empty(slow_paths, property)
    }

    /// Builds `GetLength` (`Handling::Expanded`): the length of objects with
    /// a magical length (like arrays), the property the cache has for other
    /// objects, and the length of strings.
    pub(super) fn build_get_length(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let Instruction::GetLength { dst, base, cache, .. } = *instruction else {
            unreachable!("only GetLength is built here");
        };
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let value = self.read(base)?;
        let conditions = [BranchCondition::Object, BranchCondition::String];
        self.branch_on_cases(
            &mut slow_paths,
            value,
            &conditions,
            false,
            |builder, slow_paths, case, value| {
                if case == 0 {
                    let address = builder.cell_address(value);
                    let (magical, named) = builder.branch_both_ways(BranchCondition::MagicalLength, vec![address]);
                    builder.block = magical;
                    let array = builder.refine(BranchCondition::MagicalLength, true, vec![address]);
                    let length = builder.emit(Op::LoadElementsLength, vec![array], Some(Repr::Int32));
                    builder.end_with_length(slow_paths, length);
                    builder.block = named;
                    let length = builder.probe_property_cache(slow_paths, value, cache);
                    builder.end_hot_path(slow_paths, vec![length]);
                } else {
                    let address = builder.emit(Op::StringAddress, vec![value], Some(Repr::Pointer));
                    let length = builder.emit(Op::StringLength, vec![address], Some(Repr::Int32));
                    builder.end_with_length(slow_paths, length);
                }
            },
        );
        let result = self.join_hot_paths(slow_paths);
        self.write(dst, result.expect("GetLength has a value"))?;
        Ok(Flow::Continue)
    }

    /// Ends a path of `GetLength` with the `Repr::Int32` `length`, which
    /// takes a slow path where it is no int32, an unsigned length above
    /// `i32::MAX`.
    fn end_with_length(&mut self, slow_paths: &mut SlowPaths, length: NodeId) {
        let zero = self.int32_constant(0);
        let length = self.branch_to_slow_path(
            slow_paths,
            BranchCondition::Int32(Comparison::GreaterThanEquals),
            vec![length, zero],
            true,
        );
        let length = self.emit(Op::BoxInt32, vec![length], Some(Repr::Tagged));
        self.end_hot_path(slow_paths, vec![length]);
    }
}
