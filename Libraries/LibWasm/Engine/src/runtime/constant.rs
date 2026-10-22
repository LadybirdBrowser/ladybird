/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Evaluating the constant expressions that initialize globals, tables, and segments.

use super::{FunctionAddress, GlobalAddress, Store, Trap, Value};
use crate::module::{ConstantExpression, Module};
use crate::opcode;
use crate::parser::parse_heap_type;
use crate::reader::Reader;
use crate::types::CompositeType;

pub fn evaluate(
    store: &mut Store,
    globals: &[GlobalAddress],
    functions: &[FunctionAddress],
    type_identities: &[u32],
    module: &Module,
    expression: &ConstantExpression,
) -> Result<Value, Trap> {
    let mut reader = Reader::with_range(
        &module.bytes,
        expression.code.start as usize,
        expression.code.end as usize,
    );
    let mut stack: Vec<Value> = Vec::new();
    let malformed = || Trap::Host("malformed constant expression".to_string());
    let read = |result: crate::Result<u64>| result.map_err(|_| malformed());
    loop {
        let opcode = reader.read_u8().map_err(|_| malformed())?;
        match opcode {
            opcode::END => break,
            opcode::I32_CONST => stack.push(Value::I32(reader.read_i32().map_err(|_| malformed())?)),
            opcode::I64_CONST => stack.push(Value::I64(reader.read_i64().map_err(|_| malformed())?)),
            opcode::F32_CONST => stack.push(Value::F32(reader.read_fixed_u32().map_err(|_| malformed())?)),
            opcode::F64_CONST => stack.push(Value::F64(reader.read_fixed_u64().map_err(|_| malformed())?)),
            opcode::REF_NULL => {
                parse_heap_type(&mut reader).map_err(|_| malformed())?;
                stack.push(Value::Ref(0));
            }
            opcode::REF_FUNC => {
                let index = read(reader.read_u32().map(u64::from))?;
                stack.push(Value::Ref(store.function_reference(functions[index as usize])));
            }
            opcode::GLOBAL_GET => {
                let index = read(reader.read_u32().map(u64::from))?;
                stack.push(store.global(globals[index as usize]).get());
            }
            opcode::I32_ADD | opcode::I32_SUB | opcode::I32_MUL => {
                let (Some(Value::I32(rhs)), Some(Value::I32(lhs))) = (stack.pop(), stack.pop()) else {
                    return Err(malformed());
                };
                stack.push(Value::I32(match opcode {
                    opcode::I32_ADD => lhs.wrapping_add(rhs),
                    opcode::I32_SUB => lhs.wrapping_sub(rhs),
                    _ => lhs.wrapping_mul(rhs),
                }));
            }
            opcode::I64_ADD | opcode::I64_SUB | opcode::I64_MUL => {
                let (Some(Value::I64(rhs)), Some(Value::I64(lhs))) = (stack.pop(), stack.pop()) else {
                    return Err(malformed());
                };
                stack.push(Value::I64(match opcode {
                    opcode::I64_ADD => lhs.wrapping_add(rhs),
                    opcode::I64_SUB => lhs.wrapping_sub(rhs),
                    _ => lhs.wrapping_mul(rhs),
                }));
            }
            opcode::PREFIX_SIMD => {
                read(reader.read_u32().map(u64::from))?;
                let bytes = reader.read_bytes(16).map_err(|_| malformed())?;
                let mut array = [0u8; 16];
                array.copy_from_slice(bytes);
                stack.push(Value::V128(u128::from_le_bytes(array)));
            }
            opcode::PREFIX_GC => {
                let sub_opcode = reader.read_u32().map_err(|_| malformed())?;
                let mut immediate = || reader.read_u32().map_err(|_| malformed());
                match sub_opcode {
                    opcode::gc::STRUCT_NEW | opcode::gc::STRUCT_NEW_DEFAULT => {
                        let type_index = immediate()?;
                        let identity = type_identities[type_index as usize];
                        let CompositeType::Struct(fields) = &store.types[identity as usize].composite else {
                            return Err(malformed());
                        };
                        let count = fields.len();
                        let values = if sub_opcode == opcode::gc::STRUCT_NEW {
                            stack.split_off(stack.len().checked_sub(count).ok_or_else(malformed)?)
                        } else {
                            fields
                                .iter()
                                .map(|field| Value::default_for(field.storage.unpacked()))
                                .collect()
                        };
                        let object = store.allocate_struct(identity, &values);
                        if object == 0 {
                            return Err(Trap::Host("out of memory".to_string()));
                        }
                        stack.push(Value::Ref(object));
                    }
                    opcode::gc::ARRAY_NEW | opcode::gc::ARRAY_NEW_DEFAULT | opcode::gc::ARRAY_NEW_FIXED => {
                        let type_index = immediate()?;
                        let identity = type_identities[type_index as usize];
                        let CompositeType::Array(element) = &store.types[identity as usize].composite else {
                            return Err(malformed());
                        };
                        let element = element.storage.unpacked();
                        // NB: The length of array.new is any u32, so its elements go into the array directly, which
                        //     checks its size first, rather than into a vector of that length.
                        let array = match sub_opcode {
                            opcode::gc::ARRAY_NEW_FIXED => {
                                let count = immediate()? as usize;
                                let values = stack.split_off(stack.len().checked_sub(count).ok_or_else(malformed)?);
                                store.allocate_array(identity, &values)
                            }
                            _ => {
                                let Some(Value::I32(length)) = stack.pop() else {
                                    return Err(malformed());
                                };
                                let value = if sub_opcode == opcode::gc::ARRAY_NEW {
                                    stack.pop().ok_or_else(malformed)?
                                } else {
                                    Value::default_for(element)
                                };
                                store.allocate_filled_array(identity, value, length as u32)
                            }
                        };
                        if array == 0 {
                            return Err(Trap::Host("out of memory".to_string()));
                        }
                        stack.push(Value::Ref(array));
                    }
                    opcode::gc::REF_I31 => {
                        let Some(Value::I32(value)) = stack.pop() else {
                            return Err(malformed());
                        };
                        stack.push(Value::Ref(super::gc::i31_reference(value)));
                    }
                    // A reference keeps its bits when it moves between the any and extern hierarchies.
                    opcode::gc::ANY_CONVERT_EXTERN | opcode::gc::EXTERN_CONVERT_ANY => {}
                    _ => return Err(malformed()),
                }
            }
            _ => return Err(Trap::Host("unsupported constant expression".to_string())),
        }
    }
    stack.pop().ok_or_else(malformed)
}
