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
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/Table.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Table);

WebIDL::ExceptionOr<GC::Ref<Table>> Table::create(NonnullRefPtr<Detail::WebAssemblyCache> cache, Wasm::ValueType element_type, Wasm::AddressType address_type, u64 initial, Optional<u64> maximum, Wasm::Reference reference)
{
    Wasm::Limits limits { address_type, initial, maximum };
    Wasm::TableType table_type { element_type, move(limits) };

    if (maximum.has_value() && maximum.value() < initial)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Maximum should not be less than initial in table type"sv };

    // 11. Let (store, tableaddr) be table_alloc(store, type, ref). If allocation fails, throw a RangeError exception.
    auto address = cache->abstract_machine().store().allocate(table_type);
    if (!address.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Wasm Table allocation failed"sv };

    auto& table = *cache->abstract_machine().store().get(*address);
    for (auto& element : table.elements())
        element = reference;

    // 12. Set the surrounding agent's associated store to store.
    // NOTE: The store is updated in-place.

    // 13. Initialize this from tableaddr.
    return GC::Heap::the().allocate<Table>(cache, *address);
}

Table::Table(NonnullRefPtr<Detail::WebAssemblyCache> cache, Wasm::TableAddress address)
    : m_cache(move(cache))
    , m_address(address)
{
    // https://webassembly.github.io/spec/js-api/#initialize-a-table-object
    // 1. Let map be the surrounding agent's associated Table object cache.

    // 2. Assert: map[tableaddr] doesn't exist.
    auto exists = m_cache->table_instances().contains(m_address);
    VERIFY(!exists);

    // 3. Set table.[[Table]] to tableaddr.
    // NOTE: This is already set by the Table constructor.

    // 4. Set map[tableaddr] to table.
    m_cache->add_table_instance(m_address, *this);
}

void Table::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_cache->visit_edges(visitor);
}

// https://webassembly.github.io/spec/js-api/#dom-table-grow
WebIDL::ExceptionOr<u64> Table::grow(u64 delta, Wasm::Reference reference)
{
    auto* table = cache().abstract_machine().store().get(address());
    if (!table)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the memory table to grow"sv };

    // 3. Let initialSize be table_size(store, tableaddr).
    auto initial_size = table->elements().size();

    if (!table->grow(delta, reference))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Failed to grow table"sv };

    // 10. Set the surrounding agent's associated store to result.
    // NOTE: The store is updated in-place.

    return initial_size;
}

// https://webassembly.github.io/spec/js-api/#dom-table-get
WebIDL::ExceptionOr<Wasm::Reference> Table::get(u64 index) const
{
    auto* table = cache().abstract_machine().store().get(address());
    if (!table)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the memory table"sv };

    // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    auto element_type = table->type().element_type();

    // 4. If elementtype matches exnref, throw a TypeError exception.
    if (element_type.kind() == Wasm::ValueType::ExceptionReference)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot get an exnref table element"sv };

    // 6. Let result be table_read(store, tableaddr, index64).
    // 7. If result is error, throw a RangeError exception.
    if (table->elements().size() <= index)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Table element index out of range"sv };

    return table->elements()[index];
}

// https://webassembly.github.io/spec/js-api/#dom-table-set
WebIDL::ExceptionOr<void> Table::set(u64 index, Wasm::Reference reference)
{
    auto* table = cache().abstract_machine().store().get(address());
    if (!table)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the memory table"sv };

    // 3. Let (addrtype, limits, elementtype) be table_type(store, tableaddr).
    auto element_type = table->type().element_type();

    // 4. If elementtype matches exnref, throw a TypeError exception.
    if (element_type.kind() == Wasm::ValueType::ExceptionReference)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set an exnref table element"sv };

    if (table->elements().size() <= index)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Table element index out of range"sv };

    // 8. Let store be table_write(store, tableaddr, index64, ref).
    // 9. If store is error, throw a RangeError exception.
    table->elements()[index] = reference;

    // 10. Set the surrounding agent's associated store to store.
    // NOTE: The store is updated in-place.
    return {};
}

// https://webassembly.github.io/spec/js-api/#dom-table-length
WebIDL::ExceptionOr<u64> Table::length() const
{
    auto* table = cache().abstract_machine().store().get(address());
    if (!table)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the memory table"sv };

    return table->elements().size();
}

WebIDL::ExceptionOr<Wasm::AddressType> Table::address_type() const
{
    auto* table = cache().abstract_machine().store().get(address());
    if (!table)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the memory table"sv };

    return table->type().limits().address_type();
}

WebIDL::ExceptionOr<Wasm::ValueType> Table::element_type() const
{
    auto* table = cache().abstract_machine().store().get(address());
    if (!table)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Could not find the memory table"sv };

    return table->type().element_type();
}

}

namespace Web::Bindings {

static Wasm::ValueType table_kind_to_value_type(TableKind kind)
{
    switch (kind) {
    case TableKind::Externref:
        return Wasm::ValueType { Wasm::ValueType::ExternReference };
    case TableKind::Anyfunc:
        return Wasm::ValueType { Wasm::ValueType::FunctionReference };
    }

    VERIFY_NOT_REACHED();
}

static Wasm::AddressType address_type_from_bindings(AddressType address_type)
{
    switch (address_type) {
    case AddressType::I32:
        return Wasm::AddressType::I32;
    case AddressType::I64:
        return Wasm::AddressType::I64;
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

WebIDL::ExceptionOr<GC::Ref<WebAssembly::Table>> construct_table(JS::Realm& realm, TableDescriptor const& descriptor, Optional<JS::Value> value)
{
    auto& vm = realm.vm();

    // 1. Let elementtype be ToValueType(descriptor["element"]).
    auto element_type = table_kind_to_value_type(descriptor.element);

    // 2. If elementtype is not a reftype, throw a TypeError exception.
    // NOTE: The TableKind IDL enum only accepts reference types.

    // 3. If descriptor["address"] exists, let addrtype be descriptor["address"]; otherwise, let addrtype be "i32".
    auto address_type = address_type_from_bindings(descriptor.address.value_or(AddressType::I32));

    // 4. Let initial be ? AddressValueToU64(descriptor["initial"], addrtype).
    auto initial = TRY(address_value_to_u64(vm, descriptor.initial, address_type));

    // 5. If descriptor["maximum"] exists, let maximum be ? AddressValueToU64(descriptor["maximum"], addrtype); otherwise, let maximum be empty.
    auto maximum = descriptor.maximum.has_value()
        ? Optional<u64> { TRY(address_value_to_u64(vm, descriptor.maximum.value(), address_type)) }
        : Optional<u64> {};

    auto reference_value = !value.has_value()
        ? WebAssembly::Detail::default_webassembly_value(realm, element_type)
        : TRY(WebAssembly::Detail::to_webassembly_value(realm, *value, element_type));

    auto cache = WebAssembly::Detail::get_cache(realm);
    return WebAssembly::Table::create(move(cache), element_type, address_type, initial, maximum, reference_value.to<Wasm::Reference>());
}

JS::Value table(JS::Realm& realm, GC::Ref<WebAssembly::Table> table)
{
    return wrap(host_defined_wrapper_world(realm), realm, table);
}

static WebIDL::ExceptionOr<Wasm::Reference> value_to_table_reference(JS::Realm& realm, WebAssembly::Table& table, Optional<JS::Value> value)
{
    auto element_type = TRY(table.element_type());
    auto reference_value = !value.has_value()
        ? WebAssembly::Detail::default_webassembly_value(realm, element_type)
        : TRY(WebAssembly::Detail::to_webassembly_value(realm, *value, element_type));

    return reference_value.to<Wasm::Reference>();
}

WebIDL::ExceptionOr<JS::Value> grow(JS::Realm& realm, WebAssembly::Table& table, JS::Value delta_value, Optional<JS::Value> value)
{
    auto& vm = realm.vm();
    auto address_type = TRY(table.address_type());
    auto delta = TRY(address_value_to_u64(vm, delta_value, address_type));
    auto reference = TRY(value_to_table_reference(realm, table, value));
    auto initial_size = TRY(table.grow(delta, reference));
    return table_limit_to_js_value(vm, initial_size, address_type);
}

WebIDL::ExceptionOr<JS::Value> get(JS::Realm& realm, WebAssembly::Table& table, JS::Value index_value)
{
    auto& vm = realm.vm();
    auto address_type = TRY(table.address_type());
    auto index = TRY(address_value_to_u64(vm, index_value, address_type));
    auto reference = TRY(table.get(index));
    auto element_type = TRY(table.element_type());
    auto wasm_value = Wasm::Value { reference };
    return WebAssembly::Detail::to_js_value(realm, wasm_value, element_type);
}

WebIDL::ExceptionOr<void> set(JS::Realm& realm, WebAssembly::Table& table, JS::Value index_value, Optional<JS::Value> value)
{
    auto& vm = realm.vm();
    auto address_type = TRY(table.address_type());
    auto index = TRY(address_value_to_u64(vm, index_value, address_type));
    auto reference = TRY(value_to_table_reference(realm, table, value));
    return table.set(index, reference);
}

WebIDL::ExceptionOr<JS::Value> length(JS::Realm& realm, WebAssembly::Table& table)
{
    auto address_type = TRY(table.address_type());
    auto length = TRY(table.length());
    return table_limit_to_js_value(realm.vm(), length, address_type);
}

}
