/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWasm/Runtime/Runtime.h>
#include <RustFFI.h>

namespace Wasm::Runtime {

static_assert(sizeof(ValueType) == sizeof(FFI::WasmValueType));
static_assert(sizeof(Value) == sizeof(FFI::WasmValue));
static_assert(sizeof(Extern) == sizeof(FFI::WasmExtern));
static_assert(sizeof(Limits) == sizeof(FFI::WasmLimits));
static_assert(sizeof(ExternType) == sizeof(FFI::WasmExternType));

static FFI::WasmValueType to_ffi(ValueType type) { return bit_cast<FFI::WasmValueType>(type); }
static FFI::WasmValue to_ffi(Value value) { return bit_cast<FFI::WasmValue>(value); }
static FFI::WasmLimits to_ffi(Limits limits) { return bit_cast<FFI::WasmLimits>(limits); }
static ValueType from_ffi(FFI::WasmValueType type) { return bit_cast<ValueType>(type); }
static Value from_ffi(FFI::WasmValue value) { return bit_cast<Value>(value); }
static Limits from_ffi(FFI::WasmLimits limits) { return bit_cast<Limits>(limits); }
static ExternType from_ffi(FFI::WasmExternType type) { return bit_cast<ExternType>(type); }
static StringView from_ffi(FFI::WasmStringView view) { return { reinterpret_cast<char const*>(view.data), view.length }; }

static ByteString take_string(FFI::WasmString string)
{
    if (!string.data)
        return {};
    ByteString result { reinterpret_cast<char const*>(string.data), string.length };
    FFI::wasm_string_release(string);
    return result;
}

static FFI::WasmString make_string(StringView text)
{
    // The engine releases the strings it gets back, so they are allocated the way it allocates its own: as a boxed
    // byte slice, which only the engine can create.
    FFI::WasmString string {};
    FFI::wasm_string_create(reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length(), &string);
    return string;
}

ErrorOr<NonnullRefPtr<Module>, ByteString> Module::compile(ReadonlyBytes bytes)
{
    FFI::WasmString error {};
    auto const* module = FFI::wasm_module_compile(bytes.data(), bytes.size(), &error);
    if (!module)
        return take_string(error);
    return adopt_ref(*new Module(module));
}

Module::~Module()
{
    FFI::wasm_module_release(m_module);
}

size_t Module::import_count() const
{
    return FFI::wasm_module_import_count(m_module);
}

Import Module::import(size_t index) const
{
    FFI::WasmStringView module_name {};
    FFI::WasmStringView name {};
    FFI::WasmExternType type {};
    FFI::wasm_module_import(m_module, index, &module_name, &name, &type);
    return { from_ffi(module_name), from_ffi(name), from_ffi(type) };
}

size_t Module::export_count() const
{
    return FFI::wasm_module_export_count(m_module);
}

ModuleExport Module::module_export(size_t index) const
{
    FFI::WasmStringView name {};
    FFI::WasmExternType type {};
    FFI::wasm_module_export(m_module, index, &name, &type);
    return { from_ffi(name), from_ffi(type) };
}

static FunctionType make_function_type(size_t parameter_count, size_t result_count, auto copy)
{
    Vector<FFI::WasmValueType> parameters;
    Vector<FFI::WasmValueType> results;
    parameters.resize(parameter_count);
    results.resize(result_count);
    copy(parameters.data(), results.data());
    FunctionType type;
    type.parameters.ensure_capacity(parameter_count);
    type.results.ensure_capacity(result_count);
    for (auto parameter : parameters)
        type.parameters.unchecked_append(from_ffi(parameter));
    for (auto result : results)
        type.results.unchecked_append(from_ffi(result));
    return type;
}

Optional<FunctionType> Module::function_type(u32 type_index) const
{
    size_t parameter_count = 0;
    size_t result_count = 0;
    if (!FFI::wasm_module_function_type(m_module, type_index, &parameter_count, &result_count))
        return {};
    return make_function_type(parameter_count, result_count, [&](auto* parameters, auto* results) {
        FFI::wasm_module_function_type_values(m_module, type_index, parameters, results);
    });
}

Store::Store()
    : m_store(FFI::wasm_store_create())
{
}

Store::~Store()
{
    FFI::wasm_store_destroy(m_store);
}

namespace {

struct HostFunctionData {
    Store& store;
    Store::HostFunction function;
    size_t result_count { 0 };
};

}

static bool call_host_function(void* user_data, FFI::WasmStore*, FFI::WasmValue const* arguments, size_t argument_count, FFI::WasmValue* results, size_t result_count, FFI::WasmString* error)
{
    auto& data = *static_cast<HostFunctionData*>(user_data);
    ReadonlySpan<Value> argument_span { reinterpret_cast<Value const*>(arguments), argument_count };
    Span<Value> result_span { reinterpret_cast<Value*>(results), result_count };
    auto result = data.function(data.store, argument_span, result_span);
    if (result.is_error()) {
        *error = make_string(result.error());
        return false;
    }
    return true;
}

static void release_host_function(void* user_data)
{
    delete static_cast<HostFunctionData*>(user_data);
}

u32 Store::allocate_host_function(FunctionType const& type, HostFunction function)
{
    Vector<FFI::WasmValueType> parameters;
    Vector<FFI::WasmValueType> results;
    for (auto parameter : type.parameters)
        parameters.append(to_ffi(parameter));
    for (auto result : type.results)
        results.append(to_ffi(result));
    auto* data = new HostFunctionData { *this, move(function), type.results.size() };
    return FFI::wasm_store_allocate_host_function(m_store, parameters.data(), parameters.size(), results.data(), results.size(), call_host_function, data, release_host_function);
}

Optional<u32> Store::allocate_memory(Limits limits)
{
    u32 address = 0;
    if (!FFI::wasm_store_allocate_memory(m_store, to_ffi(limits), &address))
        return {};
    return address;
}

u32 Store::allocate_table(ValueType element, Limits limits, u64 initial)
{
    return FFI::wasm_store_allocate_table(m_store, to_ffi(element), to_ffi(limits), initial);
}

u32 Store::allocate_global(ValueType type, bool is_mutable, Value value)
{
    return FFI::wasm_store_allocate_global(m_store, to_ffi(type), is_mutable, to_ffi(value));
}

ErrorOr<u32, Failure> Store::instantiate(Module const& module, ReadonlySpan<Extern> imports)
{
    u32 instance = 0;
    FFI::WasmString error {};
    auto status = FFI::wasm_store_instantiate(m_store, module.handle(), reinterpret_cast<FFI::WasmExtern const*>(imports.data()), imports.size(), &instance, &error);
    switch (status) {
    case FFI::WasmStatus::Ok:
        return instance;
    case FFI::WasmStatus::LinkError:
        return Failure { FailureKind::LinkError, take_string(error) };
    case FFI::WasmStatus::Trap:
        return Failure { FailureKind::Trap, take_string(error) };
    }
    VERIFY_NOT_REACHED();
}

Vector<InstanceExport> Store::instance_exports(u32 instance)
{
    auto count = FFI::wasm_store_instance_export_count(m_store, instance);
    Vector<InstanceExport> exports;
    exports.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index) {
        FFI::WasmStringView name {};
        FFI::WasmExtern value {};
        if (!FFI::wasm_store_instance_export(m_store, instance, index, &name, &value))
            continue;
        exports.unchecked_append({ from_ffi(name), bit_cast<Extern>(value) });
    }
    return exports;
}

Optional<Extern> Store::find_export(u32 instance, StringView name)
{
    FFI::WasmExtern value {};
    if (!FFI::wasm_store_instance_find_export(m_store, instance, reinterpret_cast<u8 const*>(name.characters_without_null_termination()), name.length(), &value))
        return {};
    return bit_cast<Extern>(value);
}

FunctionType Store::function_type(u32 function)
{
    size_t parameter_count = 0;
    size_t result_count = 0;
    FFI::wasm_store_function_type(m_store, function, &parameter_count, &result_count);
    return make_function_type(parameter_count, result_count, [&](auto* parameters, auto* results) {
        FFI::wasm_store_function_type_values(m_store, function, parameters, results);
    });
}

ErrorOr<Vector<Value>, ByteString> Store::invoke(u32 function, ReadonlySpan<Value> arguments)
{
    size_t parameter_count = 0;
    size_t result_count = 0;
    FFI::wasm_store_function_type(m_store, function, &parameter_count, &result_count);
    Vector<Value> results;
    results.resize(result_count);
    FFI::WasmString error {};
    auto status = FFI::wasm_store_invoke(m_store, function, reinterpret_cast<FFI::WasmValue const*>(arguments.data()), arguments.size(), reinterpret_cast<FFI::WasmValue*>(results.data()), results.size(), &error);
    if (status != FFI::WasmStatus::Ok)
        return take_string(error);
    return results;
}

u64 Store::function_reference(u32 function)
{
    return FFI::wasm_store_function_reference(m_store, function);
}

Optional<u32> Store::function_from_reference(u64 reference)
{
    u32 function = 0;
    if (!FFI::wasm_store_function_from_reference(m_store, reference, &function))
        return {};
    return function;
}

Bytes Store::memory(u32 memory)
{
    size_t size = 0;
    auto* data = FFI::wasm_store_memory_data(m_store, memory, &size);
    return { data, size };
}

i64 Store::grow_memory(u32 memory, u64 delta)
{
    return FFI::wasm_store_memory_grow(m_store, memory, delta);
}

Limits Store::memory_limits(u32 memory)
{
    return from_ffi(FFI::wasm_store_memory_limits(m_store, memory));
}

u64 Store::table_size(u32 table)
{
    return FFI::wasm_store_table_size(m_store, table);
}

ValueType Store::table_element_type(u32 table)
{
    FFI::WasmValueType element {};
    FFI::WasmLimits limits {};
    FFI::wasm_store_table_type(m_store, table, &element, &limits);
    return from_ffi(element);
}

Limits Store::table_limits(u32 table)
{
    FFI::WasmValueType element {};
    FFI::WasmLimits limits {};
    FFI::wasm_store_table_type(m_store, table, &element, &limits);
    return from_ffi(limits);
}

Optional<u64> Store::table_get(u32 table, u64 index)
{
    u64 value = 0;
    if (!FFI::wasm_store_table_get(m_store, table, index, &value))
        return {};
    return value;
}

bool Store::table_set(u32 table, u64 index, u64 value)
{
    return FFI::wasm_store_table_set(m_store, table, index, value);
}

i64 Store::grow_table(u32 table, u64 delta, u64 initial)
{
    return FFI::wasm_store_table_grow(m_store, table, delta, initial);
}

ValueType Store::global_type(u32 global, bool& is_mutable)
{
    return from_ffi(FFI::wasm_store_global_type(m_store, global, &is_mutable));
}

Value Store::global_get(u32 global)
{
    return from_ffi(FFI::wasm_store_global_get(m_store, global));
}

void Store::global_set(u32 global, Value value)
{
    FFI::wasm_store_global_set(m_store, global, to_ffi(value));
}

}
