/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of the ops of for-in loops, which read the keys of the property
//! iterator cache (`ObjectPropertyIteratorCacheData`) of the object they
//! iterate over, like the interpreter's `ObjectPropertyIteratorNext`
//! handler.

use super::Codegen;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Jumps to `yes` if the property iterator cache `keys` still holds the
    /// keys of the object `receiver`: the cache has a fast path, the
    /// receiver has the shape (and for dictionaries, its generation) the
    /// keys were taken from, as many packed indexed properties if the keys
    /// include those, and the prototype chain is unchanged if the cache
    /// depends on it. Jumps to `no` otherwise.
    pub(in crate::codegen) fn emit_property_iterator_cache_test(
        &mut self,
        node: NodeId,
        receiver: Gpr,
        keys: Gpr,
        yes: Label,
        no: Label,
    ) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (object, cache, temp) = (self.temp(node, 0), self.temp(node, 1), self.temp(node, 2));

        self.emit_unbox_cell(object, receiver);
        self.emit_unbox_cell(cache, keys);
        self.masm
            .load8(scratch, &Address::new(cache, layout.property_iterator_fast_path as i32));
        self.masm.branch32_imm(
            Condition::Equal,
            scratch,
            i32::from(layout.property_iterator_fast_path_none),
            no,
        );

        // The receiver's shape, and for dictionaries its generation.
        self.masm
            .load64(temp, &Address::new(object, self.runtime.offsets.object_shape as i32));
        self.masm.branch64_memory(
            Condition::NotEqual,
            &Address::new(cache, layout.property_iterator_shape as i32),
            temp,
            no,
        );
        let not_dictionary = self.masm.new_label();
        self.masm.load8(
            scratch,
            &Address::new(cache, layout.property_iterator_shape_is_dictionary as i32),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, not_dictionary);
        let expected = Address::new(cache, layout.property_iterator_shape_dictionary_generation as i32);
        self.branch_unless_dictionary_generation_is(temp, expected, [temp, scratch], no);
        self.masm.bind(not_dictionary);

        // Packed indexed properties, as many as when the keys were taken.
        let not_indexed = self.masm.new_label();
        self.masm
            .load8(scratch, &Address::new(cache, layout.property_iterator_fast_path as i32));
        self.masm.branch32_imm(
            Condition::NotEqual,
            scratch,
            i32::from(layout.property_iterator_fast_path_packed_indexed),
            not_indexed,
        );
        self.masm.load8(
            scratch,
            &Address::new(object, layout.object_indexed_storage_kind as i32),
        );
        self.masm.branch32_imm(
            Condition::NotEqual,
            scratch,
            i32::from(layout.indexed_storage_kind_packed),
            no,
        );
        self.masm.load32(
            temp,
            &Address::new(object, layout.object_indexed_array_like_size as i32),
        );
        self.masm.load32(
            scratch,
            &Address::new(cache, layout.property_iterator_indexed_property_count as i32),
        );
        self.masm.branch32(Condition::NotEqual, temp, scratch, no);
        self.masm.bind(not_indexed);

        // The prototype chain, if the keys depend on it.
        self.masm.load64(
            scratch,
            &Address::new(cache, layout.property_iterator_prototype_chain_validity as i32),
        );
        self.masm.branch_test64(Condition::Zero, scratch, u64::MAX, yes);
        self.branch_unless_prototype_chain_valid(scratch, no);
        self.masm.jump(yes);
    }

    /// `LoadPropertyIteratorKeyCount`: the number of keys of a property
    /// iterator cache, at most `i32::MAX`.
    pub(in crate::codegen) fn emit_load_property_iterator_key_count(&mut self, node: NodeId) {
        let (keys, output) = (self.input(node, 0), self.output(node));
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let small = self.masm.new_label();
        self.emit_unbox_cell(output, keys);
        self.masm.load64(
            output,
            &Address::new(output, layout.property_iterator_property_value_count as i32),
        );
        self.masm.move_imm64(scratch, i32::MAX as u64);
        self.masm.branch64(Condition::BelowOrEqual, output, scratch, small);
        self.masm.move64(output, scratch);
        self.masm.bind(small);
    }

    /// `LoadPropertyIteratorKey`: the key of a property iterator cache at
    /// an index below their number.
    pub(in crate::codegen) fn emit_load_property_iterator_key(&mut self, node: NodeId) {
        let (keys, index, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.emit_unbox_cell(output, keys);
        self.masm.load64(
            output,
            &Address::new(output, layout.property_iterator_property_values as i32),
        );
        // NB: Int32 values only have their low 32 bits defined.
        self.masm.move32(scratch, index);
        self.masm
            .load64(output, &Address::indexed(output, scratch, Scale::Eight, 0));
    }
}
