/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Table, https://webassembly.github.io/spec/js-api/#tables

use libjs_rust::api::{BigInt, Trace, Value, Vm};
use libwasm_engine::runtime::{TableAddress, TableInstance, Value as WasmValue};
use libwasm_engine::types::{AddressType as CoreAddressType, HeapType, Limits, RefType, TableType, ValType};

use crate::bindings::conversions::to_unsigned_long_enforcing_range;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::web_assembly::values::{default_webassembly_value, to_js_value, to_webassembly_value};
use crate::web_assembly::{Agent, EntityRef, host_object, implementation_of_value, set_host_object};

/// https://webassembly.github.io/spec/js-api/#enumdef-addresstype
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AddressType {
    I32,
    I64,
}

/// https://webassembly.github.io/spec/js-api/#enumdef-tablekind
#[derive(Clone, Copy)]
pub(crate) enum TableKind {
    Externref,
    Anyfunc,
}

/// https://webassembly.github.io/spec/js-api/#dictdef-tabledescriptor
pub(crate) struct TableDescriptor {
    pub(crate) element: TableKind,
    pub(crate) initial: Value,
    pub(crate) maximum: Option<Value>,
    pub(crate) address: Option<AddressType>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Table {
    // [[Table]]
    address: EntityRef<TableInstance>,
}

impl Wrappable for Table {}

// https://webassembly.github.io/spec/js-api/#addressvaluetou64
fn address_value_to_u64(vm: &Vm, value: Value, address64: bool) -> WebIdlResult<u64> {
    if address64 {
        let n = value.to_bigint(vm)?;

        return n
            .to_u64_if_exact()
            .ok_or_else(|| Exception::type_error("The table address value is out of range"));
    }

    Ok(u64::from(to_unsigned_long_enforcing_range(vm, value)?))
}

// https://webassembly.github.io/spec/js-api/#u64toaddressvalue
fn u64_to_address_value(vm: &Vm, value: u64, address64: bool) -> Value {
    if address64 {
        return Value::from_bigint(BigInt::create_from_u64(vm, value));
    }

    Value::from_f64(value as f64)
}

impl Table {
    // https://webassembly.github.io/spec/js-api/#dom-table-table
    pub(crate) fn construct(
        cx: &mut CallContext,
        descriptor: TableDescriptor,
        value: Option<Value>,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();

        // 1. Let elementtype be ToValueType(descriptor["element"]).
        // NB: The TableKind enumeration only has reference types.
        let element = RefType {
            nullable: true,
            heap: match descriptor.element {
                TableKind::Externref => HeapType::Extern,
                TableKind::Anyfunc => HeapType::Func,
            },
        };

        // 3. If descriptor["address"] exists, let addrtype be descriptor["address"]; otherwise, let addrtype be "i32".
        let address64 = descriptor.address == Some(AddressType::I64);

        // 4. Let initial be ? AddressValueToU64(descriptor["initial"], addrtype).
        let initial = address_value_to_u64(vm, descriptor.initial, address64)?;

        let maximum = descriptor
            .maximum
            .map(|maximum| address_value_to_u64(vm, maximum, address64))
            .transpose()?;

        if maximum.is_some_and(|maximum| maximum < initial) {
            return Err(Exception::range_error(
                "Maximum should not be less than initial in table type",
            ));
        }

        // 8. If value is missing,
        //     1. Let ref be DefaultValue(elementtype).
        //     2. Assert: ref is not error.
        // 9. Otherwise,
        //     1. Let ref be ? ToWebAssemblyValue(value, elementtype).
        let reference = Self::value_or_default(vm, ValType::Ref(element), value)?
            .expect("ToValueType gives types with a default value");

        // 11. Let (store, tableaddr) be table_alloc(store, type, ref). If allocation fails, throw a RangeError exception.
        // 12. Set the surrounding agent's associated store to store.
        let table = Agent::the(vm)
            .store()
            .allocate_table(
                TableType {
                    element,
                    limits: Limits {
                        min: initial,
                        max: maximum,
                        address_type: if address64 {
                            CoreAddressType::I64
                        } else {
                            CoreAddressType::I32
                        },
                        shared: false,
                    },
                },
                reference,
            )
            .ok_or_else(|| Exception::range_error("Wasm Table allocation failed"))?;

        // 13. Initialize this from tableaddr.
        Ok(Self::initialize(vm, table))
    }

    /// Returns `None` where DefaultValue(ty) is an error.
    fn value_or_default(vm: &Vm, ty: ValType, value: Option<Value>) -> WebIdlResult<Option<u64>> {
        Ok(match value {
            None => default_webassembly_value(vm, ty).map(reference_of),
            Some(value) => Some(reference_of(to_webassembly_value(vm, value, ty)?)),
        })
    }

    // https://webassembly.github.io/spec/js-api/#initialize-a-table-object
    fn initialize(vm: &Vm, table: TableAddress) -> Ref<Self> {
        // 2. Assert: map[tableaddr] doesn't exist.
        assert!(host_object::<()>(table.cell()).is_none());

        // 3. Set table.[[Table]] to tableaddr.
        let object = allocate(vm, |_| Self {
            address: EntityRef(table),
        });

        // 4. Set map[tableaddr] to table.
        set_host_object(table.cell(), object);
        object
    }

    /// The Table object of the table, https://webassembly.github.io/spec/js-api/#create-a-table-object
    pub(crate) fn from_address(vm: &Vm, table: TableAddress) -> Ref<Self> {
        if let Some(object) = host_object(table.cell()) {
            return object;
        }

        Self::initialize(vm, table)
    }

    pub(crate) fn from_value(value: Value) -> Option<Ref<Self>> {
        implementation_of_value(value)
    }

    pub(crate) fn address(&self) -> TableAddress {
        self.address.0
    }

    fn ty(&self, vm: &Vm) -> TableType {
        Agent::the(vm).store().table(self.address()).ty
    }

    fn is_address64(&self, vm: &Vm) -> bool {
        self.ty(vm).limits.address_type == CoreAddressType::I64
    }

    // https://webassembly.github.io/spec/js-api/#dom-table-grow
    pub(crate) fn grow(&self, cx: &mut CallContext, delta: Value, value: Option<Value>) -> WebIdlResult<Value> {
        let vm = cx.vm();

        // 3. Let initialSize be table_size(store, tableaddr).
        let ty = self.ty(vm);
        let address64 = ty.limits.address_type == CoreAddressType::I64;

        let delta = address_value_to_u64(vm, delta, address64)?;

        // 6. If value is missing,
        //     1. Let ref be DefaultValue(elementtype).
        //     2. If ref is error, throw a TypeError exception.
        // 7. Otherwise,
        //     1. Let ref be ? ToWebAssemblyValue(value, elementtype).
        let reference = Self::value_or_default(vm, ValType::Ref(ty.element), value)?
            .ok_or_else(|| Exception::type_error("The table's element type has no default value"))?;

        // 10. Set the surrounding agent's associated store to result.
        let initial_size = Agent::the(vm)
            .store()
            .table_mut(self.address())
            .grow(delta, reference)
            .ok_or_else(|| Exception::range_error("Failed to grow table"))?;

        Ok(u64_to_address_value(vm, initial_size, address64))
    }

    // https://webassembly.github.io/spec/js-api/#dom-table-length
    pub(crate) fn length(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        let vm = cx.vm();

        let length = Agent::the(vm).store().table(self.address()).elements.len() as u64;

        Ok(u64_to_address_value(vm, length, self.is_address64(vm)))
    }

    // https://webassembly.github.io/spec/js-api/#dom-table-get
    pub(crate) fn get(&self, cx: &mut CallContext, index: Value) -> WebIdlResult<Value> {
        let vm = cx.vm();

        // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
        let ty = self.ty(vm);

        if matches!(ty.element.heap, HeapType::Exn | HeapType::NoExn) {
            return Err(Exception::type_error("Cannot get an exnref table element"));
        }

        let index = address_value_to_u64(vm, index, ty.limits.address_type == CoreAddressType::I64)?;

        // 6. Let result be table_read(store, tableaddr, index64).
        // 7. If result is error, throw a RangeError exception.
        let reference = usize::try_from(index)
            .ok()
            .and_then(|index| {
                Agent::the(vm)
                    .store()
                    .table(self.address())
                    .elements
                    .get(index)
                    .copied()
            })
            .ok_or_else(|| Exception::range_error("Table element index out of range"))?;

        Ok(to_js_value(vm, WasmValue::Ref(reference), ValType::Ref(ty.element))?)
    }

    // https://webassembly.github.io/spec/js-api/#dom-table-set
    pub(crate) fn set(&self, cx: &mut CallContext, index: Value, value: Option<Value>) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
        let ty = self.ty(vm);

        if matches!(ty.element.heap, HeapType::Exn | HeapType::NoExn) {
            return Err(Exception::type_error("Cannot set an exnref table element"));
        }

        let index = address_value_to_u64(vm, index, ty.limits.address_type == CoreAddressType::I64)?;

        // 6. If value is missing,
        //     1. Let ref be DefaultValue(elementtype).
        //     2. If ref is error, throw a TypeError exception.
        // 7. Otherwise,
        //     1. Let ref be ? ToWebAssemblyValue(value, elementtype).
        let reference = Self::value_or_default(vm, ValType::Ref(ty.element), value)?
            .ok_or_else(|| Exception::type_error("The table's element type has no default value"))?;

        // 8. Let store be table_write(store, tableaddr, index64, ref).
        // 9. If store is error, throw a RangeError exception.
        // 10. Set the surrounding agent's associated store to store.
        let element = usize::try_from(index)
            .ok()
            .and_then(|index| Agent::the(vm).store().table_mut(self.address()).elements.get_mut(index))
            .ok_or_else(|| Exception::range_error("Table element index out of range"))?;
        *element = reference;
        Ok(())
    }
}

fn reference_of(value: WasmValue) -> u64 {
    match value {
        WasmValue::Ref(reference) => reference,
        _ => unreachable!("a table holds references"),
    }
}
