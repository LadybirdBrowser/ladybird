/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The next key of a for-in loop, built as IR.

use super::Flow;
use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::Operand;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Op;
use crate::ir::value;

impl GraphBuilder<'_> {
    /// Builds `ObjectPropertyIteratorNext`, the next key of a for-in loop
    /// over `receiver`: while the property iterator cache `keys` still holds
    /// the receiver's keys and the cursor is an int32, the key at the cursor,
    /// or done once there are no more. The instruction's slow path, in cold
    /// blocks, handles everything else.
    pub(super) fn build_object_property_iterator_next(
        &mut self,
        dst_value: Operand,
        dst_done: Operand,
        receiver: Operand,
        keys: Operand,
        cursor: Operand,
    ) -> Result<Flow, CompileFailure> {
        let receiver = self.read(receiver)?;
        let keys = self.read(keys)?;
        let cursor_value = self.read(cursor)?;
        // NB: Once there are no more keys, the value stays what it was.
        let old_value = self.read(dst_value)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let receiver = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, vec![receiver], true);
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::PropertyIteratorCacheValid,
            vec![receiver, keys],
            true,
        );
        let cursor_index = self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::NonNegativeInt32,
            vec![cursor_value],
            true,
        );
        let index = self.emit(Op::UnboxInt32, vec![cursor_index], Some(Repr::Int32));
        let count = self.emit(Op::LoadPropertyIteratorKeyCount, vec![keys], Some(Repr::Int32));
        let (more, finished) = self.branch_both_ways(BranchCondition::Int32(Comparison::LessThan), vec![index, count]);

        self.block = more;
        let index = self.refine(BranchCondition::Int32(Comparison::LessThan), true, vec![index, count]);
        let key = self.emit(Op::LoadPropertyIteratorKey, vec![keys, index], Some(Repr::Tagged));
        // NB: The index is below the count, so the next one is an int32 too.
        let one = self.int32_constant(1);
        let next = self.emit_checked(
            Op::Int32Binary { op: BinaryOp::Add },
            vec![index, one],
            Some(Repr::Int32),
        );
        let next = self.emit(Op::BoxInt32, vec![next], Some(Repr::Tagged));
        let not_done = self.constant(value::FALSE);

        let done = self.constant(value::TRUE);
        self.join_blocks(vec![more, finished]);
        let value = self.add_phi(vec![key, old_value]);
        let is_done = self.add_phi(vec![not_done, done]);
        let new_cursor = self.add_phi(vec![next, cursor_value]);

        let outputs = self.join_paths(slow_paths, vec![value, is_done, new_cursor], |_, outputs| outputs);
        self.write(dst_value, outputs[0])?;
        self.write(dst_done, outputs[1])?;
        self.write(cursor, outputs[2])?;
        Ok(Flow::Continue)
    }
}
