/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The builtins of the JS API, https://webassembly.github.io/spec/js-api/#builtins, and the imported strings,
//! https://webassembly.github.io/spec/js-api/#instantiate-imported-strings

use core::ffi::c_void;
use std::collections::HashSet;
use std::rc::Rc;

use ak::{Utf16String, Utf16StringUnits};
use libjs_rust::api::{Gc, Must, Object, PrimitiveString, PropertyKey, Value, Vm};
use libwasm_engine::module::{Import, ImportDescription};
use libwasm_engine::runtime::{FunctionAddress, HostFunction, Store, Trap, Value as WasmValue};
use libwasm_engine::types::{GlobalType, HeapType, RefType, ValType};

use crate::bindings::conversions::UsvString;
use crate::web_assembly::exported_function::create_exported_function;
use crate::web_assembly::instance::function_type_in_store;
use crate::web_assembly::values::{EXTERNREF, to_js_value, to_webassembly_value};
use crate::web_assembly::{Agent, CompiledModule};

const JS_STRING_BUILTIN_SET_NAME: &str = "js-string";
const JS_STRING_BUILTIN_SET_QUALIFIED_NAME: &str = "wasm:js-string";

/// The funcTypes of the builtins, each in a recursion group of its own, as the types of a module that has nothing
/// else. The store gives types the same identity wherever they are equivalent, so an import matches the funcType of a
/// builtin exactly when the identity of its type is the identity of that funcType.
#[derive(Clone, Copy)]
enum BuiltinTypeIndex {
    // (rec (type (array (mut i16)))).0
    #[allow(
        dead_code,
        reason = "the array type is the type of the other types' array parameters"
    )]
    ArrayType,
    // (rec (type (func (param externref) (result (ref extern))))).0
    ExternrefToRefExtern,
    // (rec (type (func (param externref) (result i32)))).0
    ExternrefToI32,
    // (rec (type (func (param (ref null arrayType) i32 i32) (result (ref extern))))).0
    ArrayI32I32ToRefExtern,
    // (rec (type (func (param externref (ref null arrayType) i32) (result i32)))).0
    ExternrefArrayI32ToI32,
    // (rec (type (func (param i32) (result (ref extern))))).0
    I32ToRefExtern,
    // (rec (type (func (param externref i32) (result i32)))).0
    ExternrefI32ToI32,
    // (rec (type (func (param externref externref) (result (ref extern))))).0
    ExternrefExternrefToRefExtern,
    // (rec (type (func (param externref i32 i32) (result (ref extern))))).0
    ExternrefI32I32ToRefExtern,
    // (rec (type (func (param externref externref) (result i32)))).0
    ExternrefExternrefToI32,
}

pub(crate) const BUILTIN_TYPES_MODULE_BYTES: [u8; 75] = [
    // Magic and version.
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, //
    // The type section: 65 bytes, 10 types.
    0x01, 0x41, 0x0a, //
    // array (mut i16)
    0x5e, 0x77, 0x01, //
    // func (param externref) (result (ref extern))
    0x60, 0x01, 0x6f, 0x01, 0x64, 0x6f, //
    // func (param externref) (result i32)
    0x60, 0x01, 0x6f, 0x01, 0x7f, //
    // func (param (ref null 0) i32 i32) (result (ref extern))
    0x60, 0x03, 0x63, 0x00, 0x7f, 0x7f, 0x01, 0x64, 0x6f, //
    // func (param externref (ref null 0) i32) (result i32)
    0x60, 0x03, 0x6f, 0x63, 0x00, 0x7f, 0x01, 0x7f, //
    // func (param i32) (result (ref extern))
    0x60, 0x01, 0x7f, 0x01, 0x64, 0x6f, //
    // func (param externref i32) (result i32)
    0x60, 0x02, 0x6f, 0x7f, 0x01, 0x7f, //
    // func (param externref externref) (result (ref extern))
    0x60, 0x02, 0x6f, 0x6f, 0x01, 0x64, 0x6f, //
    // func (param externref i32 i32) (result (ref extern))
    0x60, 0x03, 0x6f, 0x7f, 0x7f, 0x01, 0x64, 0x6f, //
    // func (param externref externref) (result i32)
    0x60, 0x02, 0x6f, 0x6f, 0x01, 0x7f,
];

/// The store-wide identity of the type at `type_index` of a module.
fn type_identity(vm: &Vm, module: &CompiledModule, type_index: u32) -> u32 {
    Agent::the(vm).store().module_type_identities_of(module)[type_index as usize]
}

type BuiltinSteps = fn(&Vm, &[WasmValue]) -> Result<WasmValue, Trap>;

struct Builtin {
    name: &'static str,
    function_type: BuiltinTypeIndex,
    steps: BuiltinSteps,
}

fn trap(message: &str) -> Trap {
    Trap::Host(message.to_string())
}

fn js_value_of(vm: &Vm, value: WasmValue) -> Value {
    to_js_value(vm, value, EXTERNREF).must()
}

fn string_reference(vm: &Vm, string: Gc<PrimitiveString>) -> WasmValue {
    to_webassembly_value(vm, Value::from_string(string), EXTERNREF).must()
}

fn string_reference_of_code_units(vm: &Vm, code_units: &[u16]) -> WasmValue {
    string_reference(vm, PrimitiveString::create(vm, Utf16String::from_utf16(code_units)))
}

fn i32_of(value: WasmValue) -> i32 {
    let WasmValue::I32(value) = value else {
        unreachable!("the funcType of the builtin has an i32 there");
    };
    value
}

fn unsigned_i32(value: WasmValue) -> u32 {
    i32_of(value) as u32
}

/// The code units of a string.
fn code_units_of(string: Gc<PrimitiveString>) -> Vec<u16> {
    let string = string.utf16_string();
    match string.as_units() {
        Utf16StringUnits::Ascii(characters) => characters.iter().map(|&character| u16::from(character)).collect(),
        Utf16StringUnits::Utf16(code_units) => code_units.to_vec(),
    }
}

/// The number of elements of an array, which is not null.
fn array_length(array: u64) -> u32 {
    // SAFETY: The array is the argument of a builtin whose funcType makes it a live array of i16s.
    unsafe { libwasm_engine::runtime::gc::array_length(array) }
}

// https://webassembly.github.io/spec/js-api/#abstract-opdef-unwrapstring
fn unwrap_string(vm: &Vm, v: WasmValue) -> Result<Gc<PrimitiveString>, Trap> {
    let value = js_value_of(vm, v);

    // 1. If v is not a String,
    if !value.is_string() {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return Err(trap("Expected a string"));
    }

    // 2. Return v
    Ok(value.as_string())
}

// https://webassembly.github.io/spec/js-api/#abstract-opdef-fromcharcode
fn from_char_code(v: i32) -> u16 {
    // 1. Assert: v is of type i32.
    // 2. Return ! Call(String.fromCharCode, undefined, « ToJSValue(v) »).
    v as u16
}

// https://webassembly.github.io/spec/js-api/#abstract-opdef-charcodeat
fn char_code_at(string: Gc<PrimitiveString>, index: u32) -> i32 {
    // 1. Assert: index is of type i32.
    // 2. Return ! Call(String.prototype.charCodeAt, string, « ToJSValue(index) »).
    i32::from(string.code_unit_at(index as usize))
}

#[allow(
    clippy::unnecessary_wraps,
    reason = "the steps of every builtin have the one type, and some can trap"
)]
mod js_string_builtins {
    use libjs_rust::api::{PrimitiveString, Vm};
    use libwasm_engine::runtime::{Trap, Value as WasmValue};

    use super::{
        array_length, code_units_of, i32_of, js_value_of, string_reference, string_reference_of_code_units, trap,
        unsigned_i32, unwrap_string,
    };

    // https://webassembly.github.io/spec/js-api/#js-string-cast
    pub(super) fn cast(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // 1. Return ? UnwrapString(v)
        unwrap_string(vm, arguments[0])?;
        Ok(arguments[0])
    }

    // https://webassembly.github.io/spec/js-api/#js-string-test
    pub(super) fn test(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // 1. If v is not a String,
        if !js_value_of(vm, arguments[0]).is_string() {
            // 1. Return 0.
            return Ok(WasmValue::I32(0));
        }

        // 2. Return 1.
        Ok(WasmValue::I32(1))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-fromcharcodearray
    pub(super) fn from_char_code_array(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        let WasmValue::Ref(array) = arguments[0] else {
            unreachable!("the funcType of the builtin has an array reference there");
        };
        // NB: The indices are unsigned, as everywhere else in WebAssembly.
        let start = unsigned_i32(arguments[1]);
        let end = unsigned_i32(arguments[2]);

        // 1. If array is null,
        if array == 0 {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The array is null"));
        }

        // 2. Let length be the number of elements in array.
        let length = array_length(array);

        // 3. If start > end or end > length,
        if start > end || end > length {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The range is out of the array's bounds"));
        }

        // 4. Let result be the empty string.
        let mut result = Vec::with_capacity((end - start) as usize);

        // 5. Let i be start.
        // 6. While i < end:
        for i in start..end {
            // 1. Let charCode be the value of the element stored at index i in array.
            // SAFETY: The array is a live array of i16s, and i is in its bounds.
            let char_code = unsafe { libwasm_engine::runtime::gc::array_element(array, i) } as i32;

            // 2. Let charCodeString be FromCharCode(charCode).
            // 3. Let result be the concatenation of result and charCodeString.
            // NB: FromCharCode() makes a string of one code unit, which is the code unit itself here.
            result.push(super::from_char_code(char_code));

            // 4. Set i to i + 1.
        }

        // 7. Return result.
        Ok(string_reference_of_code_units(vm, &result))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-intocharcodearray
    pub(super) fn into_char_code_array(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        let WasmValue::Ref(array) = arguments[1] else {
            unreachable!("the funcType of the builtin has an array reference there");
        };
        let start = unsigned_i32(arguments[2]);

        // 1. If array is null,
        if array == 0 {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The array is null"));
        }

        // 2. Let string be ? UnwrapString(string).
        let string = unwrap_string(vm, arguments[0])?;

        // 3. Let stringLength be the length of string.
        let string_length = string.length_in_utf16_code_units();

        // 4. Let arrayLength be the number of elements in array.
        let array_length = array_length(array);

        // 5. If start + stringLength > arrayLength,
        if u64::from(start) + string_length as u64 > u64::from(array_length) {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The string does not fit in the array"));
        }

        // 6. Let i be 0.
        // 7. While i < stringLength:
        for i in 0..string_length as u32 {
            // 1. Let charCode be CharCodeAt(string, i).
            let char_code = super::char_code_at(string, i);

            // 2. Set the element at index start + i in array to ToWebAssemblyValue(charCode, i32).
            // SAFETY: The array is a live array of i16s, and start + i is in its bounds.
            unsafe { libwasm_engine::runtime::gc::set_array_element(array, start + i, u64::from(char_code as u32)) };

            // 3. Set i to i + 1.
        }

        // 8. Return stringLength.
        Ok(WasmValue::I32(string_length as i32))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-fromcharcode
    pub(super) fn from_char_code(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // 1. Return FromCharCode(v).
        Ok(string_reference_of_code_units(
            vm,
            &[super::from_char_code(i32_of(arguments[0]))],
        ))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-fromcodepoint
    pub(super) fn from_code_point(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        let v = unsigned_i32(arguments[0]);

        // 1. If v > 0x10ffff,
        if v > 0x10ffff {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The code point is out of range"));
        }

        // 2. Return ! Call(String.fromCodePoint, undefined, « ToJSValue(v) »).
        // NB: A code point beyond the BMP is a surrogate pair, and a surrogate code point is a lone surrogate.
        if v < 0x10000 {
            return Ok(string_reference_of_code_units(vm, &[v as u16]));
        }
        let offset = v - 0x10000;
        Ok(string_reference_of_code_units(
            vm,
            &[0xd800 | (offset >> 10) as u16, 0xdc00 | (offset & 0x3ff) as u16],
        ))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-charcodeat
    pub(super) fn char_code_at(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        let index = unsigned_i32(arguments[1]);

        // 1. Let string be ? UnwrapString(string).
        let string = unwrap_string(vm, arguments[0])?;

        // 2. Let length be the length of string.
        let length = string.length_in_utf16_code_units();

        // 3. If index >= length,
        if index as usize >= length {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The index is out of the string's bounds"));
        }

        // 4. Return CharCodeAt(string, index).
        Ok(WasmValue::I32(super::char_code_at(string, index)))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-codepointat
    pub(super) fn code_point_at(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        let index = unsigned_i32(arguments[1]) as usize;

        // 1. Let string be ? UnwrapString(string).
        let string = unwrap_string(vm, arguments[0])?;

        // 2. Let length be the length of string.
        let length = string.length_in_utf16_code_units();

        // 3. If index >= length,
        if index >= length {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("The index is out of the string's bounds"));
        }

        // 4. Return ! Call(String.prototype.codePointAt, string, « ToJSValue(index) »).
        // NB: A leading surrogate that a trailing one follows is the code point of the pair, and any other code unit
        //     is the code point of its value.
        let first = string.code_unit_at(index);
        if (0xd800..=0xdbff).contains(&first) && index + 1 < length {
            let second = string.code_unit_at(index + 1);
            if (0xdc00..=0xdfff).contains(&second) {
                let code_point = 0x10000 + ((u32::from(first) - 0xd800) << 10) + (u32::from(second) - 0xdc00);
                return Ok(WasmValue::I32(code_point as i32));
            }
        }
        Ok(WasmValue::I32(i32::from(first)))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-length
    pub(super) fn length(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // 1. Let string be ? UnwrapString(v).
        let string = unwrap_string(vm, arguments[0])?;

        // 2. Return the length of string.
        Ok(WasmValue::I32(string.length_in_utf16_code_units() as i32))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-concat
    pub(super) fn concat(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // 1. Let first be ? UnwrapString(first).
        let first = unwrap_string(vm, arguments[0])?;

        // 2. Let second be ? UnwrapString(second).
        let second = unwrap_string(vm, arguments[1])?;

        // 3. Return ! Call(String.prototype.concat, first, « second »).
        let mut result = code_units_of(first);
        result.extend(code_units_of(second));
        Ok(string_reference_of_code_units(vm, &result))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-substring
    pub(super) fn substring(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // NB: The indices are unsigned, as everywhere else in WebAssembly.
        let start = unsigned_i32(arguments[1]) as usize;
        let end = unsigned_i32(arguments[2]) as usize;

        // 1. Let string be ? UnwrapString(string).
        let string = unwrap_string(vm, arguments[0])?;

        // 2. Let length be the length of string.
        let length = string.length_in_utf16_code_units();

        // 3. If start > end or start > length,
        if start > end || start > length {
            // 1. Return the empty string.
            return Ok(string_reference(vm, vm.empty_string()));
        }

        // 4. Return ! Call(String.prototype.substring, string, « ToJSValue(start), ToJSValue(end) »).
        // NB: start <= end here, and String.prototype.substring clamps end to the length.
        let clamped_end = end.min(length);
        Ok(string_reference(
            vm,
            PrimitiveString::create_from_substring(vm, string, start, clamped_end - start),
        ))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-equals
    pub(super) fn equals(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        let first = js_value_of(vm, arguments[0]);
        let second = js_value_of(vm, arguments[1]);

        // 1. If first is not null and first is not a String,
        if !first.is_null() && !first.is_string() {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("Expected a string or null"));
        }

        // 2. If second is not null and second is not a String,
        if !second.is_null() && !second.is_string() {
            // 1. Throw a RuntimeError exception as if a trap was executed.
            return Err(trap("Expected a string or null"));
        }

        // 3. If IsStrictlyEqual(first, second) is true,
        let strictly_equal = if first.is_null() || second.is_null() {
            first.is_null() && second.is_null()
        } else {
            first.as_string().utf16_string() == second.as_string().utf16_string()
        };
        if strictly_equal {
            // 1. Return 1.
            return Ok(WasmValue::I32(1));
        }

        // 4. Return 0.
        Ok(WasmValue::I32(0))
    }

    // https://webassembly.github.io/spec/js-api/#js-string-compare
    pub(super) fn compare(vm: &Vm, arguments: &[WasmValue]) -> Result<WasmValue, Trap> {
        // 1. Let first be ? UnwrapString(first).
        let first = code_units_of(unwrap_string(vm, arguments[0])?);

        // 2. Let second be ? UnwrapString(second).
        let second = code_units_of(unwrap_string(vm, arguments[1])?);

        // 3. If IsStrictlyEqual(first, second) is true,
        if first == second {
            // 1. Return 0.
            return Ok(WasmValue::I32(0));
        }

        // 4. If ! IsLessThan(first, second, true) is true,
        // NB: IsLessThan() compares two strings by their code units.
        if first < second {
            // 1. Return -1.
            return Ok(WasmValue::I32(-1));
        }

        // 5. Return 1.
        Ok(WasmValue::I32(1))
    }
}

// https://webassembly.github.io/spec/js-api/#string-builtins
const JS_STRING_BUILTINS: [Builtin; 13] = [
    Builtin {
        name: "cast",
        function_type: BuiltinTypeIndex::ExternrefToRefExtern,
        steps: js_string_builtins::cast,
    },
    Builtin {
        name: "test",
        function_type: BuiltinTypeIndex::ExternrefToI32,
        steps: js_string_builtins::test,
    },
    Builtin {
        name: "fromCharCodeArray",
        function_type: BuiltinTypeIndex::ArrayI32I32ToRefExtern,
        steps: js_string_builtins::from_char_code_array,
    },
    Builtin {
        name: "intoCharCodeArray",
        function_type: BuiltinTypeIndex::ExternrefArrayI32ToI32,
        steps: js_string_builtins::into_char_code_array,
    },
    Builtin {
        name: "fromCharCode",
        function_type: BuiltinTypeIndex::I32ToRefExtern,
        steps: js_string_builtins::from_char_code,
    },
    // INTEROP: The funcType of fromCodePoint has a (ref extern) result in other implementations, where the spec has an
    //          externref result.
    Builtin {
        name: "fromCodePoint",
        function_type: BuiltinTypeIndex::I32ToRefExtern,
        steps: js_string_builtins::from_code_point,
    },
    Builtin {
        name: "charCodeAt",
        function_type: BuiltinTypeIndex::ExternrefI32ToI32,
        steps: js_string_builtins::char_code_at,
    },
    Builtin {
        name: "codePointAt",
        function_type: BuiltinTypeIndex::ExternrefI32ToI32,
        steps: js_string_builtins::code_point_at,
    },
    Builtin {
        name: "length",
        function_type: BuiltinTypeIndex::ExternrefToI32,
        steps: js_string_builtins::length,
    },
    Builtin {
        name: "concat",
        function_type: BuiltinTypeIndex::ExternrefExternrefToRefExtern,
        steps: js_string_builtins::concat,
    },
    Builtin {
        name: "substring",
        function_type: BuiltinTypeIndex::ExternrefI32I32ToRefExtern,
        steps: js_string_builtins::substring,
    },
    Builtin {
        name: "equals",
        function_type: BuiltinTypeIndex::ExternrefExternrefToI32,
        steps: js_string_builtins::equals,
    },
    Builtin {
        name: "compare",
        function_type: BuiltinTypeIndex::ExternrefExternrefToI32,
        steps: js_string_builtins::compare,
    },
];

pub(crate) fn refers_to_a_builtin_set(builtin_set_name: &UsvString) -> bool {
    builtin_set_name.to_utf8() == JS_STRING_BUILTIN_SET_NAME
}

// https://webassembly.github.io/spec/js-api/#get-the-builtins-for-a-builtin-set
fn get_the_builtins_for_a_builtin_set(builtin_set_name: &UsvString) -> &'static [Builtin] {
    // 1. Return a list of (name, funcType, steps) for the set with name builtinSetName defined within this section.
    assert!(refers_to_a_builtin_set(builtin_set_name));
    &JS_STRING_BUILTINS
}

// https://webassembly.github.io/spec/js-api/#validate-builtin-set-names
fn validate_builtin_set_names(builtin_set_names: &[UsvString]) -> bool {
    // 1. If builtinSetNames contains any duplicates, return false.
    let mut seen_names = HashSet::new();
    if !builtin_set_names
        .iter()
        .all(|builtin_set_name| seen_names.insert(builtin_set_name.to_utf8()))
    {
        return false;
    }

    // 2. Return true.
    true
}

// https://webassembly.github.io/spec/js-api/#find-a-builtin
fn find_a_builtin(import: &Import, builtin_set_names: &[UsvString]) -> Option<&'static Builtin> {
    // 1. Assert: validate builtin set names builtinSetNames is true.
    assert!(validate_builtin_set_names(builtin_set_names));

    // 2. Let importModuleName be import[0].
    let import_module_name = &import.module;

    // 3. Let importName be import[1].
    let import_name = &import.name;

    // 4. For each builtinSetName of builtinSetNames,
    for builtin_set_name in builtin_set_names {
        // 1. If builtinSetName does not refer to a builtin set, then continue.
        if !refers_to_a_builtin_set(builtin_set_name) {
            continue;
        }

        // 2. Let builtinSetQualifiedName be builtinSetName prefixed with "wasm:".
        let builtin_set_qualified_name = JS_STRING_BUILTIN_SET_QUALIFIED_NAME;

        // 3. If importModuleName equals builtinSetQualifiedName,
        if import_module_name == builtin_set_qualified_name {
            // 1. Let builtins be the result of get the builtins for a builtin set builtinSetName.
            let builtins = get_the_builtins_for_a_builtin_set(builtin_set_name);

            // 2. For each builtin of builtins,
            for builtin in builtins {
                // 1. Let builtinName be builtin[0].
                // 2. If importName equals builtinName, return (builtinSetName, builtin).
                if import_name == builtin.name {
                    return Some(builtin);
                }
            }
        }
    }

    // 5. Return null.
    None
}

/// Whether finding a builtin for the import and builtinSetNames does not return null.
pub(crate) fn import_refers_to_a_builtin(import: &Import, builtin_set_names: &[UsvString]) -> bool {
    find_a_builtin(import, builtin_set_names).is_some()
}

// https://webassembly.github.io/spec/js-api/#validate-an-import-for-builtins
fn validate_an_import_for_builtins(
    vm: &Vm,
    module: &CompiledModule,
    import: &Import,
    builtin_set_names: &[UsvString],
) -> bool {
    // 1. Assert: validate builtin set names builtinSetNames is true.
    assert!(validate_builtin_set_names(builtin_set_names));

    // 2. Let maybeBuiltin be the result of finding a builtin for import and builtinSetNames.
    let maybe_builtin = find_a_builtin(import, builtin_set_names);

    // 3. If maybeBuiltin is null, return true.
    let Some(maybe_builtin) = maybe_builtin else {
        return true;
    };

    // 4. Let importExternType be import[2].
    let import_extern_type = &import.description;

    // 5. Let builtinFuncType be maybeBuiltin[1][1].
    let builtin_func_type = maybe_builtin.function_type;

    // 6. Let builtinExternType be func |builtinFuncType|.
    // 7. Return match_externtype(builtinExternType, importExternType).
    // NB: The funcType of a builtin is final and has no supertypes, so it only matches the import's type if both are
    //     the same type.
    let ImportDescription::Function(type_index) = *import_extern_type else {
        return false;
    };
    let builtin_types_module = Agent::the(vm).builtin_types_module();
    type_identity(vm, module, type_index) == type_identity(vm, builtin_types_module, builtin_func_type as u32)
}

// https://webassembly.github.io/spec/js-api/#validate-builtins-and-imported-string-for-a-webassembly-module
pub(crate) fn validate_builtins_and_imported_strings(
    vm: &Vm,
    module: &CompiledModule,
    builtin_set_names: &[UsvString],
    imported_string_module: Option<&UsvString>,
) -> bool {
    // 1. If validating builtin set names for builtinSetNames is false, return false.
    if !validate_builtin_set_names(builtin_set_names) {
        return false;
    }

    let imported_string_module = imported_string_module.map(UsvString::to_utf8);

    // 2. For each import of module_imports(module),
    for import in &module.imports {
        // 1. If importedStringModule is not null and import[0] equals importedStringModule,
        if imported_string_module.as_ref() == Some(&import.module) {
            // 1. Let importExternType be import[2].
            let import_extern_type = &import.description;

            // 2. Let stringExternType be global ref extern.
            // 3. If match_externtype(stringExternType, importExternType) is false, return false
            // NB: An immutable global matches if (ref extern) is a subtype of the import's value type, which then is
            //     (ref extern) or externref.
            if !matches!(
                import_extern_type,
                ImportDescription::Global(GlobalType {
                    value: ValType::Ref(RefType {
                        heap: HeapType::Extern,
                        ..
                    }),
                    mutable: false,
                })
            ) {
                return false;
            }
        }
        // 2. Else,
        else {
            // 1. If validating an import for builtins with import and builtinSetNames is false, return false.
            if !validate_an_import_for_builtins(vm, module, import, builtin_set_names) {
                return false;
            }
        }
    }

    // 3. Return true.
    true
}

/// The steps of a builtin's host function, and the index of the builtin in its set.
struct BuiltinFunction {
    steps: BuiltinSteps,
    index: u32,
}

impl HostFunction for BuiltinFunction {
    fn call(&self, _: &mut Store, arguments: &[WasmValue]) -> Result<Vec<WasmValue>, Trap> {
        let vm = Agent::current().vm();
        (self.steps)(vm, arguments).map(|result| vec![result])
    }

    fn user_data(&self) -> *mut c_void {
        core::ptr::from_ref(&self.index).cast_mut().cast()
    }
}

// https://webassembly.github.io/spec/js-api/#create-a-builtin-function
fn create_a_builtin_function(
    vm: &Vm,
    function_type: BuiltinTypeIndex,
    steps: BuiltinSteps,
    index: u32,
) -> FunctionAddress {
    // 1. Let hostfunc be a host function which executes steps when called.
    let host_function = Rc::new(BuiltinFunction { steps, index });

    // 2. Let store be the surrounding agent's associated store.
    // 3. Let (store, funcaddr) be func_alloc(store, funcType, hostfunc).
    // 4. Set the surrounding agent's associated store to store.
    // 5. Return funcaddr.
    // NB: The index is the builtin's position in its set, which names the Exported Function made for it.
    let agent = Agent::the(vm);
    let ty = function_type_in_store(vm, agent.builtin_types_module(), function_type as u32);
    agent.store().allocate_host_function(ty, host_function)
}

// https://webassembly.github.io/spec/js-api/#instantiate-a-builtin-set
pub(crate) fn instantiate_a_builtin_set(vm: &Vm, builtin_set_name: &UsvString) -> Gc<Object> {
    let realm = vm.current_realm().expect("instantiating a builtin set runs in a realm");

    // 1. Let builtins be the result of get the builtins for a builtin set builtinSetName.
    let builtins = get_the_builtins_for_a_builtin_set(builtin_set_name);

    // 2. Let exportsObject be OrdinaryObjectCreate(null).
    let exports_object = Object::create(vm, realm, None);

    // 3. For each (name, funcType, steps) of builtins,
    for (index, builtin) in builtins.iter().enumerate() {
        // 1. Let funcaddr be the result of create a builtin function with funcType and steps.
        let funcaddr = create_a_builtin_function(vm, builtin.function_type, builtin.steps, index as u32);

        // 2. Let func be the result of creating a new Exported Function from funcaddr.
        let func = create_exported_function(vm, realm, funcaddr);

        // 3. Let value be func.
        let value = Value::from_object(func.upcast::<Object>());

        // 4. Let status be ! CreateDataProperty(exportsObject, name, value).
        let status = exports_object
            .create_data_property(vm, &PropertyKey::from_utf8(builtin.name), value, None, None)
            .must();

        // 5. Assert: status is true.
        assert!(status);
    }

    // 4. Return exportsObject.
    exports_object
}

// https://webassembly.github.io/spec/js-api/#instantiate-imported-strings
pub(crate) fn instantiate_imported_strings(
    vm: &Vm,
    module: &CompiledModule,
    imported_string_module: &UsvString,
) -> Gc<Object> {
    let realm = vm
        .current_realm()
        .expect("instantiating imported strings runs in a realm");

    // 1. Assert: importedStringModule is not null.
    let imported_string_module = imported_string_module.to_utf8();

    // 2. Let exportsObject be OrdinaryObjectCreate(null).
    let exports_object = Object::create(vm, realm, None);

    // 3. For each (moduleName, componentName, externtype) of module_imports(module),
    for import in &module.imports {
        // 1. If moduleName does not equal importedStringModule, then continue.
        if import.module != imported_string_module {
            continue;
        }

        // 2. Let stringConstant be componentName.
        let string_constant = Utf16String::from_utf8(&import.name);

        // 3. Let status be ! CreateDataProperty(exportsObject, stringConstant, stringConstant).
        let value = Value::from_string(PrimitiveString::create(vm, string_constant.clone()));
        let status = exports_object
            .create_data_property(vm, &PropertyKey::from(&string_constant), value, None, None)
            .must();

        // 4. Assert: status is true.
        assert!(status);
    }

    // 4. Return exportsObject.
    exports_object
}
