/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of the ops on strings (equality, lengths, code units, single
//! character strings), of `typeof`, and of the tests of which builtin a
//! function is.

use super::Codegen;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;
use crate::ir::TypeofKind;
use crate::ir::value;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// `StringsEqual`: strings with the same bits are equal. Others are not
    /// if both are interned (two interned strings are equal only if they are
    /// the same string, like the interpreter's `string_compare`) or if their
    /// lengths differ, and short strings (whose ASCII bytes are in their
    /// storage word, so that equal ones have equal words) compare their
    /// words. The others result in the empty value. Needs a temp.
    pub(in crate::codegen) fn emit_strings_equal(&mut self, node: NodeId) {
        let (lhs, rhs, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let temp = self.temp(node, 0);
        let (equal, not_equal, slow, done) = (
            self.masm.new_label(),
            self.masm.new_label(),
            self.masm.new_label(),
            self.masm.new_label(),
        );
        self.masm.branch64(Condition::Equal, lhs, rhs, equal);
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let is_interned = layout.primitive_string_is_interned as i32;
        let interned_mask = u32::from(layout.primitive_string_interned_mask);
        let contents = self.masm.new_label();
        for string in [lhs, rhs] {
            self.emit_unbox_cell(temp, string);
            self.masm.load8(scratch, &Address::new(temp, is_interned));
            self.masm
                .branch_test32(Condition::Zero, scratch, interned_mask, contents);
        }
        self.masm.jump(not_equal);

        self.masm.bind(contents);
        let length = layout.primitive_string_length as i32;
        self.emit_unbox_cell(temp, lhs);
        self.masm.load32(temp, &Address::new(temp, length));
        self.emit_unbox_cell(scratch, rhs);
        self.masm.load32(scratch, &Address::new(scratch, length));
        self.masm.branch32(Condition::NotEqual, temp, scratch, not_equal);
        // NB: Strings without storage yet (deferred ones) have no short flag.
        let storage = layout.primitive_string_storage as i32;
        let short_flag = u32::from(layout.utf16_short_string_flag);
        self.emit_unbox_cell(temp, lhs);
        self.masm.load64(temp, &Address::new(temp, storage));
        self.masm.branch_test32(Condition::Zero, temp, short_flag, slow);
        self.emit_unbox_cell(scratch, rhs);
        self.masm.load64(scratch, &Address::new(scratch, storage));
        self.masm.branch_test32(Condition::Zero, scratch, short_flag, slow);
        self.masm.branch64(Condition::Equal, temp, scratch, equal);
        self.masm.jump(not_equal);

        for (label, result) in [(equal, value::TRUE), (not_equal, value::FALSE), (slow, value::EMPTY)] {
            self.masm.bind(label);
            self.masm.move_imm64(output, result);
            self.masm.jump(done);
        }
        self.masm.bind(done);
    }

    /// Loads the code unit at `index` (an int32 in bounds) of the string at
    /// `string` (a `PrimitiveString` pointer), whose characters are
    /// readable, into `code_unit`, like the interpreter's
    /// `load_primitive_string_utf16_code_unit()`: from the bytes of an inline
    /// or a short string, or the ASCII or UTF-16 storage of a long one.
    fn emit_string_code_unit(&mut self, string: Gpr, index: Gpr, code_unit: Gpr) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (inline, short, wide, done) = (
            self.masm.new_label(),
            self.masm.new_label(),
            self.masm.new_label(),
            self.masm.new_label(),
        );
        self.masm.load8(
            scratch,
            &Address::new(string, layout.primitive_string_is_interned as i32),
        );
        self.masm
            .and32_imm(scratch, scratch, u32::from(layout.primitive_string_deferred_kind_mask));
        self.masm.branch32_imm(
            Condition::Equal,
            scratch,
            i32::from(layout.primitive_string_deferred_kind_inline),
            inline,
        );
        let storage = layout.primitive_string_storage as i32;
        self.masm.load64(code_unit, &Address::new(string, storage));
        self.masm.branch_test32(
            Condition::NonZero,
            code_unit,
            u32::from(layout.utf16_short_string_flag),
            short,
        );

        // `code_unit` points at the string's data.
        self.masm
            .load32(scratch, &Address::new(code_unit, layout.utf16_string_data_flags as i32));
        self.masm.branch_test32(
            Condition::NonZero,
            scratch,
            layout.utf16_string_data_has_utf16_storage,
            wide,
        );
        let units = layout.utf16_string_data_storage as i32;
        self.masm.move32(scratch, index);
        self.masm
            .load8(code_unit, &Address::indexed(code_unit, scratch, Scale::One, units));
        self.masm.jump(done);
        self.masm.bind(wide);
        self.masm.move32(scratch, index);
        self.masm
            .load16(code_unit, &Address::indexed(code_unit, scratch, Scale::Two, units));
        self.masm.jump(done);

        // The bytes of inline strings are in the cell.
        self.masm.bind(inline);
        self.masm.move32(scratch, index);
        self.masm.load8(
            code_unit,
            &Address::indexed(
                string,
                scratch,
                Scale::One,
                layout.primitive_string_inline_storage as i32,
            ),
        );
        self.masm.jump(done);

        // The bytes of short strings follow their count in the storage word.
        self.masm.bind(short);
        self.masm.move32(scratch, index);
        self.masm
            .load8(code_unit, &Address::indexed(string, scratch, Scale::One, storage + 1));
        self.masm.bind(done);
    }

    /// Jumps to `yes` if the characters of the string at `string` are readable,
    /// because it is no deferred string other than an inline one and has
    /// storage, and to `no` otherwise (see `emit_load_string_code_unit()`).
    pub(super) fn emit_resolved_string_test(&mut self, string: Gpr, yes: Label, no: Label) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.masm.load8(
            scratch,
            &Address::new(string, layout.primitive_string_is_interned as i32),
        );
        self.masm
            .and32_imm(scratch, scratch, u32::from(layout.primitive_string_deferred_kind_mask));
        self.masm.branch32_imm(
            Condition::Equal,
            scratch,
            i32::from(layout.primitive_string_deferred_kind_inline),
            yes,
        );
        self.masm.branch_test32(Condition::NonZero, scratch, u32::MAX, no);
        self.masm
            .load64(scratch, &Address::new(string, layout.primitive_string_storage as i32));
        self.masm.branch_test64(Condition::Zero, scratch, u64::MAX, no);
        self.masm.jump(yes);
    }

    /// Jumps to `yes` if `value` is the function object of builtin `id`, like
    /// `validate_builtin` checks it, and to `no` otherwise.
    pub(super) fn emit_builtin_test(&mut self, node: NodeId, value: Gpr, id: u8, yes: Label, no: Label) {
        let (scratch, function) = (self.pinned.scratch, self.temp(node, 0));
        let layout = self.runtime.layout;
        self.unbox_object_or_branch(function, value, no);
        self.masm.load16(
            scratch,
            &Address::new(function, self.runtime.offsets.object_flags as i32),
        );
        self.masm.branch_test32(
            Condition::Zero,
            scratch,
            u32::from(self.runtime.object_flag_is_function),
            no,
        );
        self.masm.load8(
            scratch,
            &Address::new(function, layout.function_object_has_builtin as i32),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, no);
        self.masm
            .load8(scratch, &Address::new(function, layout.function_object_builtin as i32));
        self.masm.branch32_imm(Condition::Equal, scratch, i32::from(id), yes);
        self.masm.jump(no);
    }

    /// `StringLength`: the length of a string.
    pub(super) fn emit_string_length(&mut self, node: NodeId) {
        let (string, output) = (self.input(node, 0), self.output(node));
        self.masm.load32(
            output,
            &Address::new(string, self.runtime.layout.primitive_string_length as i32),
        );
    }

    /// `LoadStringCodeUnit`: the code unit of a string whose characters are
    /// readable at an index in bounds.
    pub(super) fn emit_load_string_code_unit_node(&mut self, node: NodeId) {
        let (string, index, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        self.emit_string_code_unit(string, index, output);
    }

    /// `SingleCharacterString`: the VM's string of an ASCII code unit.
    pub(super) fn emit_single_character_string(&mut self, node: NodeId) {
        let (code_unit, output) = (self.input(node, 0), self.output(node));
        let scratch = self.pinned.scratch;
        // NB: Int32 values only have their low 32 bits defined.
        self.masm.move32(output, code_unit);
        self.masm
            .move_imm64(scratch, self.runtime.layout.single_ascii_character_strings);
        self.masm
            .load64(output, &Address::indexed(scratch, output, Scale::Eight, 0));
        self.box_cell_with_tag(output, output, value::STRING_TAG);
    }

    /// `typeof` of input 0 of `node`, like `Value::typeof_()`: the VM's
    /// string for the kind of the value.
    pub(super) fn emit_typeof(&mut self, node: NodeId) {
        let scratch = self.pinned.scratch;
        let value = self.input(node, 0);
        let output = self.output(node);
        let strings = self.runtime.layout.typeof_strings;
        let done = self.masm.new_label();
        let object = self.masm.new_label();
        self.branch_on_tag(Condition::Equal, value, value::OBJECT_TAG, scratch, object);
        let tags = [
            (value::STRING_TAG, strings.string),
            (value::UNDEFINED_TAG, strings.undefined),
            (value::BOOLEAN_TAG, strings.boolean),
            (value::INT32_TAG, strings.number),
            (value::NULL_TAG, strings.object),
            (value::SYMBOL_TAG, strings.symbol),
            (value::BIGINT_TAG, strings.bigint),
        ];
        for (tag, string) in tags {
            let next = self.masm.new_label();
            self.masm
                .branch32_imm(Condition::NotEqual, scratch, i32::from(tag), next);
            self.masm.move_imm64(output, string);
            self.masm.jump(done);
            self.masm.bind(next);
        }
        // Every other value is a double.
        self.masm.move_imm64(output, strings.number);
        self.masm.jump(done);

        // Objects are "function" if they are callable, unless they are
        // `[[IsHTMLDDA]]` objects, which are "undefined".
        self.masm.bind(object);
        self.emit_unbox_cell(scratch, value);
        self.masm.load16(
            scratch,
            &Address::new(scratch, self.runtime.offsets.object_flags as i32),
        );
        let (not_htmldda, not_function) = (self.masm.new_label(), self.masm.new_label());
        if self.htmldda_objects_may_exist() {
            self.masm.branch_test32(
                Condition::Zero,
                scratch,
                u32::from(self.runtime.layout.object_flag_is_htmldda),
                not_htmldda,
            );
            self.masm.move_imm64(output, strings.undefined);
            self.masm.jump(done);
        }
        self.masm.bind(not_htmldda);
        self.masm.branch_test32(
            Condition::Zero,
            scratch,
            u32::from(self.runtime.object_flag_is_function),
            not_function,
        );
        self.masm.move_imm64(output, strings.function);
        self.masm.jump(done);
        self.masm.bind(not_function);
        self.masm.move_imm64(output, strings.object);
        self.masm.bind(done);
    }

    /// Whether `typeof` of input 0 of `node` is the string of `kind` (or
    /// not, if not `equal`), as a `Repr::Bool`, like `emit_typeof()` but
    /// without making the string.
    pub(super) fn emit_typeof_is(&mut self, node: NodeId, kind: TypeofKind, equal: bool) {
        let scratch = self.pinned.scratch;
        let value = self.input(node, 0);
        let output = self.output(node);
        let (is, is_not, done) = (self.masm.new_label(), self.masm.new_label(), self.masm.new_label());
        self.masm.shr64_imm(scratch, value, value::TAG_SHIFT);
        let tag = |kind| match kind {
            TypeofKind::String => Some(value::STRING_TAG),
            TypeofKind::Symbol => Some(value::SYMBOL_TAG),
            TypeofKind::Bigint => Some(value::BIGINT_TAG),
            TypeofKind::Boolean => Some(crate::ir::value::BOOLEAN_TAG),
            _ => None,
        };
        if let Some(tag) = tag(kind) {
            self.masm.branch32_imm(Condition::Equal, scratch, i32::from(tag), is);
            self.masm.jump(is_not);
        } else if kind == TypeofKind::Number {
            self.masm
                .branch32_imm(Condition::Equal, scratch, i32::from(crate::ir::value::INT32_TAG), is);
            // NB: Canonical NaN has the tag bits other values have set.
            self.masm
                .branch32_imm(Condition::Equal, scratch, i32::from(value::BASE_TAG), is);
            self.masm.and32_imm(scratch, scratch, u32::from(value::BASE_TAG));
            self.masm
                .branch32_imm(Condition::NotEqual, scratch, i32::from(value::BASE_TAG), is);
            self.masm.jump(is_not);
        } else {
            // Undefined, null and objects, by their flags.
            let (undefined_is, null_is) = match kind {
                TypeofKind::Undefined => (is, is_not),
                TypeofKind::Object => (is_not, is),
                _ => (is_not, is_not),
            };
            self.masm.branch32_imm(
                Condition::Equal,
                scratch,
                i32::from(crate::ir::value::UNDEFINED_TAG),
                undefined_is,
            );
            self.masm.branch32_imm(
                Condition::Equal,
                scratch,
                i32::from(crate::ir::value::NULL_TAG),
                null_is,
            );
            self.masm.branch32_imm(
                Condition::NotEqual,
                scratch,
                i32::from(crate::ir::value::OBJECT_TAG),
                is_not,
            );
            if kind == TypeofKind::Undefined && !self.htmldda_objects_may_exist() {
                // NB: Without `[[IsHTMLDDA]]` objects, no object is "undefined".
                self.masm.jump(is_not);
            } else {
                self.emit_unbox_cell(scratch, value);
                self.masm.load16(
                    scratch,
                    &Address::new(scratch, self.runtime.offsets.object_flags as i32),
                );
                let htmldda = if self.htmldda_objects_may_exist() {
                    u32::from(self.runtime.layout.object_flag_is_htmldda)
                } else {
                    0
                };
                let function = u32::from(self.runtime.object_flag_is_function);
                match kind {
                    TypeofKind::Undefined => {
                        self.masm.branch_test32(Condition::NonZero, scratch, htmldda, is);
                        self.masm.jump(is_not);
                    }
                    TypeofKind::Function => {
                        if htmldda != 0 {
                            self.masm.branch_test32(Condition::NonZero, scratch, htmldda, is_not);
                        }
                        self.masm.branch_test32(Condition::NonZero, scratch, function, is);
                        self.masm.jump(is_not);
                    }
                    _ => {
                        self.masm
                            .branch_test32(Condition::NonZero, scratch, htmldda | function, is_not);
                        self.masm.jump(is);
                    }
                }
            }
        }
        self.masm.bind(is);
        self.masm.move_imm32(output, u32::from(equal));
        self.masm.jump(done);
        self.masm.bind(is_not);
        self.masm.move_imm32(output, u32::from(!equal));
        self.masm.bind(done);
    }
}
