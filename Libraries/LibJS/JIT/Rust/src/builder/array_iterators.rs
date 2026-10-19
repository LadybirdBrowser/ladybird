/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Iteration over arrays, built as IR: `GetIterator` iterates arrays in fast
//! array mode, without an iterator, and `IteratorNextUnpack` steps that mode
//! and array iterators like `%ArrayIteratorPrototype%.next` does. The slow
//! paths of the instructions handle everything else.

use super::Flow;
use super::GraphBuilder;
use super::checks::SlowPaths;
use crate::CompileFailure;
use crate::bytecode::Operand;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::BranchCondition;
use crate::ir::ElementsKind;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;

/// The `hint` of a `GetIterator` for synchronous iteration.
const SYNC_HINT: u32 = 0;

impl GraphBuilder<'_> {
    /// The builtin `%ArrayIteratorPrototype%.next` of the realm of the
    /// compiled code, if `%ArrayIteratorPrototype%` held it when the
    /// snapshot was taken.
    fn array_iterator_next(&mut self) -> Option<NodeId> {
        let next = self.runtime.array_iterator_next?;
        self.embed(next);
        let bits = self.boxed_object(next);
        Some(self.constant(bits))
    }

    /// Builds `GetIterator` (`Handling::Expanded`) of a synchronous iterator
    /// in fast array mode (see `Op::OpenArrayIteration`), where the
    /// instruction allows it: the index 0, the array, and not done.
    ///
    /// Arrays of the realm are iterated in that mode while they inherit
    /// `%Array.prototype.values%` as their `@@iterator` and the realm's
    /// array iterators have the builtin `next` method. The realm holds a
    /// valid watchpoint set for each while it knows that they do, which the
    /// code checks inline, like that the array has no named properties, of
    /// which an own `@@iterator` would be one. Where that fails, the code
    /// asks the runtime, which watches the intrinsics anew where unrelated
    /// changes fired the sets. Returns `None`, having built nothing, where
    /// the instruction does not allow fast array mode, and where the realm
    /// held no valid set for one when the snapshot was taken: it never holds
    /// one again then.
    pub(super) fn try_build_get_iterator(
        &mut self,
        dst_iterator_object: Operand,
        dst_iterator_next: Operand,
        dst_iterator_done: Operand,
        iterable: Operand,
        hint: u32,
        fast_array_mode_allowed: bool,
    ) -> Result<Option<Flow>, CompileFailure> {
        if hint != SYNC_HINT || !fast_array_mode_allowed {
            return Ok(None);
        }
        let (Some(next_watchpoint_set_address), Some(iterator_method_watchpoint_set_address)) = (
            self.runtime.array_iterator_next_watchpoint_set_address,
            self.runtime.array_iterator_method_watchpoint_set_address,
        ) else {
            return Ok(None);
        };
        let iterable = self.read(iterable)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let first_index = self.constant(value::int32(0));
        let not_done = self.constant(value::FALSE);
        self.embed(self.runtime.realm);
        for watchpoint_set_address in [next_watchpoint_set_address, iterator_method_watchpoint_set_address] {
            let watchpoint_set_address = self.typed_constant(watchpoint_set_address, Repr::Pointer);
            self.branch_to_next_case(
                &mut slow_paths,
                BranchCondition::HoldsValidWatchpointSet,
                vec![watchpoint_set_address],
                true,
            );
        }
        let array = self.branch_to_next_case(&mut slow_paths, BranchCondition::Object, vec![iterable], true);
        let address = self.cell_address(array);
        self.embed(self.runtime.array_prototype);
        self.branch_to_next_case(
            &mut slow_paths,
            BranchCondition::ArrayWithRealmArrayPrototypeAndNoNamedProperties,
            vec![address],
            true,
        );
        self.end_hot_path(&mut slow_paths, vec![first_index, array, not_done]);

        let started = self.start_next_case(&mut slow_paths);
        debug_assert!(started, "values that fail the checks go on to the next case");
        let array = self.emit(Op::OpenArrayIteration, vec![iterable], Some(Repr::Tagged));
        let array = self.branch_if_empty(&mut slow_paths, array);
        let outputs = self.join_paths(slow_paths, vec![first_index, array, not_done], |_, outputs| outputs);
        self.write(dst_iterator_object, outputs[0])?;
        self.write(dst_iterator_next, outputs[1])?;
        self.write(dst_iterator_done, outputs[2])?;
        Ok(Some(Flow::Continue))
    }

    /// Builds `IteratorNextUnpack` (`Handling::Expanded`) for arrays with
    /// packed or holey elements, stepped like `%ArrayIteratorPrototype%.next`
    /// steps array iterators over their values: the element at the index,
    /// which moves on to the next one, or done once the index reaches the
    /// length of the array. Holes take the slow path, which looks them up in
    /// the prototype chain.
    ///
    /// In fast array mode (see `get_iterator()` in the runtime), the iterator
    /// operand holds the index, an int32 until the slow path makes it a
    /// double, the next operand holds the array, and the done operand tells
    /// when the iteration is done. Otherwise, array
    /// iterators whose next method is the builtin one hold the array and the
    /// index, if the realm's array iterators had that method when the
    /// snapshot was taken.
    pub(super) fn try_build_iterator_next_unpack(
        &mut self,
        dst_value: Operand,
        dst_done: Operand,
        iterator_object: Operand,
        iterator_next: Operand,
        iterator_done: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        let iterator = self.read(iterator_object)?;
        let next = self.read(iterator_next)?;
        let iterator_is_done = self.read(iterator_done)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let (undefined, done, not_done) = (
            self.constant(value::UNDEFINED),
            self.constant(value::TRUE),
            self.constant(value::FALSE),
        );
        let kind = ElementsKind::Holey;

        // Fast array mode. NB: Iterations that are done take the slow path.
        let index = self.branch_to_next_case(&mut slow_paths, BranchCondition::NonNegativeInt32, vec![iterator], true);
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::TaggedEquals { equal: true },
            vec![iterator_is_done, not_done],
            true,
        );
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, vec![next], true);
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::ElementsKind(kind), vec![array], true);
        let array = self.cell_address(array);
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::MagicalLength, vec![array], true);
        let index = self.emit(Op::UnboxInt32, vec![index], Some(Repr::Int32));
        let length = self.emit(Op::LoadElementsLength, vec![array], Some(Repr::Int32));
        let (inside, outside) = self.branch_both_ways(BranchCondition::IndexInBounds, vec![index, length]);

        // NB: The done operand keeps the iteration done, also if the array
        //     grows.
        self.block = outside;
        self.end_hot_path(&mut slow_paths, vec![undefined, done, iterator, done]);

        self.block = inside;
        let index = self.refine(BranchCondition::IndexInBounds, true, vec![index, length]);
        let element = self.load_array_iteration_element(&mut slow_paths, kind, array, index);
        // NB: The index is below the capacity, so the next one is an int32 too.
        let one = self.int32_constant(1);
        let next_index = self.emit_checked(
            Op::Int32Binary { op: BinaryOp::Add },
            vec![index, one],
            Some(Repr::Int32),
        );
        let next_index = self.emit(Op::BoxInt32, vec![next_index], Some(Repr::Tagged));
        self.end_hot_path(&mut slow_paths, vec![element, not_done, next_index, iterator_is_done]);

        // Array iterators. NB: Without them, the other values take the slow
        //     path.
        let Some(builtin_next) = self.array_iterator_next() else {
            let outputs = self.join_hot_paths_with(slow_paths, |_, outputs| outputs);
            self.write_iterator_next_unpack_outputs(&outputs, dst_value, dst_done, iterator_object, iterator_done)?;
            return Ok(Some(Flow::Continue));
        };
        let started = self.start_next_case(&mut slow_paths);
        debug_assert!(started, "values other than indices go on to the next case");
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::TaggedEquals { equal: true },
            vec![next, builtin_next],
            true,
        );
        let object = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, vec![iterator], true);
        let object = self.cell_address(object);
        let object = self.branch_to_slow_path(&mut slow_paths, BranchCondition::ArrayIterator, vec![object], true);

        // NB: Done iterators no longer have an array.
        let array = self.emit(Op::LoadArrayIteratorArray, vec![object], Some(Repr::Tagged));
        let (exhausted, live) = self.branch_both_ways(BranchCondition::Undefined, vec![array]);
        self.block = exhausted;
        self.end_hot_path(&mut slow_paths, vec![undefined, done, iterator, done]);

        self.block = live;
        let array = self.refine(BranchCondition::Undefined, false, vec![array]);
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::ElementsKind(kind), vec![array], true);
        let array = self.cell_address(array);
        // NB: Array iterators iterate array-likes up to their `length`,
        //     which only arrays keep as the size of their elements.
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::MagicalLength, vec![array], true);
        let index = self.emit(Op::LoadArrayIteratorIndex, vec![object], Some(Repr::Int32));
        let length = self.emit(Op::LoadElementsLength, vec![array], Some(Repr::Int32));
        let (inside, outside) = self.branch_both_ways(BranchCondition::IndexInBounds, vec![index, length]);

        // NB: The array has no element at the index, so the iterator is done.
        self.block = outside;
        self.emit(Op::FinishArrayIterator, vec![object], None);
        self.end_hot_path(&mut slow_paths, vec![undefined, done, iterator, done]);

        self.block = inside;
        let index = self.refine(BranchCondition::IndexInBounds, true, vec![index, length]);
        let element = self.load_array_iteration_element(&mut slow_paths, kind, array, index);
        let one = self.int32_constant(1);
        let next_index = self.emit_checked(
            Op::Int32Binary { op: BinaryOp::Add },
            vec![index, one],
            Some(Repr::Int32),
        );
        self.emit(Op::StoreArrayIteratorIndex, vec![object, next_index], None);
        let outputs = self.join_paths(
            slow_paths,
            vec![element, not_done, iterator, iterator_is_done],
            |_, outputs| outputs,
        );
        self.write_iterator_next_unpack_outputs(&outputs, dst_value, dst_done, iterator_object, iterator_done)?;
        Ok(Some(Flow::Continue))
    }

    /// The element of `array` at `index`, which is below its length: holes
    /// take the slow path.
    fn load_array_iteration_element(
        &mut self,
        slow_paths: &mut SlowPaths,
        kind: ElementsKind,
        array: NodeId,
        index: NodeId,
    ) -> NodeId {
        // NB: Holey elements below the length may be past the capacity.
        //     Elements are accessed through the array (see
        //     `element_storage_and_bound()`).
        let capacity = self.emit(Op::LoadElementsCapacity, vec![array], Some(Repr::Int32));
        let index = self.branch_to_slow_path(slow_paths, BranchCondition::IndexInBounds, vec![index, capacity], true);
        let element = self.load_element(kind, array, index);
        self.branch_if_empty(slow_paths, element)
    }

    /// Writes the outputs of `IteratorNextUnpack`, in the order of the
    /// outputs of its slow path.
    fn write_iterator_next_unpack_outputs(
        &mut self,
        outputs: &[NodeId],
        dst_value: Operand,
        dst_done: Operand,
        iterator_object: Operand,
        iterator_done: Operand,
    ) -> Result<(), CompileFailure> {
        self.write(dst_value, outputs[0])?;
        self.write(dst_done, outputs[1])?;
        self.write(iterator_object, outputs[2])?;
        self.write(iterator_done, outputs[3])?;
        Ok(())
    }
}
