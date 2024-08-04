/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TablePrototype.h>
#include <LibWeb/WebAssembly/Table.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

JS_DEFINE_ALLOCATOR(Table);

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

static JS::ThrowCompletionOr<Wasm::Value> value_to_reference(JS::VM& vm, JS::Value value, Wasm::ValueType const& reference_type)
{
    if (value.is_undefined())
        return Wasm::Value();
    return Detail::to_webassembly_value(vm, value, reference_type);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<Table>> Table::construct_impl(JS::Realm& realm, TableDescriptor& descriptor, JS::Value value)
{
    auto& vm = realm.vm();

    auto reference_type = table_kind_to_value_type(descriptor.element);
    auto reference_value = TRY(value_to_reference(vm, value, reference_type));

    Wasm::Limits limits { descriptor.initial, move(descriptor.maximum) };
    Wasm::TableType table_type { reference_type, move(limits) };

    auto& cache = Detail::get_cache(realm);
    auto address = cache.abstract_machine().store().allocate(table_type);
    if (!address.has_value())
        return vm.throw_completion<JS::TypeError>("Wasm Table allocation failed"sv);

    auto const& reference = reference_value.to<Wasm::Reference>();
    auto& table = *cache.abstract_machine().store().get(*address);
    for (auto& element : table.elements())
        element = reference;

    return vm.heap().allocate<Table>(realm, realm, *address);
}

Table::Table(JS::Realm& realm, Wasm::TableAddress address)
    : Bindings::PlatformObject(realm)
    , m_address(address)
{
}

void Table::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Table, WebAssembly.Table);
}

// https://webassembly.github.io/spec/js-api/#dom-table-grow
WebIDL::ExceptionOr<u32> Table::grow(u32 delta, JS::Value value)
{
    auto& vm = this->vm();

    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table to grow"sv);

    auto initial_size = table->elements().size();

    auto reference_value = TRY(value_to_reference(vm, value, table->type().element_type()));
    auto const& reference = reference_value.to<Wasm::Reference>();

    if (!table->grow(delta, reference))
        return vm.throw_completion<JS::RangeError>("Failed to grow table"sv);

    return initial_size;
}

// https://webassembly.github.io/spec/js-api/#dom-table-get
WebIDL::ExceptionOr<JS::Value> Table::get(u32 index) const
{
    auto& vm = this->vm();

    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table"sv);

    if (table->elements().size() <= index)
        return vm.throw_completion<JS::RangeError>("Table element index out of range"sv);

    auto& ref = table->elements()[index];

    Wasm::Value wasm_value { ref };
    return Detail::to_js_value(vm, wasm_value, table->type().element_type());
}

// https://webassembly.github.io/spec/js-api/#dom-table-set
WebIDL::ExceptionOr<void> Table::set(u32 index, JS::Value value)
{
    auto& vm = this->vm();

    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table"sv);

    if (table->elements().size() <= index)
        return vm.throw_completion<JS::RangeError>("Table element index out of range"sv);

    auto reference_value = TRY(value_to_reference(vm, value, table->type().element_type()));
    auto const& reference = reference_value.to<Wasm::Reference>();

    table->elements()[index] = reference;

    return {};
}

// https://webassembly.github.io/spec/js-api/#dom-table-length
WebIDL::ExceptionOr<u32> Table::length() const
{
    auto& vm = this->vm();

    auto& cache = Detail::get_cache(realm());
    auto* table = cache.abstract_machine().store().get(address());
    if (!table)
        return vm.throw_completion<JS::RangeError>("Could not find the memory table"sv);

    return table->elements().size();
}

}
