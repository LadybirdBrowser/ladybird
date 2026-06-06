/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Table.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Table);

static Wasm::ValueType table_kind_to_value_type(Bindings::TableKind kind)
{
    switch (kind) {
    case Bindings::TableKind::Externref:
        return Wasm::ValueType { Wasm::ValueType::ExternReference };
    case Bindings::TableKind::Anyfunc:
        return Wasm::ValueType { Wasm::ValueType::FunctionReference };
    }

    VERIFY_NOT_REACHED();
}

// https://webassembly.github.io/spec/js-api/#addressvaluetou64
static WebIDL::ExceptionOr<u64> address_value_to_u64(JS::VM& vm, JS::Value value, Wasm::AddressType address_type)
{
    if (address_type == Wasm::AddressType::I64) {
        // 1. If addrtype is i64, then:
        // 1.1. Let n be ? ToBigInt(value).
        auto bigint = TRY(value.to_bigint(vm));

        // 1.2. If n < 0 or n >= 2^64, throw a TypeError exception.
        if (bigint->big_integer().is_negative())
            return vm.throw_completion<JS::TypeError>("Table size must be non-negative"sv);

        auto string = TRY_OR_THROW_OOM(vm, bigint->big_integer().to_base(10));
        auto number = string.to_number<u64>();
        if (!number.has_value())
            return vm.throw_completion<JS::TypeError>("Table size is too large"sv);

        // 1.3. Return n.
        return *number;
    }

    // 2. Otherwise:
    // 2.1. Assert: addrtype is i32.
    // 2.2. Let n be ? Web IDL's convert a JavaScript value to an IDL value of type [EnforceRange] unsigned long, given value.
    auto n = TRY(WebIDL::convert_to_int<WebIDL::UnsignedLong>(vm, value, WebIDL::EnforceRange::Yes));

    // 2.3. Return n.
    return n;
}

static JS::Value table_limit_to_js_value(JS::VM& vm, u64 value, Wasm::AddressType address_type)
{
    if (address_type == Wasm::AddressType::I64)
        return JS::BigInt::create(vm, ::Crypto::SignedBigInteger { ::Crypto::UnsignedBigInteger { value } });
    return JS::Value { static_cast<u32>(value) };
}

WebIDL::ExceptionOr<GC::Ref<Table>> Table::construct_impl(JS::Realm& realm, Bindings::TableDescriptor& descriptor, Optional<JS::Value> value)
{
    auto& vm = realm.vm();

    // 1. Let elementtype be ToValueType(descriptor["element"]).
    auto reference_type = table_kind_to_value_type(descriptor.element);

    // 2. If elementtype is not a reftype, throw a TypeError exception.
    // NOTE: The TableKind IDL enum only accepts reference types.

    // 3. If descriptor["address"] exists, let addrtype be descriptor["address"]; otherwise, let addrtype be "i32".
    auto address_type = descriptor.address == Bindings::AddressType::I64 ? Wasm::AddressType::I64 : Wasm::AddressType::I32;

    // 4. Let initial be ? AddressValueToU64(descriptor["initial"], addrtype).
    auto initial = TRY(address_value_to_u64(vm, descriptor.initial, address_type));

    // 5. If descriptor["maximum"] exists, let maximum be ? AddressValueToU64(descriptor["maximum"], addrtype); otherwise, let maximum be empty.
    auto maximum = descriptor.maximum.has_value()
        ? Optional<u64> { TRY(address_value_to_u64(vm, descriptor.maximum.value(), address_type)) }
        : Optional<u64> {};

    // 6. Let type be the table type addrtype { min initial, max maximum } elementtype.
    Wasm::Limits limits { address_type, initial, maximum };
    Wasm::TableType table_type { reference_type, move(limits) };

    // 7. If type is not valid, throw a RangeError exception.
    if (maximum.has_value() && maximum.value() < initial)
        return vm.throw_completion<JS::RangeError>("Maximum should not be less than initial in table type"sv);

    // 8. If value is missing, let ref be DefaultValue(elementtype).
    // 9. Otherwise, let ref be ? ToWebAssemblyValue(value, elementtype).
    auto reference_value = !value.has_value()
        ? Detail::default_webassembly_value(vm, reference_type)
        : TRY(Detail::to_webassembly_value(vm, *value, reference_type));

    // 10. Let store be the surrounding agent's associated store.
    auto& cache = Detail::get_cache(realm);

    // 11. Let (store, tableaddr) be table_alloc(store, type, ref). If allocation fails, throw a RangeError exception.
    auto address = cache.abstract_machine().store().allocate(table_type);
    if (!address.has_value())
        return vm.throw_completion<JS::RangeError>("Wasm Table allocation failed"sv);

    auto const& reference = reference_value.to<Wasm::Reference>();
    auto& table = *cache.abstract_machine().store().get(*address);
    for (auto& element : table.elements())
        element = reference;

    // 12. Set the surrounding agent's associated store to store.
    // NOTE: The store is updated in-place.

    // 13. Initialize this from tableaddr.
    return realm.create<Table>(realm, *address);
}

Table::Table(JS::Realm& realm, Wasm::TableAddress address)
    : Bindings::PlatformObject(realm)
    , m_address(address)
{
}

void Table::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Table, WebAssembly.Table);
    Base::initialize(realm);

    // https://webassembly.github.io/spec/js-api/#initialize-a-table-object
    // 1. Let map be the surrounding agent's associated Table object cache.
    auto& cache = Detail::get_cache(realm);

    // 2. Assert: map[tableaddr] doesn't exist.
    auto exists = cache.table_instances().contains(m_address);
    VERIFY(!exists);

    // 3. Set table.[[Table]] to tableaddr.
    // NOTE: This is already set by the Table constructor.

    // 4. Set map[tableaddr] to table.
    cache.add_table_instance(m_address, *this);
}

// https://webassembly.github.io/spec/js-api/#dom-table-grow
WebIDL::ExceptionOr<JS::Value> Table::grow(JS::Value delta_value, Optional<JS::Value> value)
{
    auto& vm = this->vm();

    // 1. Let tableaddr be this.[[Table]].
    // 2. Let store be the surrounding agent's associated store.
    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table to grow"sv);

    // 3. Let initialSize be table_size(store, tableaddr).
    auto initial_size = table->elements().size();

    // 4. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    auto address_type = table->type().limits().address_type();
    auto element_type = table->type().element_type();

    // 5. Let delta64 be ? AddressValueToU64(delta, addrtype).
    auto delta = TRY(address_value_to_u64(vm, delta_value, address_type));

    // 6. If value is missing, let ref be DefaultValue(elementtype).
    // 7. Otherwise, let ref be ? ToWebAssemblyValue(value, elementtype).
    auto reference_value = !value.has_value()
        ? Detail::default_webassembly_value(vm, element_type)
        : TRY(Detail::to_webassembly_value(vm, *value, element_type));
    auto const& reference = reference_value.to<Wasm::Reference>();

    // 8. Let result be table_grow(store, tableaddr, delta64, ref).
    // 9. If result is error, throw a RangeError exception.
    if (!table->grow(delta, reference))
        return vm.throw_completion<JS::RangeError>("Failed to grow table"sv);

    // 10. Set the surrounding agent's associated store to result.
    // NOTE: The store is updated in-place.

    // 11. Return U64ToAddressValue(initialSize, addrtype).
    return table_limit_to_js_value(vm, initial_size, address_type);
}

// https://webassembly.github.io/spec/js-api/#dom-table-get
WebIDL::ExceptionOr<JS::Value> Table::get(JS::Value index_value) const
{
    auto& vm = this->vm();

    // 1. Let tableaddr be this.[[Table]].
    // 2. Let store be the surrounding agent's associated store.
    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table"sv);

    // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    auto address_type = table->type().limits().address_type();
    auto element_type = table->type().element_type();

    // 4. If elementtype matches exnref, throw a TypeError exception.
    if (element_type.kind() == Wasm::ValueType::ExceptionReference)
        return vm.throw_completion<JS::TypeError>("Cannot get an exnref table element"sv);

    // 5. Let index64 be ? AddressValueToU64(index, addrtype).
    auto index = TRY(address_value_to_u64(vm, index_value, address_type));

    // 6. Let result be table_read(store, tableaddr, index64).
    // 7. If result is error, throw a RangeError exception.
    if (table->elements().size() <= index)
        return vm.throw_completion<JS::RangeError>("Table element index out of range"sv);

    auto& ref = table->elements()[index];

    // 8. Return ! ToJSValue(result).
    Wasm::Value wasm_value { ref };
    return Detail::to_js_value(vm, wasm_value, element_type);
}

// https://webassembly.github.io/spec/js-api/#dom-table-set
WebIDL::ExceptionOr<void> Table::set(JS::Value index_value, Optional<JS::Value> value)
{
    auto& vm = this->vm();

    // 1. Let tableaddr be this.[[Table]].
    // 2. Let store be the surrounding agent's associated store.
    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table"sv);

    // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    auto address_type = table->type().limits().address_type();
    auto element_type = table->type().element_type();

    // 4. If elementtype matches exnref, throw a TypeError exception.
    if (element_type.kind() == Wasm::ValueType::ExceptionReference)
        return vm.throw_completion<JS::TypeError>("Cannot set an exnref table element"sv);

    // 5. Let index64 be ? AddressValueToU64(index, addrtype).
    auto index = TRY(address_value_to_u64(vm, index_value, address_type));

    if (table->elements().size() <= index)
        return vm.throw_completion<JS::RangeError>("Table element index out of range"sv);

    // 6. If value is missing, let ref be DefaultValue(elementtype).
    // 7. Otherwise, let ref be ? ToWebAssemblyValue(value, elementtype).
    auto reference_value = !value.has_value()
        ? Detail::default_webassembly_value(vm, element_type)
        : TRY(Detail::to_webassembly_value(vm, *value, element_type));
    auto const& reference = reference_value.to<Wasm::Reference>();

    // 8. Let store be table_write(store, tableaddr, index64, ref).
    // 9. If store is error, throw a RangeError exception.
    table->elements()[index] = reference;

    // 10. Set the surrounding agent's associated store to store.
    // NOTE: The store is updated in-place.
    return {};
}

// https://webassembly.github.io/spec/js-api/#dom-table-length
WebIDL::ExceptionOr<JS::Value> Table::length() const
{
    auto& vm = this->vm();

    // 1. Let tableaddr be this.[[Table]].
    // 2. Let store be the surrounding agent's associated store.
    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table"sv);

    // 3. Let addrtype be the address type in table_type(store, tableaddr).
    auto address_type = table->type().limits().address_type();

    // 4. Let length64 be table_size(store, tableaddr).
    auto length = table->elements().size();

    // 5. Return U64ToAddressValue(length64, addrtype).
    return table_limit_to_js_value(vm, length, address_type);
}

}
