/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls of builtin functions the code knows (see `Intrinsic`) that compiled
//! code runs inline.

use super::Flow;
use super::GraphBuilder;
use super::checks::SlowPaths;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::ElementsKind;
use crate::ir::Float64UnaryOp;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::snapshot::CellId;
use crate::snapshot::Intrinsic;

/// What a call of a string builtin that reads a character returns.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StringCharacter {
    /// `String.prototype.charCodeAt`: the code unit.
    CodeUnit,
    /// `String.prototype.charAt`: the string of the code unit.
    String,
}

impl GraphBuilder<'_> {
    /// Builds `Math.<op>(argument)`, a `CallBuiltinMath*` instruction, for
    /// numbers, if `callee` is the builtin of any realm. The instruction's
    /// slow path, which calls the callee, handles everything else.
    pub(super) fn build_math_function(
        &mut self,
        op: Float64UnaryOp,
        dst: Operand,
        callee: Operand,
        argument: Operand,
    ) -> Result<Flow, CompileFailure> {
        let layout = &self.runtime.layout;
        let builtin = match op {
            Float64UnaryOp::Abs => layout.builtin_math_abs,
            Float64UnaryOp::Floor => layout.builtin_math_floor,
            Float64UnaryOp::Ceil => layout.builtin_math_ceil,
            Float64UnaryOp::Round => layout.builtin_math_round,
            Float64UnaryOp::Sqrt => layout.builtin_math_sqrt,
            Float64UnaryOp::Negate => unreachable!("no Math function negates"),
        };
        let callee = self.read(callee)?;
        let argument = self.read(argument)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        self.branch_to_slow_path(&mut slow_paths, BranchCondition::Builtin(builtin), vec![callee], true);
        let result = if let Some(integer) = self.int32_value_of(argument) {
            self.build_int32_math_function(&mut slow_paths, op, integer)
        } else {
            // NB: Int32 arguments, then doubles.
            let (int32, other) = self.branch_both_ways(BranchCondition::Int32Value, vec![argument]);
            self.block = other;
            let double = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Double, vec![argument], true);
            let double = self.emit(Op::UnboxDouble, vec![double], Some(Repr::Float64));
            let result = self.emit(Op::Float64Unary { op }, vec![double], Some(Repr::Float64));
            let double_result = self.emit(Op::BoxFloat64, vec![result], Some(Repr::Tagged));
            let double = self.block;

            self.block = int32;
            let integer = self.refine(BranchCondition::Int32Value, true, vec![argument]);
            let integer = self.emit(Op::UnboxInt32, vec![integer], Some(Repr::Int32));
            let int32_result = self.build_int32_math_function(&mut slow_paths, op, integer);
            let int32 = self.block;

            self.join_blocks(vec![int32, double]);
            self.add_phi(vec![int32_result, double_result])
        };
        let result = self
            .join_slow_paths(slow_paths, Some(result))
            .expect("the instruction has a value");
        self.write(dst, result)?;
        Ok(Flow::Continue)
    }

    /// The `Repr::Int32` value of `value`, if it is known to be an int32:
    /// itself, or what it boxes.
    pub(super) fn int32_value_of(&self, value: NodeId) -> Option<NodeId> {
        let node = self.graph.node(value);
        match (node.repr, &node.op) {
            (Some(Repr::Int32), _) => Some(value),
            (_, Op::BoxInt32) => Some(node.inputs[0]),
            _ => None,
        }
    }

    /// The `Repr::Float64` of the `Repr::Int32` `integer`.
    pub(super) fn int32_to_float64(&mut self, integer: NodeId) -> NodeId {
        match self.graph.constant_value(integer) {
            Some(bits) => self.typed_constant(f64::from((bits as u32).cast_signed()).to_bits(), Repr::Float64),
            None => self.emit(Op::Int32ToFloat64, vec![integer], Some(Repr::Float64)),
        }
    }

    /// `value` as a `Repr::Float64`, where it is a number, taking a slow
    /// path of `slow_paths` otherwise.
    pub(super) fn float64_of_number(&mut self, slow_paths: &mut SlowPaths, value: NodeId) -> NodeId {
        self.float64_of(value, |builder, condition, inputs| {
            builder.branch_to_slow_path(slow_paths, condition, inputs, true)
        })
    }

    /// `value` as a `Repr::Int32`, where it is an int32, taking a slow path
    /// of `slow_paths` otherwise.
    pub(super) fn int32_or_slow_path(&mut self, slow_paths: &mut SlowPaths, value: NodeId) -> NodeId {
        if let Some(integer) = self.int32_value_of(value) {
            return integer;
        }
        let value = self.tagged(value);
        let refined = self.branch_to_slow_path(slow_paths, BranchCondition::Int32Value, vec![value], true);
        self.emit(Op::UnboxInt32, vec![refined], Some(Repr::Int32))
    }

    /// Branches on whether the tagged `value` is an int32, and if not on
    /// whether it is a double, where `otherwise` branches on that (and
    /// returns the refinement of the value where it is one). The value of
    /// the int32 case is `int32` of its `Repr::Int32`, that of the double
    /// case `double` of its `Repr::Float64`. Returns the phi of those, of
    /// `repr`.
    pub(super) fn on_int32_or_double(
        &mut self,
        value: NodeId,
        repr: Repr,
        int32: fn(&mut Self, NodeId) -> NodeId,
        double: fn(&mut Self, NodeId) -> NodeId,
        otherwise: impl FnOnce(&mut Self, BranchCondition, Vec<NodeId>) -> NodeId,
    ) -> NodeId {
        let (int32_block, other) = self.branch_both_ways(BranchCondition::Int32Value, vec![value]);
        self.block = int32_block;
        let refined = self.refine(BranchCondition::Int32Value, true, vec![value]);
        let integer = self.emit(Op::UnboxInt32, vec![refined], Some(Repr::Int32));
        let int32_value = int32(self, integer);
        let int32_end = self.block;

        self.block = other;
        let refined = otherwise(self, BranchCondition::Double, vec![value]);
        let number = self.emit(Op::UnboxDouble, vec![refined], Some(Repr::Float64));
        let double_value = double(self, number);
        let double_end = self.block;

        self.join_blocks(vec![int32_end, double_end]);
        let phi = self.add_phi(vec![int32_value, double_value]);
        self.graph.nodes[phi.index()].repr = Some(repr);
        phi
    }

    /// The tagged result of `Math.<op>(integer)` of a `Repr::Int32`
    /// `integer`: itself for the functions that round, and the absolute
    /// value but for the smallest int32, whose absolute value is no int32
    /// and goes to a slow path of `slow_paths`.
    fn build_int32_math_function(&mut self, slow_paths: &mut SlowPaths, op: Float64UnaryOp, integer: NodeId) -> NodeId {
        match op {
            Float64UnaryOp::Floor | Float64UnaryOp::Ceil | Float64UnaryOp::Round => self.tagged(integer),
            Float64UnaryOp::Abs => {
                let smallest = self.int32_constant(i32::MIN);
                let integer = self.branch_to_slow_path(
                    slow_paths,
                    BranchCondition::Int32(Comparison::StrictlyEquals),
                    vec![integer, smallest],
                    false,
                );
                let result = self.emit(Op::Int32Abs, vec![integer], Some(Repr::Int32));
                self.tagged(result)
            }
            Float64UnaryOp::Sqrt | Float64UnaryOp::Negate => {
                let double = self.int32_to_float64(integer);
                let result = self.emit(Op::Float64Unary { op }, vec![double], Some(Repr::Float64));
                self.emit(Op::BoxFloat64, vec![result], Some(Repr::Tagged))
            }
        }
    }

    /// Builds `this_value.charCodeAt(argument)` or `.charAt(argument)`, a
    /// `CallBuiltinStringPrototypeCharCodeAt` or `CharAt` instruction, for
    /// strings whose characters are readable and int32 indices in bounds,
    /// if `callee` is the builtin of any realm. `charAt` makes the strings
    /// of ASCII code units, which the VM keeps. The instruction's slow path,
    /// which calls the callee, handles everything else.
    #[expect(clippy::too_many_arguments, reason = "the operands of the instruction")]
    pub(super) fn build_string_character(
        &mut self,
        character: StringCharacter,
        dst: Operand,
        callee: Operand,
        this_value: Operand,
        argument: Operand,
    ) -> Result<Flow, CompileFailure> {
        let layout = &self.runtime.layout;
        let builtin = match character {
            StringCharacter::CodeUnit => layout.builtin_string_prototype_char_code_at,
            StringCharacter::String => layout.builtin_string_prototype_char_at,
        };
        let callee = self.read(callee)?;
        let string = self.read(this_value)?;
        let index = self.read(argument)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        self.branch_to_slow_path(&mut slow_paths, BranchCondition::Builtin(builtin), vec![callee], true);
        let index = self.int32_or_slow_path(&mut slow_paths, index);
        let code_unit = self.string_code_unit(&mut slow_paths, string, index);
        let result = match character {
            StringCharacter::CodeUnit => self.emit(Op::BoxInt32, vec![code_unit], Some(Repr::Tagged)),
            StringCharacter::String => self.ascii_character_string(&mut slow_paths, code_unit),
        };
        let result = self
            .join_slow_paths(slow_paths, Some(result))
            .expect("the instruction has a value");
        self.write(dst, result)?;
        Ok(Flow::Continue)
    }

    /// The code unit at `index`, a `Repr::Int32`, of the tagged `string`,
    /// taking a slow path of `slow_paths` unless it is a string whose
    /// characters can be read and the index is in its bounds.
    pub(super) fn string_code_unit(&mut self, slow_paths: &mut SlowPaths, string: NodeId, index: NodeId) -> NodeId {
        let string = self.branch_to_slow_path(slow_paths, BranchCondition::String, vec![string], true);
        let address = self.emit(Op::StringAddress, vec![string], Some(Repr::Pointer));
        let readable = self.branch_to_slow_path(slow_paths, BranchCondition::ResolvedString, vec![address], true);
        let length = self.emit(Op::StringLength, vec![address], Some(Repr::Int32));
        let index = self.branch_to_slow_path(slow_paths, BranchCondition::IndexInBounds, vec![index, length], true);
        self.emit(Op::LoadStringCodeUnit, vec![readable, index], Some(Repr::Int32))
    }

    /// The VM's string of `code_unit`, taking a slow path of `slow_paths`
    /// unless it is ASCII.
    pub(super) fn ascii_character_string(&mut self, slow_paths: &mut SlowPaths, code_unit: NodeId) -> NodeId {
        let limit = self.int32_constant(0x80);
        let code_unit = self.branch_to_slow_path(
            slow_paths,
            BranchCondition::Int32(Comparison::LessThan),
            vec![code_unit, limit],
            true,
        );
        self.emit(Op::SingleCharacterString, vec![code_unit], Some(Repr::Tagged))
    }

    /// Builds `array.push(value)`, a `Call` whose callee is known to be
    /// `Array.prototype.push`, for arrays it appends to without any
    /// observable step, and exits for anything else: the value becomes the
    /// next element where the elements have room for it, and a cold block
    /// calls the runtime to grow them otherwise. Returns `None`, having
    /// built nothing, if it is not such a call.
    pub(super) fn try_build_array_push(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            dst,
            callee,
            this_value,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        let [value] = arguments.as_slice() else {
            return Ok(None);
        };
        if !self.may_speculate(ExitKind::SlowPath)
            || self.holds_virtual_arguments(*this_value)
            || self.holds_virtual_arguments(*value)
        {
            return Ok(None);
        }
        if self.constant_intrinsic(*callee) != Some(Intrinsic::ArrayPrototypePush) {
            return Ok(None);
        }
        let receiver = self.read(*this_value)?;
        let value = self.read(*value)?;
        self.embed(self.runtime.array_prototype);
        self.embed(self.runtime.object_prototype);
        let receiver = self.emit_checked(Op::CheckAppendableArray, vec![receiver], Some(Repr::Tagged));
        let array = self.cell_address(receiver);
        let length = self.emit(Op::LoadElementsLength, vec![array], Some(Repr::Int32));
        let capacity = self.emit(Op::LoadElementsCapacity, vec![array], Some(Repr::Int32));
        let (hot, cold) = self.branch_with_cold_side(
            BranchCondition::Int32(Comparison::LessThan),
            vec![length, capacity],
            true,
        );
        self.block = cold;
        let pushed = self.emit(Op::CallArrayPush, vec![receiver, value], Some(Repr::Tagged));
        self.block = hot;
        let length = self.refine(
            BranchCondition::Int32(Comparison::LessThan),
            true,
            vec![length, capacity],
        );
        let new_length = self.emit(Op::AppendElement, vec![array, length, value], Some(Repr::Int32));
        let new_length = self.emit(Op::BoxInt32, vec![new_length], Some(Repr::Tagged));
        let hot = self.block;
        self.join_blocks(vec![hot, cold]);
        let length = self.add_phi(vec![new_length, pushed]);
        self.write(*dst, length)?;
        Ok(Some(Flow::Continue))
    }

    /// The intrinsic a call site's feedback saw as its single callee, with
    /// the code checking that `callee` is still that function. Returns `None`,
    /// having built nothing, if the site saw another callee.
    fn checked_intrinsic_callee(
        &mut self,
        call_feedback: u16,
        callee: Operand,
    ) -> Result<Option<Intrinsic>, CompileFailure> {
        let Some((target, intrinsic)) = self
            .call_feedback(call_feedback)
            .and_then(|feedback| feedback.monomorphic_intrinsic())
        else {
            return Ok(None);
        };
        if !self.may_speculate(ExitKind::SlowPath) || !self.may_speculate(ExitKind::BadCallTarget) {
            return Ok(None);
        }
        self.check_callee(callee, target)?;
        Ok(Some(intrinsic))
    }

    /// Makes the code exit unless the value of `operand` is the function
    /// `expected`, which needs no check where it is that constant.
    pub(super) fn check_callee(&mut self, operand: Operand, expected: CellId) -> Result<(), CompileFailure> {
        let value = self.read(operand)?;
        let expected_bits = self.boxed_object(expected);
        if self.graph.constant_value(value) == Some(expected_bits) {
            return Ok(());
        }
        self.embed(expected);
        self.emit_checked(
            Op::CheckValue {
                expected: expected_bits,
                kind: ExitKind::BadCallTarget,
            },
            vec![value],
            None,
        );
        Ok(())
    }

    /// Builds calls of the `String`, `Array`, `Object` and `Boolean`
    /// constructors with at most one argument, if the site saw only that
    /// constructor: `Boolean(x)` is the truthiness of `x`, `String(x)` is a
    /// string `x` itself or the string of another primitive, `Array(n)` an
    /// array of length `n`, and `Object(x)` an object `x` itself or the
    /// object of another primitive but undefined and null. The call runs in
    /// a cold slow path otherwise. Returns `None`, having built nothing, if
    /// the `Call` is no such call.
    pub(super) fn try_build_builtin_constructor_call(
        &mut self,
        instruction: &Instruction,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            call_feedback,
            dst,
            callee,
            arguments,
            ..
        } = instruction
        else {
            return Ok(None);
        };
        if arguments.len() > 1 || arguments.iter().any(|operand| self.holds_virtual_arguments(*operand)) {
            return Ok(None);
        }
        let Some(constructor) = self.peek_intrinsic_callee(*call_feedback).filter(|intrinsic| {
            matches!(
                intrinsic,
                Intrinsic::StringConstructor
                    | Intrinsic::ArrayConstructor
                    | Intrinsic::ObjectConstructor
                    | Intrinsic::BooleanConstructor
            )
        }) else {
            return Ok(None);
        };
        if self.checked_intrinsic_callee(*call_feedback, *callee)?.is_none() {
            return Ok(None);
        }
        // 20.3.1.1 Boolean ( value ), https://tc39.es/ecma262/#sec-boolean-constructor-boolean-value
        if constructor == Intrinsic::BooleanConstructor {
            match arguments.first() {
                Some(argument) => self.build_truthiness_value(*dst, *argument, false)?,
                None => {
                    let falsity = self.constant(crate::ir::value::FALSE);
                    self.write(*dst, falsity)?;
                }
            }
            return Ok(Some(Flow::Continue));
        }
        // 23.1.1.1 Array ( ...values ), https://tc39.es/ecma262/#sec-array
        if constructor == Intrinsic::ArrayConstructor {
            let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
            let length = match arguments.first() {
                Some(argument) => {
                    let length = self.read(*argument)?;
                    // NB: Other numbers are lengths that throw, and other
                    //     values elements.
                    let length = self.branch_to_slow_path(
                        &mut slow_paths,
                        BranchCondition::NonNegativeInt32,
                        vec![length],
                        true,
                    );
                    self.emit(Op::UnboxInt32, vec![length], Some(Repr::Int32))
                }
                None => self.int32_constant(0),
            };
            let function = self.read(*callee)?;
            let array = self.emit(Op::ArrayCreate, vec![length, function], Some(Repr::Tagged));
            let array = self.branch_if_empty(&mut slow_paths, array);
            let array = self
                .join_slow_paths(slow_paths, Some(array))
                .expect("calls have a value");
            self.write(*dst, array)?;
            return Ok(Some(Flow::Continue));
        }
        let Some(argument) = arguments.first() else {
            return Ok(None);
        };
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let value = self.read(*argument)?;
        let result = if constructor == Intrinsic::StringConstructor {
            // 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
            let (string, other) = self.branch_both_ways(BranchCondition::String, vec![value]);
            self.block = string;
            let string = self.refine(BranchCondition::String, true, vec![value]);
            self.end_hot_path(&mut slow_paths, vec![string]);
            // NB: The VM has the strings of small integers.
            self.block = other;
            let (int32, other) = self.branch_both_ways(BranchCondition::Int32Value, vec![value]);
            self.block = int32;
            let integer = self.refine(BranchCondition::Int32Value, true, vec![value]);
            let unboxed = self.emit(Op::UnboxInt32, vec![integer], Some(Repr::Int32));
            let cached = self.emit(Op::IntegerToString, vec![unboxed], Some(Repr::Tagged));
            let empty = self.constant(crate::ir::value::EMPTY);
            let missing = BranchCondition::TaggedEquals { equal: true };
            let (miss, hit) = self.branch_both_ways(missing, vec![cached, empty]);
            self.block = hit;
            let cached = self.refine(missing, false, vec![cached, empty]);
            self.end_hot_path(&mut slow_paths, vec![cached]);
            self.block = miss;
            let string = self.emit(Op::PrimitiveToString, vec![integer], Some(Repr::Tagged));
            self.end_hot_path(&mut slow_paths, vec![string]);
            self.block = other;
            let primitive = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, vec![value], false);
            self.emit(Op::PrimitiveToString, vec![primitive], Some(Repr::Tagged))
        } else {
            // 20.1.1.1 Object ( [ value ] ), https://tc39.es/ecma262/#sec-object-value
            let (object, other) = self.branch_both_ways(BranchCondition::Object, vec![value]);
            self.block = object;
            let object = self.refine(BranchCondition::Object, true, vec![value]);
            self.end_hot_path(&mut slow_paths, vec![object]);
            self.block = other;
            let primitive = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Nullish, vec![value], false);
            let function = self.read(*callee)?;
            let object = self.emit(Op::ToObject, vec![primitive, function], Some(Repr::Tagged));
            self.branch_if_empty(&mut slow_paths, object)
        };
        self.end_hot_path(&mut slow_paths, vec![result]);
        let result = self.join_hot_paths(slow_paths).expect("calls have a value");
        self.write(*dst, result)?;
        Ok(Some(Flow::Continue))
    }

    /// Builds `String.fromCharCode` of an int32 whose code unit (ToUint16)
    /// is ASCII as the VM's string of that character, if the site saw only
    /// the builtin, and in a cold slow path otherwise. Returns `None`,
    /// having built nothing, if the site saw something else.
    pub(super) fn try_build_string_from_char_code(
        &mut self,
        instruction: &Instruction,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::CallBuiltinStringFromCharCode {
            call_feedback,
            dst,
            callee,
            argument,
            ..
        } = *instruction
        else {
            return Ok(None);
        };
        if self.holds_virtual_arguments(argument) {
            return Ok(None);
        }
        // NB: Checks nothing yet, so building nothing is fine.
        if self.peek_intrinsic_callee(call_feedback) != Some(Intrinsic::StringFromCharCode)
            || self.checked_intrinsic_callee(call_feedback, callee)?.is_none()
        {
            return Ok(None);
        }
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let code = self.read(argument)?;
        let code = self.int32_or_slow_path(&mut slow_paths, code);
        let mask = self.int32_constant(0xFFFF);
        let code_unit = self.emit(
            Op::Int32Binary {
                op: BinaryOp::BitwiseAnd,
            },
            vec![code, mask],
            Some(Repr::Int32),
        );
        let ascii_limit = self.int32_constant(0x80);
        let code_unit = self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::Int32(Comparison::LessThan),
            vec![code_unit, ascii_limit],
            true,
        );
        let string = self.emit(Op::SingleCharacterString, vec![code_unit], Some(Repr::Tagged));
        let result = self
            .join_slow_paths(slow_paths, Some(string))
            .expect("calls have a value");
        self.write(dst, result)?;
        Ok(Some(Flow::Continue))
    }

    /// The intrinsic a call site's feedback saw as its single callee, if any.
    fn peek_intrinsic_callee(&self, call_feedback: u16) -> Option<Intrinsic> {
        self.call_feedback(call_feedback)?
            .monomorphic_intrinsic()
            .map(|(_, intrinsic)| intrinsic)
    }

    /// Builds `CreateDataPropertyOrThrow` of an index of an object with
    /// packed or holey elements: a store to an element it has, or that fills
    /// a hole of an extensible one, and an append of the next element of a
    /// packed array where there is room, like `Array.prototype.push` appends
    /// it (defining an element runs no code). Other keys and objects take a
    /// cold slow path.
    pub(super) fn build_create_data_property(
        &mut self,
        object: Operand,
        property: Operand,
        value: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        let operands = [object, property, value];
        if !self.may_speculate(ExitKind::SlowPath)
            || operands.iter().any(|operand| self.holds_virtual_arguments(*operand))
        {
            return Ok(None);
        }
        // NB: Appends check that the prototype chain is the realm's default
        //     one, like pushes.
        self.embed(self.runtime.array_prototype);
        self.embed(self.runtime.object_prototype);
        let mut slow_paths = self.start_slow_paths(None)?;
        let object = self.read(object)?;
        let key = self.read_value(property)?;
        let value = self.read(value)?;
        let index = self.int32_or_slow_path(&mut slow_paths, key);
        let object = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, vec![object], true);
        let kinds = [ElementsKind::Packed, ElementsKind::Holey];
        let conditions = kinds.map(BranchCondition::ElementsKind);
        self.branch_on_cases(
            &mut slow_paths,
            object,
            &conditions,
            false,
            |builder, slow_paths, case, object| {
                let kind = kinds[case];
                let address = builder.cell_address(object);
                let index = if kind == ElementsKind::Packed {
                    builder.element_index_or_append(slow_paths, object, address, index, value)
                } else {
                    // NB: Filling a hole defines a new element, which takes an
                    //     extensible object. Filling the last one may make the
                    //     elements packed, which the slow path does.
                    builder.branch_to_slow_path(slow_paths, BranchCondition::Extensible, vec![address], true);
                    let index = builder.element_index_in_bounds(slow_paths, kind, address, index);
                    let length = builder.emit(Op::LoadElementsLength, vec![address], Some(Repr::Int32));
                    let one = builder.int32_constant(1);
                    let last = builder.int32_binary(BinaryOp::Sub, length, one);
                    builder.branch_to_slow_path(
                        slow_paths,
                        BranchCondition::Int32(Comparison::LessThan),
                        vec![index, last],
                        true,
                    )
                };
                builder.emit(Op::StoreElementAt { kind }, vec![address, index, value], None);
                builder.end_hot_path(slow_paths, Vec::new());
            },
        );
        self.join_hot_paths(slow_paths);
        Ok(Some(Flow::Continue))
    }
}
