/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CAPI.h>
#include <LibGC/Heap.h>
#include <LibWasm/Runtime.h>
#include <RustFFI.h>

namespace Wasm::Runtime {

// NB: The types of Runtime.h mirror those of the C ABI, which they are bit_cast to and from.
static_assert(to_underlying(ValueKind::I32) == to_underlying(FFI::WasmValueKind::I32));
static_assert(to_underlying(ValueKind::I64) == to_underlying(FFI::WasmValueKind::I64));
static_assert(to_underlying(ValueKind::F32) == to_underlying(FFI::WasmValueKind::F32));
static_assert(to_underlying(ValueKind::F64) == to_underlying(FFI::WasmValueKind::F64));
static_assert(to_underlying(ValueKind::V128) == to_underlying(FFI::WasmValueKind::V128));
static_assert(to_underlying(ValueKind::Reference) == to_underlying(FFI::WasmValueKind::Reference));
static_assert(to_underlying(HeapKind::Function) == to_underlying(FFI::WasmHeapKind::Function));
static_assert(to_underlying(HeapKind::NoFunction) == to_underlying(FFI::WasmHeapKind::NoFunction));
static_assert(to_underlying(HeapKind::Extern) == to_underlying(FFI::WasmHeapKind::Extern));
static_assert(to_underlying(HeapKind::NoExtern) == to_underlying(FFI::WasmHeapKind::NoExtern));
static_assert(to_underlying(HeapKind::Any) == to_underlying(FFI::WasmHeapKind::Any));
static_assert(to_underlying(HeapKind::Eq) == to_underlying(FFI::WasmHeapKind::Eq));
static_assert(to_underlying(HeapKind::I31) == to_underlying(FFI::WasmHeapKind::I31));
static_assert(to_underlying(HeapKind::Struct) == to_underlying(FFI::WasmHeapKind::Struct));
static_assert(to_underlying(HeapKind::Array) == to_underlying(FFI::WasmHeapKind::Array));
static_assert(to_underlying(HeapKind::None) == to_underlying(FFI::WasmHeapKind::None));
static_assert(to_underlying(HeapKind::Exception) == to_underlying(FFI::WasmHeapKind::Exception));
static_assert(to_underlying(HeapKind::NoException) == to_underlying(FFI::WasmHeapKind::NoException));
static_assert(to_underlying(HeapKind::Concrete) == to_underlying(FFI::WasmHeapKind::Concrete));
static_assert(to_underlying(ExternKind::Function) == to_underlying(FFI::WasmExternKind::Function));
static_assert(to_underlying(ExternKind::Table) == to_underlying(FFI::WasmExternKind::Table));
static_assert(to_underlying(ExternKind::Memory) == to_underlying(FFI::WasmExternKind::Memory));
static_assert(to_underlying(ExternKind::Global) == to_underlying(FFI::WasmExternKind::Global));
static_assert(to_underlying(ExternKind::Tag) == to_underlying(FFI::WasmExternKind::Tag));
static_assert(sizeof(ValueType) == sizeof(FFI::WasmValueType));
static_assert(offsetof(ValueType, kind) == offsetof(FFI::WasmValueType, kind));
static_assert(offsetof(ValueType, heap) == offsetof(FFI::WasmValueType, heap));
static_assert(offsetof(ValueType, nullable) == offsetof(FFI::WasmValueType, nullable));
static_assert(offsetof(ValueType, type_index) == offsetof(FFI::WasmValueType, type_index));
static_assert(sizeof(Value) == sizeof(FFI::WasmValue));
static_assert(offsetof(Value, kind) == offsetof(FFI::WasmValue, kind));
static_assert(offsetof(Value, low) == offsetof(FFI::WasmValue, low));
static_assert(offsetof(Value, high) == offsetof(FFI::WasmValue, high));
static_assert(sizeof(Limits) == sizeof(FFI::WasmLimits));
static_assert(offsetof(Limits, minimum) == offsetof(FFI::WasmLimits, minimum));
static_assert(offsetof(Limits, maximum) == offsetof(FFI::WasmLimits, maximum));
static_assert(offsetof(Limits, has_maximum) == offsetof(FFI::WasmLimits, has_maximum));
static_assert(offsetof(Limits, address64) == offsetof(FFI::WasmLimits, address64));
static_assert(offsetof(Limits, shared) == offsetof(FFI::WasmLimits, shared));
static_assert(sizeof(ExternType) == sizeof(FFI::WasmExternType));
static_assert(offsetof(ExternType, kind) == offsetof(FFI::WasmExternType, kind));
static_assert(offsetof(ExternType, type_index) == offsetof(FFI::WasmExternType, type_index));
static_assert(offsetof(ExternType, limits) == offsetof(FFI::WasmExternType, limits));
static_assert(offsetof(ExternType, value_type) == offsetof(FFI::WasmExternType, value_type));
static_assert(offsetof(ExternType, is_mutable) == offsetof(FFI::WasmExternType, mutable_));

static FFI::WasmValueType to_ffi(ValueType type) { return bit_cast<FFI::WasmValueType>(type); }
static FFI::WasmValue to_ffi(Value value) { return bit_cast<FFI::WasmValue>(value); }
static FFI::WasmLimits to_ffi(Limits limits) { return bit_cast<FFI::WasmLimits>(limits); }
static ValueType from_ffi(FFI::WasmValueType type) { return bit_cast<ValueType>(type); }
static Value from_ffi(FFI::WasmValue value) { return bit_cast<Value>(value); }
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

template<typename To, typename From>
static To function_cast(From function)
{
    return reinterpret_cast<To>(function);
}

static FFI::GcInterface gc_interface(GC::Heap& heap)
{
    using Interface = FFI::GcInterface;
    return {
        .heap = &heap,
        .gc_get_layout = function_cast<decltype(Interface::gc_get_layout)>(gc_get_layout),
        .gc_allocator_create = function_cast<decltype(Interface::gc_allocator_create)>(gc_allocator_create),
        .gc_heap_allocate_cell = function_cast<decltype(Interface::gc_heap_allocate_cell)>(gc_heap_allocate_cell),
        .gc_heap_defer_gc = function_cast<decltype(Interface::gc_heap_defer_gc)>(gc_heap_defer_gc),
        .gc_heap_undefer_gc = function_cast<decltype(Interface::gc_heap_undefer_gc)>(gc_heap_undefer_gc),
        .gc_heap_collect_garbage = function_cast<decltype(Interface::gc_heap_collect_garbage)>(gc_heap_collect_garbage),
        .gc_heap_did_allocate_external_memory = function_cast<decltype(Interface::gc_heap_did_allocate_external_memory)>(gc_heap_did_allocate_external_memory),
        .gc_root_create = function_cast<decltype(Interface::gc_root_create)>(gc_root_create),
        .gc_root_destroy = function_cast<decltype(Interface::gc_root_destroy)>(gc_root_destroy),
        .gc_visitor_visit_cell = function_cast<decltype(Interface::gc_visitor_visit_cell)>(gc_visitor_visit_cell),
        .gc_visitor_visit_possible_values = function_cast<decltype(Interface::gc_visitor_visit_possible_values)>(gc_visitor_visit_possible_values),
        .gc_primitive_storage_reserve = function_cast<decltype(Interface::gc_primitive_storage_reserve)>(gc_primitive_storage_reserve),
        .gc_primitive_storage_resize = function_cast<decltype(Interface::gc_primitive_storage_resize)>(gc_primitive_storage_resize),
        .gc_primitive_storage_free = gc_primitive_storage_free,
        .gc_primitive_storage_data = gc_primitive_storage_data,
    };
}

Store::Store(GC::Heap& heap)
{
    auto interface = gc_interface(heap);
    m_store = FFI::wasm_store_create(&interface);
}

Store::~Store()
{
    FFI::wasm_store_destroy(m_store);
}

static void* to_ffi(Entity entity) { return entity; }
static Entity entity_from_ffi(void* cell) { return static_cast<GC::ForeignCell*>(cell); }

namespace {

struct HostFunctionData {
    AK_ALLOC_WITH_KMALLOC;

    Store& store;
    Store::HostFunction function;
};

}

static FFI::WasmStatus call_host_function(void* user_data, FFI::WasmStore*, FFI::WasmValue const* arguments, size_t argument_count, FFI::WasmValue* results, size_t result_count, FFI::WasmString* error, u64*)
{
    auto& data = *static_cast<HostFunctionData*>(user_data);
    ReadonlySpan<Value> argument_span { reinterpret_cast<Value const*>(arguments), argument_count };
    Span<Value> result_span { reinterpret_cast<Value*>(results), result_count };
    auto result = data.function(data.store, argument_span, result_span);
    if (!result.is_error())
        return FFI::WasmStatus::Ok;
    *error = make_string(result.error().message);
    return FFI::WasmStatus::Trap;
}

static void visit_host_function(void* user_data, void* visitor)
{
    // NB: LibGC's C interface passes its Cell::Visitor as the visitor.
    auto& data = *static_cast<HostFunctionData*>(user_data);
    static_cast<GC::Cell::Visitor*>(visitor)->visit_possible_values(data.function.raw_capture_range());
}

static void release_host_function(void* user_data)
{
    delete static_cast<HostFunctionData*>(user_data);
}

Entity Store::allocate_host_function(FunctionType const& type, HostFunction function)
{
    Vector<FFI::WasmValueType> parameters;
    Vector<FFI::WasmValueType> results;
    for (auto parameter : type.parameters)
        parameters.append(to_ffi(parameter));
    for (auto result : type.results)
        results.append(to_ffi(result));
    auto* data = new HostFunctionData { *this, move(function) };
    return entity_from_ffi(FFI::wasm_store_allocate_host_function(m_store, parameters.data(), parameters.size(), results.data(), results.size(), call_host_function, visit_host_function, release_host_function, data));
}

Optional<Entity> Store::allocate_memory(Limits limits)
{
    void* memory = nullptr;
    if (!FFI::wasm_store_allocate_memory(m_store, to_ffi(limits), &memory))
        return {};
    return entity_from_ffi(memory);
}

Optional<Entity> Store::allocate_table(ValueType element, Limits limits, u64 initial)
{
    void* table = nullptr;
    if (!FFI::wasm_store_allocate_table(m_store, to_ffi(element), to_ffi(limits), initial, &table))
        return {};
    return entity_from_ffi(table);
}

Entity Store::allocate_global(ValueType type, bool is_mutable, Value value)
{
    return entity_from_ffi(FFI::wasm_store_allocate_global(m_store, to_ffi(type), is_mutable, to_ffi(value)));
}

static Failure failure_of(FFI::WasmStatus status, FFI::WasmString error, u64 exception)
{
    switch (status) {
    case FFI::WasmStatus::LinkError:
        return { FailureKind::LinkError, take_string(error) };
    case FFI::WasmStatus::Trap:
        return { FailureKind::Trap, take_string(error) };
    case FFI::WasmStatus::Exception:
        return { FailureKind::Exception, take_string(error), exception };
    case FFI::WasmStatus::StackExhausted:
        return { FailureKind::StackExhausted, take_string(error) };
    case FFI::WasmStatus::Ok:
        break;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Entity, Failure> Store::instantiate(Module const& module, ReadonlySpan<Extern> imports)
{
    static_assert(sizeof(Extern) == sizeof(FFI::WasmExtern));
    void* instance = nullptr;
    FFI::WasmString error {};
    u64 exception = 0;
    auto status = FFI::wasm_store_instantiate(m_store, module.handle(), reinterpret_cast<FFI::WasmExtern const*>(imports.data()), imports.size(), &instance, &error, &exception);
    if (status == FFI::WasmStatus::Ok)
        return entity_from_ffi(instance);
    return failure_of(status, error, exception);
}

Vector<InstanceExport> Store::instance_exports(Entity instance)
{
    auto count = FFI::wasm_store_instance_export_count(m_store, to_ffi(instance));
    Vector<InstanceExport> exports;
    exports.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index) {
        FFI::WasmStringView name {};
        FFI::WasmExtern value {};
        if (!FFI::wasm_store_instance_export(m_store, to_ffi(instance), index, &name, &value))
            continue;
        exports.unchecked_append({ from_ffi(name), bit_cast<Extern>(value) });
    }
    return exports;
}

Optional<Extern> Store::find_export(Entity instance, StringView name)
{
    FFI::WasmExtern value {};
    if (!FFI::wasm_store_instance_find_export(m_store, to_ffi(instance), reinterpret_cast<u8 const*>(name.characters_without_null_termination()), name.length(), &value))
        return {};
    return bit_cast<Extern>(value);
}

FunctionType Store::function_type(Entity function)
{
    size_t parameter_count = 0;
    size_t result_count = 0;
    FFI::wasm_store_function_type(m_store, to_ffi(function), &parameter_count, &result_count);
    return make_function_type(parameter_count, result_count, [&](auto* parameters, auto* results) {
        FFI::wasm_store_function_type_values(m_store, to_ffi(function), parameters, results);
    });
}

ErrorOr<Values, Failure> Store::invoke(Entity function, ReadonlySpan<Value> arguments)
{
    size_t parameter_count = 0;
    size_t result_count = 0;
    FFI::wasm_store_function_type(m_store, to_ffi(function), &parameter_count, &result_count);
    Values results;
    results.resize(result_count);
    FFI::WasmString error {};
    u64 exception = 0;
    auto status = FFI::wasm_store_invoke(m_store, to_ffi(function), reinterpret_cast<FFI::WasmValue const*>(arguments.data()), arguments.size(), reinterpret_cast<FFI::WasmValue*>(results.data()), results.size(), &error, &exception);
    if (status != FFI::WasmStatus::Ok)
        return failure_of(status, error, exception);
    return results;
}

u64 Store::function_reference(Entity function)
{
    return FFI::wasm_store_function_reference(m_store, to_ffi(function));
}

Entity Store::function_from_reference(u64 reference)
{
    void* function = nullptr;
    VERIFY(FFI::wasm_store_function_from_reference(m_store, reference, &function));
    return entity_from_ffi(function);
}

ValueType Store::global_type(Entity global, bool& is_mutable)
{
    return from_ffi(FFI::wasm_store_global_type(m_store, to_ffi(global), &is_mutable));
}

Value Store::global_get(Entity global)
{
    return from_ffi(FFI::wasm_store_global_get(m_store, to_ffi(global)));
}

static Vector<FFI::WasmStringView> string_views(ReadonlySpan<ByteString> strings)
{
    Vector<FFI::WasmStringView> views;
    views.ensure_capacity(strings.size());
    for (auto const& string : strings)
        views.unchecked_append({ reinterpret_cast<u8 const*>(string.characters()), string.length() });
    return views;
}

Wasi::Wasi(ReadonlySpan<ByteString> arguments, ReadonlySpan<ByteString> environment, ReadonlySpan<Preopen> preopens)
{
    Vector<ByteString> host_paths;
    Vector<ByteString> guest_paths;
    for (auto const& preopen : preopens) {
        host_paths.append(preopen.host_path);
        guest_paths.append(preopen.guest_path);
    }
    auto argument_views = string_views(arguments);
    auto environment_views = string_views(environment);
    auto host_views = string_views(host_paths);
    auto guest_views = string_views(guest_paths);
    m_wasi = FFI::wasm_wasi_create(argument_views.data(), argument_views.size(), environment_views.data(), environment_views.size(), host_views.data(), guest_views.data(), preopens.size());
}

Wasi::~Wasi()
{
    FFI::wasm_wasi_destroy(m_wasi);
}

Optional<Entity> Wasi::allocate_function(Store& store, StringView name)
{
    void* function = nullptr;
    if (!FFI::wasm_wasi_allocate_function(store.handle(), m_wasi, reinterpret_cast<u8 const*>(name.characters_without_null_termination()), name.length(), &function))
        return {};
    return entity_from_ffi(function);
}

void Wasi::set_memory(Entity memory)
{
    FFI::wasm_wasi_set_memory(m_wasi, to_ffi(memory));
}

Optional<i32> Wasi::exit_code() const
{
    i32 code = 0;
    if (!FFI::wasm_wasi_exit_code(m_wasi, &code))
        return {};
    return code;
}

}
