/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the JavaScript of the engine itself can say that other code cannot: abstract operations that it calls by name,
//! and constants.

use crate::ast::Name;
use crate::bytecode::constant::AbstractOperationKind;
use crate::bytecode::constant::WellKnownSymbolKind;
use crate::bytecode::executable_data::ConstantValue;
use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;
use crate::u32_from_usize;
use crate::utf16::Utf16View;

use super::context::Generator;
use super::registers::ScopedOperand;

/// The abstract operations that are calls of an intrinsic function.
const INTRINSIC_OPERATIONS: &[(&[u16], AbstractOperationKind)] = &[
    (utf16!("ArraySpeciesCreate"), AbstractOperationKind::ArraySpeciesCreate),
    (utf16!("AsyncIteratorClose"), AbstractOperationKind::AsyncIteratorClose),
    (utf16!("GetMethod"), AbstractOperationKind::GetMethod),
    (utf16!("GetIteratorDirect"), AbstractOperationKind::GetIteratorDirect),
    (
        utf16!("GetIteratorFromMethod"),
        AbstractOperationKind::GetIteratorFromMethod,
    ),
    (utf16!("IteratorComplete"), AbstractOperationKind::IteratorComplete),
];

/// The abstract operations that have an instruction of their own.
const INSTRUCTION_OPERATIONS: &[&[u16]] = &[
    utf16!("IsCallable"),
    utf16!("IsConstructor"),
    utf16!("ToBoolean"),
    utf16!("ToObject"),
    utf16!("ToLength"),
    utf16!("ThrowIfNotObject"),
    utf16!("ThrowNotAFunction"),
    utf16!("ArgumentCount"),
    utf16!("ThrowTypeError"),
    utf16!("NewTypeError"),
    utf16!("NewObjectWithNoPrototype"),
    utf16!("NewArrayWithLength"),
    utf16!("CreateAsyncFromSyncIterator"),
    utf16!("CreateDataPropertyOrThrow"),
    utf16!("Call"),
];

impl Generator<'_> {
    /// Whether a call of the global `name` is an abstract operation.
    pub(super) fn is_builtin_operation(&self, name: Name) -> bool {
        let text = self.tree.name_text(name);
        INSTRUCTION_OPERATIONS.iter().any(|&operation| text == operation)
            || INTRINSIC_OPERATIONS.iter().any(|&(operation, _)| text == operation)
    }

    /// The text of the string constant `operand`, which an error of the engine gets as its message.
    fn intern_message(&mut self, operand: Operand) -> StringTableIndex {
        let Some(ConstantValue::String(text)) = self.context.constants.get(operand) else {
            panic!("the message of a builtin error is a string literal");
        };
        let text = text.clone();
        self.intern_string_text(Utf16View::of_string(&text))
    }

    /// The abstract operation `name` with `arguments`, which writes its value to `dst` if it has one.
    pub(super) fn builtin_operation(&mut self, name: Name, arguments: &[Operand], dst: Operand) {
        let text = self.tree.name_text(name);
        let is = |operation: &[u16]| text == operation;
        match arguments {
            [value] if is(utf16!("IsCallable")) => self.put(op::IsCallable { dst, value: *value }),
            [value] if is(utf16!("IsConstructor")) => self.put(op::IsConstructor { dst, value: *value }),
            [value] if is(utf16!("ToBoolean")) => self.put(op::ToBoolean { dst, value: *value }),
            [value] if is(utf16!("ToObject")) => self.put(op::ToObject { dst, value: *value }),
            [value] if is(utf16!("ToLength")) => self.put(op::ToLength { dst, value: *value }),
            [src] if is(utf16!("ThrowIfNotObject")) => self.put(op::ThrowIfNotObject { src: *src }),
            [src] if is(utf16!("ThrowNotAFunction")) => self.put(op::ThrowNotAFunction { src: *src }),
            [] if is(utf16!("ArgumentCount")) => self.put(op::GetArgumentCount { dst }),
            [message] if is(utf16!("ThrowTypeError")) => {
                let error_string = self.intern_message(*message);
                self.put(op::NewTypeError { dst, error_string });
                self.put(op::Throw { src: dst });
            }
            [message] if is(utf16!("NewTypeError")) => {
                let error_string = self.intern_message(*message);
                self.put(op::NewTypeError { dst, error_string });
            }
            [] if is(utf16!("NewObjectWithNoPrototype")) => self.put(op::NewObjectWithNoPrototype { dst }),
            [array_length] if is(utf16!("NewArrayWithLength")) => self.put(op::NewArrayWithLength {
                dst,
                array_length: *array_length,
            }),
            [iterator, next_method, done] if is(utf16!("CreateAsyncFromSyncIterator")) => {
                self.put(op::CreateAsyncFromSyncIterator {
                    dst,
                    iterator: *iterator,
                    next_method: *next_method,
                    done: *done,
                });
            }
            [object, property, value] if is(utf16!("CreateDataPropertyOrThrow")) => {
                self.put(op::CreateDataPropertyOrThrow {
                    object: *object,
                    property: *property,
                    value: *value,
                });
            }
            [callee, this_value, rest @ ..] if is(utf16!("Call")) => {
                self.put_builtin_call(dst, *callee, *this_value, rest);
            }
            _ => {
                let &(_, operation) = INTRINSIC_OPERATIONS
                    .iter()
                    .find(|&&(operation, _)| text == operation)
                    .expect("a builtin operation takes the arguments it is declared with");
                let callee = self.add_constant_abstract_operation(operation).operand();
                let undefined = self.context.constants.add_undefined();
                self.put_builtin_call(dst, callee, undefined, arguments);
            }
        }
    }

    fn put_builtin_call(&mut self, dst: Operand, callee: Operand, this_value: Operand, arguments: &[Operand]) {
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put(op::Call {
            value_feedback,
            call_feedback,
            dst,
            callee,
            this_value,
            argument_count: u32_from_usize(arguments.len()),
            expression_string: None,
            arguments,
        });
    }

    /// The constant that the global `name` stands for in the JavaScript of the engine, if it is one.
    pub(super) fn builtin_constant(&mut self, name: Name) -> Option<ScopedOperand> {
        if !self.builtin_abstract_operations_enabled {
            return None;
        }
        let text = self.tree.name_text(name);
        if text == utf16!("SYMBOL_ITERATOR") {
            return Some(self.add_constant_well_known_symbol(WellKnownSymbolKind::SymbolIterator));
        }
        if text == utf16!("SYMBOL_ASYNC_ITERATOR") {
            return Some(self.add_constant_well_known_symbol(WellKnownSymbolKind::SymbolAsyncIterator));
        }
        if text == utf16!("MAX_ARRAY_LIKE_INDEX") {
            return Some(self.add_constant_number(9007199254740991.0));
        }
        None
    }
}
