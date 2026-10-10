/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of `ConcatenateStrings` and `IntegerToString`. Two strings make
//! a rope string, allocated inline per the allocation contract (see
//! `RopeAllocationInfo`), unless one of them is empty (then the result is
//! the other one). Concatenations short enough to become short flat strings
//! (whose ASCII bytes are in their storage word) find them in the VM's cache
//! of fly strings if both strings are short. Everything else results in the
//! empty value, for the slow path, which also fills that cache.

use super::Codegen;
use super::call::ConstantField;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;
use crate::ir::value;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// `ConcatenateStrings`: the concatenation of two strings, or the empty
    /// value. Needs three temps.
    pub(in crate::codegen) fn emit_concatenate_strings(&mut self, node: NodeId) {
        let (lhs, rhs, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let temps = &self.allocation.node(node).temps;
        let temps = [Gpr(temps[0]), Gpr(temps[1]), Gpr(temps[2])];
        let (slow, done) = (self.masm.new_label(), self.masm.new_label());
        if self.runtime.rope_allocation.allocator != 0 && self.rope_template_fits() {
            self.emit_concatenation_of_strings(node, [lhs, rhs], temps, slow, done);
        }
        self.masm.bind(slow);
        self.masm.move_imm64(output, value::EMPTY);
        self.masm.bind(done);
    }

    /// `IntegerToString`: the VM's string of an int32, as
    /// `PrimitiveString::create_from_unsigned_integer()` finds it, which is
    /// what ToString makes of it, or the empty value for integers the VM has
    /// no string of yet (and negative ones).
    pub(in crate::codegen) fn emit_integer_to_string(&mut self, node: NodeId) {
        let (integer, string) = (self.input(node, 0), self.output(node));
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (missing, done) = (self.masm.new_label(), self.masm.new_label());
        if layout.numeric_string_cache == 0 {
            self.masm.jump(missing);
        }
        // NB: Negative integers are above every size.
        self.masm.move32(scratch, integer);
        self.masm.branch32_imm(
            Condition::AboveOrEqual,
            scratch,
            layout.numeric_string_cache_size as i32,
            missing,
        );
        self.masm.move_imm64(string, layout.numeric_string_cache);
        self.masm
            .load64(string, &Address::indexed(string, scratch, Scale::Eight, 0));
        self.masm.branch_test64(Condition::Zero, string, u64::MAX, missing);
        self.box_cell_with_tag(string, string, value::STRING_TAG);
        self.masm.jump(done);
        self.masm.bind(missing);
        self.masm.move_imm64(string, value::EMPTY);
        self.masm.bind(done);
    }

    /// Whether the fields JIT code stores into a new rope string are within
    /// its template, and the template within its cell.
    fn rope_template_fits(&self) -> bool {
        let rope = &self.runtime.rope_allocation;
        let bytes = 8 * rope.template.len() as u64;
        bytes <= u64::from(rope.cell_size)
            && [rope.lhs_offset, rope.rhs_offset]
                .iter()
                .all(|offset| offset.is_multiple_of(8) && u64::from(*offset) + 8 <= bytes)
            && u64::from(rope.length_offset) + 4 <= bytes
    }

    /// The concatenation of the strings `lhs` and `rhs` into the node's
    /// output, continuing at `done`, with the temps `cell`, `other` and
    /// `length`.
    fn emit_concatenation_of_strings(
        &mut self,
        node: NodeId,
        [lhs, rhs]: [Gpr; 2],
        [cell, other, length]: [Gpr; 3],
        slow: Label,
        done: Label,
    ) {
        let scratch = self.pinned.scratch;
        let output = self.output(node);
        let rope = self.runtime.rope_allocation.clone();
        let string_length = |string: Gpr| Address::new(string, rope.length_offset as i32);
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);

        // An empty string concatenates to the other one.
        let lhs_not_empty = self.masm.new_label();
        let rhs_not_empty = self.masm.new_label();
        self.emit_unbox_cell(length, lhs);
        self.masm.load32(length, &string_length(length));
        self.emit_unbox_cell(other, rhs);
        self.masm.load32(other, &string_length(other));
        self.masm
            .branch_test32(Condition::NonZero, length, u32::MAX, lhs_not_empty);
        self.masm.move64(output, rhs);
        self.masm.jump(done);
        self.masm.bind(lhs_not_empty);
        self.masm
            .branch_test32(Condition::NonZero, other, u32::MAX, rhs_not_empty);
        self.masm.move64(output, lhs);
        self.masm.jump(done);
        self.masm.bind(rhs_not_empty);

        // A rope, unless the result could be a short flat string, or is too
        // long for a string.
        self.masm.add64(length, length, other);
        let short = self.masm.new_label();
        self.masm
            .branch64_imm(Condition::Below, length, i64::from(rope.min_length), short);
        self.masm.add64_imm(scratch, length, 1);
        self.masm.shr64_imm(scratch, scratch, 32);
        self.masm.branch_test64(Condition::NonZero, scratch, u64::MAX, slow);

        // The cell, per the allocation contract.
        self.emit_check_heap_threshold(rope.cell_size, cell, other, slow);
        self.emit_pop_free_list(rope.allocator, cell, other, slow);
        self.emit_count_allocation(rope.cell_size, rope.cell_size, other, false);

        let mut fields: Vec<ConstantField> = (0u32..)
            .zip(&rope.template)
            .map(|(index, word)| (8 * index, 8, *word))
            .filter(|(offset, _, _)| *offset != rope.lhs_offset && *offset != rope.rhs_offset)
            .collect();
        self.emit_constant_fields(cell, other, &mut fields)
            .expect("rope string fields are near the cell");
        self.masm.store32(&string_length(cell), length);
        self.emit_unbox_cell(other, lhs);
        self.masm.store64(&at(cell, rope.lhs_offset), other);
        self.emit_unbox_cell(other, rhs);
        self.masm.store64(&at(cell, rope.rhs_offset), other);
        self.box_cell_with_tag(output, cell, value::STRING_TAG);
        self.masm.jump(done);

        self.masm.bind(short);
        self.emit_short_string_concatenation(node, [lhs, rhs], [cell, other, length], slow, done);
    }

    /// The concatenation of the non-empty strings `lhs` and `rhs`, whose
    /// lengths add up to that of a short string, into the node's output if
    /// both are short strings and the VM's fly string cache has the result,
    /// continuing at `done`; branches to `slow` otherwise. Uses `word` and
    /// `other`, and `amount` once done with `lhs` and `rhs`.
    fn emit_short_string_concatenation(
        &mut self,
        node: NodeId,
        [lhs, rhs]: [Gpr; 2],
        [word, other, amount]: [Gpr; 3],
        slow: Label,
        done: Label,
    ) {
        let layout = self.runtime.layout;
        if layout.fly_string_cache == 0 {
            self.masm.jump(slow);
            return;
        }
        let scratch = self.pinned.scratch;
        let output = self.output(node);
        let storage = layout.primitive_string_storage as i32;
        let short_flag = u32::from(layout.utf16_short_string_flag);
        let count_shift = layout.utf16_short_string_byte_count_shift;

        // NB: Strings without storage yet (deferred ones) have no short flag.
        self.emit_unbox_cell(word, lhs);
        self.masm.load64(word, &Address::new(word, storage));
        self.masm.branch_test32(Condition::Zero, word, short_flag, slow);
        self.emit_unbox_cell(other, rhs);
        self.masm.load64(other, &Address::new(other, storage));
        self.masm.branch_test32(Condition::Zero, other, short_flag, slow);

        // The word of the concatenation, like `concatenate_short_ascii_strings()`:
        // the count and flag in its lowest byte, then the bytes of `lhs`, then
        // those of `rhs`. Unused bytes of short strings are 0, and adding the
        // tags (minus a flag) adds the counts.
        self.masm.and32_imm(amount, word, 0xFF);
        self.masm.shr32_imm(amount, amount, count_shift);
        self.masm.shl32_imm(amount, amount, 3);
        self.masm.add32_imm(amount, amount, 8);
        self.masm.and32_imm(scratch, other, 0xFF);
        self.masm.add64(word, word, scratch);
        self.masm.sub64_imm(word, word, i64::from(short_flag));
        self.masm.shr64_imm(other, other, 8);
        self.masm.shl64(other, other, amount);
        self.masm.or64(word, word, other);

        // Its slot in the cache, at the MurmurHash3 64-bit finalizer of it.
        self.masm.move64(other, word);
        for multiplier in [0xff51_afd7_ed55_8ccd_u64, 0xc4ce_b9fe_1a85_ec53] {
            self.masm.shr64_imm(amount, other, 33);
            self.masm.xor64(other, other, amount);
            self.masm.move_imm64(amount, multiplier);
            self.masm.mul64(other, other, amount);
        }
        self.masm.shr64_imm(amount, other, 33);
        self.masm.xor64(other, other, amount);
        self.masm.and32_imm(other, other, layout.fly_string_cache_mask);
        self.masm.move_imm64(amount, layout.fly_string_cache);
        self.masm
            .load64(other, &Address::indexed(amount, other, Scale::Eight, 0));
        self.masm.branch_test64(Condition::Zero, other, u64::MAX, slow);
        self.masm
            .branch64_memory(Condition::NotEqual, &Address::new(other, storage), word, slow);
        self.box_cell_with_tag(output, other, value::STRING_TAG);
        self.masm.jump(done);
    }
}
