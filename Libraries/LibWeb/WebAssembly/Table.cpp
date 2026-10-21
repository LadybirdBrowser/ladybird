/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Table.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Table);

WebIDL::ExceptionOr<GC::Ref<Table>> Table::create(Wasm::Runtime::ValueType element_type, bool address64, u64 initial, Optional<u64> maximum, u64 reference)
{
    if (maximum.has_value() && maximum.value() < initial)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Maximum should not be less than initial in table type"_utf16 };

    Wasm::Runtime::Limits limits {
        .minimum = initial,
        .maximum = maximum.value_or(0),
        .has_maximum = maximum.has_value(),
        .address64 = address64,
        .shared = false,
    };

    // 11. Let (store, tableaddr) be table_alloc(store, type, ref). If allocation fails, throw a RangeError exception.
    auto table = cache().store().allocate_table(element_type, limits, reference);
    if (!table.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Wasm Table allocation failed"_utf16 };

    // 12. Set the surrounding agent's associated store to store.
    // NOTE: The store is updated in-place.

    // 13. Initialize this from tableaddr.
    return Table::create(*table);
}

GC::Ref<Table> Table::create(Wasm::Runtime::Entity table)
{
    // https://webassembly.github.io/spec/js-api/#initialize-a-table-object
    // 1. Let map be the surrounding agent's associated Table object cache.
    // 2. Assert: map[tableaddr] doesn't exist.
    VERIFY(!cache().table_object(table));

    // 3. Set table.[[Table]] to tableaddr.
    auto object = GC::Heap::the().allocate<Table>(table);

    // 4. Set map[tableaddr] to table.
    cache().set_table_object(table, object);
    return object;
}

Table::Table(Wasm::Runtime::Entity table)
    : m_table(table)
{
}

void Table::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_table);
}

// https://webassembly.github.io/spec/js-api/#dom-table-grow
WebIDL::ExceptionOr<u64> Table::grow(u64 delta, u64 reference)
{
    // 3. Let initialSize be table_size(store, tableaddr).
    // 4. Let (limits, elementType) be table_type(tableaddr).
    // 7. Let result be table_grow(store, tableaddr, delta, ref).
    // 8. If result is error, throw a RangeError exception.
    auto initial_size = cache().store().grow_table(address(), delta, reference);
    if (initial_size < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Failed to grow table"_utf16 };

    // 10. Set the surrounding agent's associated store to result.
    // NOTE: The store is updated in-place.

    return static_cast<u64>(initial_size);
}

// https://webassembly.github.io/spec/js-api/#dom-table-get
WebIDL::ExceptionOr<u64> Table::get(u64 index) const
{
    // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    // 4. If elementtype matches exnref, throw a TypeError exception.
    auto heap = element_type().heap;
    if (heap == Wasm::Runtime::HeapKind::Exception || heap == Wasm::Runtime::HeapKind::NoException)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot get an exnref table element"_utf16 };

    // 6. Let result be table_read(store, tableaddr, index64).
    // 7. If result is error, throw a RangeError exception.
    auto result = cache().store().table_get(address(), index);
    if (!result.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Table element index out of range"_utf16 };
    return *result;
}

// https://webassembly.github.io/spec/js-api/#dom-table-set
WebIDL::ExceptionOr<void> Table::set(u64 index, u64 reference)
{
    // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    // 4. If elementtype matches exnref, throw a TypeError exception.
    auto heap = element_type().heap;
    if (heap == Wasm::Runtime::HeapKind::Exception || heap == Wasm::Runtime::HeapKind::NoException)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set an exnref table element"_utf16 };

    // 8. Let store be table_write(store, tableaddr, index64, ref).
    // 9. If store is error, throw a RangeError exception.
    if (!cache().store().table_set(address(), index, reference))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Table element index out of range"_utf16 };

    // 10. Set the surrounding agent's associated store to store.
    // NOTE: The store is updated in-place.
    return {};
}

// https://webassembly.github.io/spec/js-api/#dom-table-length
u64 Table::length() const
{
    return cache().store().table_size(address());
}

bool Table::is_address64() const
{
    return cache().store().table_limits(address()).address64;
}

Wasm::Runtime::ValueType Table::element_type() const
{
    return cache().store().table_element_type(address());
}

}

namespace Web::Bindings {

static Wasm::Runtime::ValueType table_kind_to_value_type(TableKind kind)
{
    switch (kind) {
    case TableKind::Externref:
        return Wasm::Runtime::ValueType::reference(Wasm::Runtime::HeapKind::Extern);
    case TableKind::Anyfunc:
        return Wasm::Runtime::ValueType::reference(Wasm::Runtime::HeapKind::Function);
    }
    VERIFY_NOT_REACHED();
}

// https://webassembly.github.io/spec/js-api/#addressvaluetou64
static WebIDL::ExceptionOr<u64> address_value_to_u64(JS::VM& vm, JS::Value value, bool address64)
{
    if (address64) {
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

static JS::Value table_limit_to_js_value(JS::VM& vm, u64 value, bool address64)
{
    if (address64)
        return JS::BigInt::create(vm, ::Crypto::SignedBigInteger { ::Crypto::UnsignedBigInteger { value } });
    return JS::Value { static_cast<u32>(value) };
}

WebIDL::ExceptionOr<GC::Ref<WebAssembly::Table>> construct_table(JS::Realm& realm, TableDescriptor const& descriptor, Optional<JS::Value> value)
{
    auto& vm = realm.vm();

    // 1. Let elementtype be ToValueType(descriptor["element"]).
    auto element_type = table_kind_to_value_type(descriptor.element);

    // 2. If elementtype is not a reftype, throw a TypeError exception.
    // NOTE: The TableKind IDL enum only accepts reference types.

    // 3. If descriptor["address"] exists, let addrtype be descriptor["address"]; otherwise, let addrtype be "i32".
    auto address64 = descriptor.address.value_or(AddressType::I32) == AddressType::I64;

    // 4. Let initial be ? AddressValueToU64(descriptor["initial"], addrtype).
    auto initial = TRY(address_value_to_u64(vm, descriptor.initial, address64));

    // 5. If descriptor["maximum"] exists, let maximum be ? AddressValueToU64(descriptor["maximum"], addrtype); otherwise, let maximum be empty.
    auto maximum = descriptor.maximum.has_value()
        ? Optional<u64> { TRY(address_value_to_u64(vm, descriptor.maximum.value(), address64)) }
        : Optional<u64> {};

    auto reference_value = !value.has_value()
        ? WebAssembly::Detail::default_webassembly_value(vm, element_type)
        : TRY(WebAssembly::Detail::to_webassembly_value(vm, *value, element_type));

    return WebAssembly::Table::create(element_type, address64, initial, maximum, reference_value.reference_bits());
}

JS::Value table(JS::Realm& realm, GC::Ref<WebAssembly::Table> table)
{
    return wrap(host_defined_wrapper_world(realm), realm, table);
}

static WebIDL::ExceptionOr<u64> value_to_table_reference(JS::Realm& realm, WebAssembly::Table& table, Optional<JS::Value> value)
{
    auto element_type = table.element_type();
    auto reference_value = !value.has_value()
        ? WebAssembly::Detail::default_webassembly_value(realm.vm(), element_type)
        : TRY(WebAssembly::Detail::to_webassembly_value(realm.vm(), *value, element_type));
    return reference_value.reference_bits();
}

WebIDL::ExceptionOr<JS::Value> grow(JS::Realm& realm, WebAssembly::Table& table, JS::Value delta_value, Optional<JS::Value> value)
{
    auto& vm = realm.vm();
    auto address64 = table.is_address64();
    auto delta = TRY(address_value_to_u64(vm, delta_value, address64));
    auto reference = TRY(value_to_table_reference(realm, table, value));
    auto initial_size = TRY(table.grow(delta, reference));
    return table_limit_to_js_value(vm, initial_size, address64);
}

WebIDL::ExceptionOr<JS::Value> get(JS::Realm& realm, WebAssembly::Table& table, JS::Value index_value)
{
    auto& vm = realm.vm();
    auto index = TRY(address_value_to_u64(vm, index_value, table.is_address64()));
    auto reference = TRY(table.get(index));
    return TRY(WebAssembly::Detail::to_js_value(vm, Wasm::Runtime::Value::from_reference(reference), table.element_type()));
}

WebIDL::ExceptionOr<void> set(JS::Realm& realm, WebAssembly::Table& table, JS::Value index_value, Optional<JS::Value> value)
{
    auto& vm = realm.vm();
    auto index = TRY(address_value_to_u64(vm, index_value, table.is_address64()));
    auto reference = TRY(value_to_table_reference(realm, table, value));
    return table.set(index, reference);
}

WebIDL::ExceptionOr<JS::Value> length(JS::Realm& realm, WebAssembly::Table& table)
{
    return table_limit_to_js_value(realm.vm(), table.length(), table.is_address64());
}

}
