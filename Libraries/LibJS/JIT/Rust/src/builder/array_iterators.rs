/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Iteration over arrays, built as IR: `GetIterator` makes array iterators
//! without looking up and calling the iterator method of the array, and
//! `IteratorNextUnpack` steps them like `%ArrayIteratorPrototype%.next`
//! does. The slow paths of the instructions handle everything else.

use super::Flow;
use super::GraphBuilder;
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
    /// as a new array iterator for arrays (see `Op::CreateArrayIterator`),
    /// whose next method is the builtin one and which is not done yet.
    /// Returns `None`, having built nothing, if the realm's array iterators
    /// had another next method.
    pub(super) fn try_build_get_iterator(
        &mut self,
        dst_iterator_object: Operand,
        dst_iterator_next: Operand,
        dst_iterator_done: Operand,
        iterable: Operand,
        hint: u32,
    ) -> Result<Option<Flow>, CompileFailure> {
        if hint != SYNC_HINT {
            return Ok(None);
        }
        let Some(next) = self.array_iterator_next() else {
            return Ok(None);
        };
        let iterable = self.read(iterable)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let iterator = self.emit(Op::CreateArrayIterator, vec![iterable, next], Some(Repr::Tagged));
        let iterator = self.branch_if_empty(&mut slow_paths, iterator);
        let not_done = self.constant(value::FALSE);
        let outputs = self.join_paths(slow_paths, vec![iterator, next, not_done], |_, outputs| outputs);
        self.write(dst_iterator_object, outputs[0])?;
        self.write(dst_iterator_next, outputs[1])?;
        self.write(dst_iterator_done, outputs[2])?;
        Ok(Some(Flow::Continue))
    }

    /// Builds `IteratorNextUnpack` (`Handling::Expanded`) for array
    /// iterators over the values of arrays with packed or holey elements,
    /// whose next method is the builtin one: the element at the iterator's
    /// index, which moves on to the next one, or done once the index reaches
    /// the length of the array. Holes take the slow path, which looks them
    /// up in the prototype chain. Returns `None`, having built nothing, if
    /// the realm's array iterators had another next method.
    pub(super) fn try_build_iterator_next_unpack(
        &mut self,
        dst_value: Operand,
        dst_done: Operand,
        iterator_object: Operand,
        iterator_next: Operand,
        iterator_done: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some(builtin_next) = self.array_iterator_next() else {
            return Ok(None);
        };
        let iterator = self.read(iterator_object)?;
        let next = self.read(iterator_next)?;
        let iterator_is_done = self.read(iterator_done)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::TaggedEquals { equal: true },
            vec![next, builtin_next],
            true,
        );
        let iterator = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, vec![iterator], true);
        let iterator = self.cell_address(iterator);
        let iterator = self.branch_to_slow_path(&mut slow_paths, BranchCondition::ArrayIterator, vec![iterator], true);
        let (undefined, done, not_done) = (
            self.constant(value::UNDEFINED),
            self.constant(value::TRUE),
            self.constant(value::FALSE),
        );

        // NB: Done iterators no longer have an array.
        let array = self.emit(Op::LoadArrayIteratorArray, vec![iterator], Some(Repr::Tagged));
        let (exhausted, live) = self.branch_both_ways(BranchCondition::Undefined, vec![array]);
        self.block = exhausted;
        self.end_hot_path(&mut slow_paths, vec![undefined, done, done]);

        self.block = live;
        let array = self.refine(BranchCondition::Undefined, false, vec![array]);
        let kind = ElementsKind::Holey;
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::ElementsKind(kind), vec![array], true);
        let array = self.cell_address(array);
        // NB: Array iterators iterate array-likes up to their `length`,
        //     which only arrays keep as the size of their elements.
        let array = self.branch_to_slow_path(&mut slow_paths, BranchCondition::MagicalLength, vec![array], true);
        let index = self.emit(Op::LoadArrayIteratorIndex, vec![iterator], Some(Repr::Int32));
        let length = self.emit(Op::LoadElementsLength, vec![array], Some(Repr::Int32));
        let (inside, outside) = self.branch_both_ways(BranchCondition::IndexInBounds, vec![index, length]);

        // NB: The array has no element at the index, so the iterator is done.
        self.block = outside;
        self.emit(Op::FinishArrayIterator, vec![iterator], None);
        self.end_hot_path(&mut slow_paths, vec![undefined, done, done]);

        self.block = inside;
        let index = self.refine(BranchCondition::IndexInBounds, true, vec![index, length]);
        // NB: Holey elements below the length may be past the capacity.
        //     Elements are accessed through the array (see
        //     `element_storage_and_bound()`).
        let capacity = self.emit(Op::LoadElementsCapacity, vec![array], Some(Repr::Int32));
        let index = self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::IndexInBounds,
            vec![index, capacity],
            true,
        );
        let element = self.load_element(kind, array, index);
        let element = self.branch_if_empty(&mut slow_paths, element);
        let one = self.int32_constant(1);
        let next_index = self.emit_checked(
            Op::Int32Binary { op: BinaryOp::Add },
            vec![index, one],
            Some(Repr::Int32),
        );
        self.emit(Op::StoreArrayIteratorIndex, vec![iterator, next_index], None);
        let outputs = self.join_paths(slow_paths, vec![element, not_done, iterator_is_done], |_, outputs| {
            outputs
        });
        self.write(dst_value, outputs[0])?;
        self.write(dst_done, outputs[1])?;
        self.write(iterator_done, outputs[2])?;
        Ok(Some(Flow::Continue))
    }
}
