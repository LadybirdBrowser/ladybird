/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The conversions between JavaScript values and WebAssembly values, https://webassembly.github.io/spec/js-api/#tojsvalue
//!
//! References are encoded so that their kind shows in their low bits: an i31 has bit 0 set, and a host value has bits
//! 1:0 equal to 0b10. A host value is the address of a cell with bit 1 set, which keeps the cell alive wherever
//! WebAssembly keeps the reference. Functions, structs, arrays and exceptions are the addresses of their cells.

use core::ptr::NonNull;

use libjs_rust::api::{
    Array, BigInt, CellHeader, CellKind, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, Gc, Object,
    ThrowCompletionOr, Trace, Value, Vm,
};
use libwasm_engine::runtime::Value as WasmValue;
use libwasm_engine::types::{HeapType, RefType, ValType};

use crate::web_assembly::Agent;
use crate::web_assembly::exported_function::{create_exported_function, exported_function_address};
use crate::web_assembly::gc_object::{create_exported_gc_object, exported_gc_object_address};

const I31_REFERENCE_BIT: u64 = 0b01;
const HOST_REFERENCE_TAG: u64 = 0b10;
const REFERENCE_TAG_MASK: u64 = 0b11;

/// A JavaScript value that is not a cell, boxed so that WebAssembly can refer to it as a host value.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ExternValueBox {
    value: Value,
}

static EXTERN_VALUE_BOX_CLASS: EmbedderClass<ExternValueBox> = EmbedderClass::new("ExternValueBox");

impl EmbedderValue for ExternValueBox {
    const CLASS: &'static EmbedderClass<Self> = &EXTERN_VALUE_BOX_CLASS;
}

pub(crate) const EXTERNREF: ValType = ValType::Ref(RefType {
    nullable: true,
    heap: HeapType::Extern,
});

fn host_reference_of(vm: &Vm, value: Value) -> u64 {
    let cell = if value.is_cell() {
        value.as_cell().as_ptr().cast::<u8>()
    } else {
        // NB: A value gets the same box while references to the box keep it alive, so that it converts to the same
        //     reference.
        let agent = Agent::the(vm);
        let value_box = agent.extern_value_box(value).unwrap_or_else(|| {
            let value_box = EXTERN_VALUE_BOX_CLASS.allocate(vm, |_| ExternValueBox { value });
            agent.set_extern_value_box(value, value_box);
            value_box
        });
        value_box.as_ptr().cast::<u8>()
    };
    cell as u64 | HOST_REFERENCE_TAG
}

fn value_of_host_reference(reference: u64) -> Value {
    let address = (reference & !REFERENCE_TAG_MASK) as *mut CellHeader;
    // SAFETY: A host reference is the address of a live cell, which starts with its header.
    let kind = unsafe { (*address).kind };
    // SAFETY: The kind of the cell says what it is.
    unsafe {
        match kind {
            CellKind::Object => Value::from_object(Gc::from_non_null(NonNull::new_unchecked(address.cast::<Object>()))),
            CellKind::PrimitiveString => Value::from_string(Gc::from_non_null(NonNull::new_unchecked(address.cast()))),
            CellKind::Symbol => Value::from_symbol(Gc::from_non_null(NonNull::new_unchecked(address.cast()))),
            CellKind::BigInt => Value::from_bigint(Gc::from_non_null(NonNull::new_unchecked(address.cast()))),
            CellKind::Other => {
                let value_box: Gc<EmbedderCell<ExternValueBox>> =
                    Gc::from_non_null(NonNull::new_unchecked(address.cast()));
                value_box.value
            }
            CellKind::Accessor => unreachable!("an accessor is no JavaScript value"),
        }
    }
}

fn i31_reference_of(value: Value) -> Option<u64> {
    const I31_MINIMUM: f64 = -((1 << 30) as f64);
    const I31_MAXIMUM: f64 = ((1 << 30) - 1) as f64;
    if !value.is_number() {
        return None;
    }
    let number = value.as_f64();
    if !(I31_MINIMUM..=I31_MAXIMUM).contains(&number)
        || number != number.trunc()
        || (number == 0.0 && number.is_sign_negative())
    {
        return None;
    }
    Some((u64::from((number as i32 as u32) & 0x7fff_ffff) << 1) | I31_REFERENCE_BIT)
}

fn value_of_i31_reference(reference: u64) -> i32 {
    // The value is in bits 31:1, so an arithmetic shift of the low 32 bits sign-extends it.
    (reference as u32 as i32) >> 1
}

/// The internal form of a JavaScript value as a reference of the any hierarchy: an i31 reference for a Number in its
/// range, the struct or array of an Exported GC Object, and otherwise a host reference.
fn internalized_reference_of(vm: &Vm, value: Value) -> u64 {
    if let Some(reference) = i31_reference_of(value) {
        return reference;
    }
    if value.is_object()
        && let Some(address) = exported_gc_object_address(value.as_object())
    {
        return address;
    }
    host_reference_of(vm, value)
}

/// Whether a reference type is of the func hierarchy, whose references are functions.
fn is_function_reference_type(vm: &Vm, heap: HeapType) -> bool {
    match heap {
        HeapType::Concrete(identity) => Agent::the(vm).store().is_function_type(identity),
        HeapType::Func | HeapType::NoFunc => true,
        _ => false,
    }
}

fn type_error<T>(vm: &Vm, message: &str) -> ThrowCompletionOr<T> {
    vm.throw_completion_with_message(ErrorKind::TypeError, message.to_string())
}

// https://webassembly.github.io/spec/js-api/#towebassemblyvalue
fn to_webassembly_reference(vm: &Vm, value: Value, ty: RefType) -> ThrowCompletionOr<u64> {
    // If v is null: if the type is nullable, return ref.null of its heap type, and otherwise throw a TypeError.
    if value.is_null() {
        if !ty.nullable {
            return type_error(vm, "Cannot convert null to a non-nullable reference");
        }
        return Ok(0);
    }

    match ty.heap {
        HeapType::Exn | HeapType::NoExn => {
            return type_error(vm, "Cannot convert a JavaScript value to an exception reference");
        }
        HeapType::NoFunc | HeapType::NoExtern | HeapType::None | HeapType::Bottom => {
            return type_error(vm, "Only null converts to a bottom reference type");
        }
        // NB: An externref keeps the bits of the anyref it externalizes.
        HeapType::Extern => return Ok(internalized_reference_of(vm, value)),
        _ => {}
    }

    // If the type is of the func hierarchy, only an Exported Function of a matching type converts, to its function.
    if is_function_reference_type(vm, ty.heap) {
        if value.is_function()
            && let Some(address) = exported_function_address(value.as_object())
        {
            let store = Agent::the(vm).store();
            let reference = store.function_reference(address);
            if store.reference_matches(reference, ty.heap) {
                return Ok(reference);
            }
        }
        return type_error(vm, "Cannot convert the value to a function reference of this type");
    }

    // Otherwise the type is of the any hierarchy: the internal form of v converts if it matches the type.
    let reference = internalized_reference_of(vm, value);
    if !Agent::the(vm).store().reference_matches(reference, ty.heap) {
        return type_error(vm, "Cannot convert the value to a reference of this type");
    }
    Ok(reference)
}

/// ToWebAssemblyValue(v, type), https://webassembly.github.io/spec/js-api/#towebassemblyvalue, for a type in the store's
/// terms (see Store::module_type_identities()).
pub(crate) fn to_webassembly_value(vm: &Vm, value: Value, ty: ValType) -> ThrowCompletionOr<WasmValue> {
    Ok(match ty {
        ValType::I64 => WasmValue::I64(value.to_bigint_int64(vm)?),
        ValType::I32 => WasmValue::I32(value.to_i32(vm)?),
        ValType::F64 => WasmValue::F64(value.to_double(vm)?.to_bits()),
        ValType::F32 => WasmValue::F32((value.to_double(vm)? as f32).to_bits()),
        ValType::V128 => return type_error(vm, "Cannot convert a javascript value to a vector value"),
        ValType::Ref(ty) => WasmValue::Ref(to_webassembly_reference(vm, value, ty)?),
    })
}

/// DefaultValue(valuetype), https://webassembly.github.io/spec/js-api/#defaultvalue
pub(crate) fn default_webassembly_value(vm: &Vm, ty: ValType) -> WasmValue {
    // 1. If valuetype equals externref, return ToWebAssemblyValue(undefined, valuetype).
    if let ValType::Ref(RefType {
        heap: HeapType::Extern, ..
    }) = ty
    {
        return WasmValue::Ref(host_reference_of(vm, Value::UNDEFINED));
    }

    // 2. Return DefaultValue(valuetype).
    WasmValue::default_for(ty)
}

/// ToJSValue(w), https://webassembly.github.io/spec/js-api/#tojsvalue, for a value of the type, which is in the store's
/// terms.
pub(crate) fn to_js_value(vm: &Vm, value: WasmValue, ty: ValType) -> ThrowCompletionOr<Value> {
    let reference = match (value, ty) {
        (WasmValue::I64(value), _) => return Ok(Value::from_bigint(BigInt::create_from_i64(vm, value))),
        (WasmValue::I32(value), _) => return Ok(Value::from_i32(value)),
        (WasmValue::F64(bits), _) => return Ok(Value::from_f64(f64::from_bits(bits))),
        (WasmValue::F32(bits), _) => return Ok(Value::from_f64(f64::from(f32::from_bits(bits)))),
        (WasmValue::V128(_), _) => return type_error(vm, "Cannot convert a vector value to a javascript value"),
        (WasmValue::Ref(reference), _) => reference,
    };
    let ValType::Ref(ty) = ty else {
        unreachable!("a reference has a reference type");
    };

    if matches!(ty.heap, HeapType::Exn | HeapType::NoExn) {
        return type_error(vm, "Cannot convert an exception reference to a javascript value");
    }

    // 1. If w is of the form ref.null t, return null.
    if reference == 0 {
        return Ok(Value::NULL);
    }

    // 2. If w is of the form ref.func funcaddr, return the result of creating a new Exported Function from funcaddr.
    if is_function_reference_type(vm, ty.heap) {
        let function = Agent::the(vm)
            .store()
            .function_from_reference(reference)
            .expect("a function reference that is not null is a function");
        let realm = vm.current_realm().expect("a conversion runs in a realm");
        return Ok(Value::from_object(
            create_exported_function(vm, realm, function).upcast::<Object>(),
        ));
    }

    // 3. If w is of the form ref.i31 u31, return 𝔽(signed_31(u31)).
    if reference & I31_REFERENCE_BIT != 0 {
        return Ok(Value::from_i32(value_of_i31_reference(reference)));
    }

    // 4. If w is of the form ref.host hostaddr, return the result of retrieving a host value from hostaddr.
    // NB: An externref of a host value keeps the bits of the host reference.
    if reference & REFERENCE_TAG_MASK == HOST_REFERENCE_TAG {
        return Ok(value_of_host_reference(reference));
    }

    // 5. If w is of the form ref.struct structaddr or ref.array arrayaddr, return the result of creating a new Exported
    //    GC Object from it.
    Ok(Value::from_object(create_exported_gc_object(vm, reference)))
}

/// The JavaScript value of the results of a call: undefined for none, the one result, or an array of them.
pub(crate) fn results_to_js_value(vm: &Vm, results: &[WasmValue], types: &[ValType]) -> ThrowCompletionOr<Value> {
    match results {
        [] => Ok(Value::UNDEFINED),
        [result] => to_js_value(vm, *result, types[0]),
        _ => {
            let values = libjs_rust::api::MarkedVec::with_capacity(vm, results.len());
            for (result, ty) in results.iter().zip(types) {
                values.push(to_js_value(vm, *result, *ty)?);
            }
            let realm = vm.current_realm().expect("a conversion runs in a realm");
            Ok(Value::from_object(
                Array::create_from(vm, realm, &values.values()).upcast::<Object>(),
            ))
        }
    }
}
