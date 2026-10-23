/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/File.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibTest/JavaScriptTestHost.h>
#include <LibWasm/Runtime.h>
#include <string.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <psapi.h>
#else
#    include <sys/resource.h>
#endif

TEST_ROOT("Libraries/LibWasm/Tests");

TESTJS_GLOBAL_FUNCTION(read_binary_wasm_file, readBinaryWasmFile)
{
    auto& realm = *vm.current_realm();

    auto error_code_to_string = [](int code) {
        auto const* error_string = strerror(code);
        return StringView { error_string, strlen(error_string) };
    };

    auto filename = TRY(vm.argument(0).to_utf16_string(vm)).to_utf8_but_should_be_ported_to_utf16();
    auto file = Core::File::open(filename, Core::File::OpenMode::Read);
    if (file.is_error())
        return vm.throw_completion<JS::TypeError>(Utf16String::from_utf8(error_code_to_string(file.error().code())));

    auto file_size = file.value()->size();
    if (file_size.is_error())
        return vm.throw_completion<JS::TypeError>(Utf16String::from_utf8(error_code_to_string(file_size.error().code())));

    auto array = TRY(JS::Uint8Array::create(realm, file_size.value()));

    auto bytes = MUST(ByteBuffer::create_uninitialized(file_size.value()));
    auto read = file.value()->read_until_filled(bytes);
    if (read.is_error())
        return vm.throw_completion<JS::TypeError>(Utf16String::from_utf8(error_code_to_string(read.error().code())));
    array->viewed_array_buffer()->overwrite(array->byte_offset(), bytes.data(), bytes.size());

    return JS::Value(array);
}

namespace {

static constexpr JSHostClass web_assembly_module_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "WebAssemblyModule"sv, nullptr, nullptr, nullptr, 0);

// Host references (`ref.extern n` in the spec tests) are encoded so that they are never null, and never look like a
// pointer or an i31 reference.
static u64 host_reference_bits(u64 value)
{
    return (value << 2) | 2;
}

static Optional<u64> host_reference_value(u64 bits)
{
    if ((bits & 3) != 2)
        return {};
    return bits >> 2;
}

struct ImportName {
    ByteString module;
    ByteString name;

    bool operator==(ImportName const&) const = default;
};

}

template<>
struct AK::Traits<ImportName> : public DefaultTraits<ImportName> {
    static unsigned hash(ImportName const& name) { return pair_int_hash(name.module.hash(), name.name.hash()); }
};

namespace {

// The module that parseWebAssemblyModule() instantiates. Script sees it as a host object that carries this cell as its
// host data and has the module's getExport() and invoke() as own properties.
class WebAssemblyModule final : public JS::Cell {
    GC_CELL(WebAssemblyModule, JS::Cell);
    GC_DECLARE_ALLOCATOR(WebAssemblyModule);

public:
    using JSValueConversionIsForbidden = void;

    static WebAssemblyModule* from_object(JS::Object const& object)
    {
        return JS::host_data_if<WebAssemblyModule>(object);
    }

    static Wasm::Runtime::Store& store()
    {
        if (!s_store)
            s_store = new Wasm::Runtime::Store(GC::Heap::the());
        return *s_store;
    }

    static void reset_store()
    {
        s_spec_test_namespace.clear();
        roots().clear();
        delete s_store;
        s_store = nullptr;
    }

    Wasm::Runtime::Entity instance() const { return m_instance.ptr(); }

    // Keep an entity of the current file alive, since the tests refer to entities by number.
    static Wasm::Runtime::Entity keep(Wasm::Runtime::Entity entity)
    {
        roots().append(GC::Root<GC::ForeignCell> { entity });
        return entity;
    }

    // NB: Like the last store, the last roots are left for the process to reclaim, since the heap is gone at exit.
    static Vector<GC::Root<GC::ForeignCell>>& roots()
    {
        static NeverDestroyed<Vector<GC::Root<GC::ForeignCell>>> roots;
        return *roots;
    }

    static JS::ThrowCompletionOr<GC::Ref<JS::HostObject>> create(JS::Realm& realm, NonnullRefPtr<Wasm::Runtime::Module> module, HashMap<ImportName, Wasm::Runtime::Extern> const& registered)
    {
        auto& vm = realm.vm();
        auto const& spectest = spec_test_namespace();
        Vector<Wasm::Runtime::Extern> imports;
        for (size_t index = 0; index < module->import_count(); ++index) {
            auto import = module->import(index);
            ImportName name { import.module, import.name };
            if (auto value = registered.get(name); value.has_value()) {
                imports.append(*value);
                continue;
            }
            if (auto value = spectest.get(name); value.has_value()) {
                imports.append(*value);
                continue;
            }
            return vm.throw_completion<JS::TypeError>("Link failed"_utf16);
        }
        auto result = store().instantiate(*module, imports);
        if (result.is_error())
            return vm.throw_completion<JS::TypeError>(Utf16String::from_utf8(result.error().message));

        auto instance = realm.create<WebAssemblyModule>();
        instance->m_module = move(module);
        instance->m_instance = keep(result.value());
        auto object = JS::HostObject::create(realm, web_assembly_module_host_class, realm.intrinsics().object_prototype(), {}, instance);
        object->define_native_function(realm, "getExport"_utf16_fly_string, get_export, 1, JS::default_attributes);
        object->define_native_function(realm, "invoke"_utf16_fly_string, wasm_invoke, 1, JS::default_attributes);
        return object;
    }

    ~WebAssemblyModule() override = default;

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_instance);
    }

    JS_DECLARE_NATIVE_FUNCTION(get_export);
    JS_DECLARE_NATIVE_FUNCTION(wasm_invoke);

    static HashMap<ImportName, Wasm::Runtime::Extern> const& spec_test_namespace()
    {
        using namespace Wasm::Runtime;
        if (!s_spec_test_namespace.is_empty())
            return s_spec_test_namespace;

        auto add_print = [](StringView name, Vector<ValueType> parameters) {
            auto address = keep(store().allocate_host_function({ move(parameters), {} }, [](auto&, auto, auto) -> ErrorOr<void, Wasm::Runtime::HostError> {
                // Noop, this just needs to exist.
                return {};
            }));
            s_spec_test_namespace.set({ "spectest", name }, { ExternKind::Function, address });
        };
        auto i32 = ValueType::scalar(ValueKind::I32);
        auto i64 = ValueType::scalar(ValueKind::I64);
        auto f32 = ValueType::scalar(ValueKind::F32);
        auto f64 = ValueType::scalar(ValueKind::F64);
        add_print("print"sv, {});
        add_print("print_i32"sv, { i32 });
        add_print("print_i64"sv, { i64 });
        add_print("print_f32"sv, { f32 });
        add_print("print_f64"sv, { f64 });
        add_print("print_i32_f32"sv, { i32, f32 });
        add_print("print_f64_f64"sv, { f64, f64 });

        auto funcref = ValueType::reference(HeapKind::Function);
        auto table = store().allocate_table(funcref, { 10, 20, true, false, false }, 0);
        s_spec_test_namespace.set({ "spectest", "table" }, { ExternKind::Table, keep(table.value()) });
        auto table64 = store().allocate_table(funcref, { 10, 20, true, true, false }, 0);
        s_spec_test_namespace.set({ "spectest", "table64" }, { ExternKind::Table, keep(table64.value()) });

        auto memory = store().allocate_memory({ 1, 2, true, false, false });
        s_spec_test_namespace.set({ "spectest", "memory" }, { ExternKind::Memory, keep(memory.value()) });

        s_spec_test_namespace.set({ "spectest", "global_i32" }, { ExternKind::Global, keep(store().allocate_global(i32, false, Value::from_i32(666))) });
        s_spec_test_namespace.set({ "spectest", "global_i64" }, { ExternKind::Global, keep(store().allocate_global(i64, false, Value::from_i64(666))) });
        s_spec_test_namespace.set({ "spectest", "global_f32" }, { ExternKind::Global, keep(store().allocate_global(f32, false, Value::from_f32(666.6f))) });
        s_spec_test_namespace.set({ "spectest", "global_f64" }, { ExternKind::Global, keep(store().allocate_global(f64, false, Value::from_f64(666.6))) });

        return s_spec_test_namespace;
    }

    static HashMap<ImportName, Wasm::Runtime::Extern> s_spec_test_namespace;
    // NB: The last store is left for the process to reclaim, since the heap it roots its cells in is gone at exit.
    static Wasm::Runtime::Store* s_store;
    RefPtr<Wasm::Runtime::Module> m_module;
    GC::Ptr<GC::ForeignCell> m_instance;
};

GC_DEFINE_ALLOCATOR(WebAssemblyModule);

Wasm::Runtime::Store* WebAssemblyModule::s_store;
HashMap<ImportName, Wasm::Runtime::Extern> WebAssemblyModule::s_spec_test_namespace;

}

TESTJS_RUN_FILE_FUNCTION(ByteString const&, JS::Realm&, JS::ExecutionContext&)
{
    WebAssemblyModule::reset_store();
    return Test::JS::RunFileHookResult::RunAsNormal;
}

static JS::ThrowCompletionOr<ByteBuffer> bytes_of_typed_array(JS::VM& vm, JS::Value value, StringView function_name)
{
    auto object = TRY(value.to_object(vm));
    if (!is<JS::Uint8Array>(*object))
        return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Expected a Uint8Array argument to {}", function_name));
    auto& array = static_cast<JS::Uint8Array&>(*object);
    auto record = JS::make_typed_array_with_buffer_witness_record(array, JS::ArrayBuffer::Order::SeqCst);
    return MUST(array.viewed_array_buffer()->copy_to_byte_buffer(array.byte_offset(), JS::typed_array_byte_length(record)));
}

TESTJS_GLOBAL_FUNCTION(parse_webassembly_module, parseWebAssemblyModule)
{
    auto& realm = *vm.current_realm();
    auto bytes = TRY(bytes_of_typed_array(vm, vm.argument(0), "parse_webassembly_module"sv));
    auto module = Wasm::Runtime::Module::compile(bytes);
    if (module.is_error())
        return vm.throw_completion<JS::SyntaxError>(Utf16String::from_utf8(module.error()));

    HashMap<ImportName, Wasm::Runtime::Extern> imports;
    auto import_value = vm.argument(1);
    if (auto import_object = import_value.template as_if<JS::Object>()) {
        for (auto property_key_value : TRY(import_object->ordinary_own_property_keys())) {
            auto property_key = MUST(JS::PropertyKey::from_value(vm, property_key_value));
            if (!property_key.is_string())
                continue;
            auto module_value = import_object->get_without_side_effects(property_key);
            auto* module_object = module_value.is_object() ? WebAssemblyModule::from_object(module_value.as_object()) : nullptr;
            if (!module_object)
                continue;
            auto module_name = property_key.as_string().to_utf16_string().to_byte_string();
            for (auto& entry : WebAssemblyModule::store().instance_exports(module_object->instance()))
                imports.set({ module_name, entry.name }, entry.value);
        }
    }

    return JS::Value(TRY(WebAssemblyModule::create(realm, module.release_value(), imports)));
}

TESTJS_GLOBAL_FUNCTION(validate_webassembly_module, validateWebAssemblyModule)
{
    auto bytes = TRY(bytes_of_typed_array(vm, vm.argument(0), "validate_webassembly_module"sv));
    auto module = Wasm::Runtime::Module::compile(bytes);
    if (module.is_error())
        return vm.throw_completion<JS::SyntaxError>(Utf16String::from_utf8(module.error()));
    return JS::js_undefined();
}

// The most memory the process has had resident, in bytes.
TESTJS_GLOBAL_FUNCTION(peak_resident_memory_size, peakResidentMemorySize)
{
#if defined(AK_OS_WINDOWS)
    PROCESS_MEMORY_COUNTERS counters {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return vm.throw_completion<JS::TypeError>("GetProcessMemoryInfo failed"_utf16);
    return JS::Value(static_cast<double>(counters.PeakWorkingSetSize));
#else
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return vm.throw_completion<JS::TypeError>("getrusage failed"_utf16);
#    if defined(AK_OS_MACOS)
    return JS::Value(static_cast<double>(usage.ru_maxrss));
#    else
    return JS::Value(static_cast<double>(usage.ru_maxrss) * 1024);
#    endif
#endif
}

TESTJS_GLOBAL_FUNCTION(compare_typed_arrays, compareTypedArrays)
{
    auto lhs = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::TypedArrayBase>(*lhs))
        return vm.throw_completion<JS::TypeError>("Expected a TypedArray"_utf16);
    auto& lhs_array = static_cast<JS::TypedArrayBase&>(*lhs);
    auto rhs = TRY(vm.argument(1).to_object(vm));
    if (!is<JS::TypedArrayBase>(*rhs))
        return vm.throw_completion<JS::TypeError>("Expected a TypedArray"_utf16);
    auto& rhs_array = static_cast<JS::TypedArrayBase&>(*rhs);
    auto lhs_record = JS::make_typed_array_with_buffer_witness_record(lhs_array, JS::ArrayBuffer::Order::SeqCst);
    auto rhs_record = JS::make_typed_array_with_buffer_witness_record(rhs_array, JS::ArrayBuffer::Order::SeqCst);
    auto lhs_byte_length = JS::typed_array_byte_length(lhs_record);
    auto rhs_byte_length = JS::typed_array_byte_length(rhs_record);
    if (lhs_byte_length != rhs_byte_length)
        return JS::Value(false);
    auto lhs_bytes = MUST(lhs_array.viewed_array_buffer()->copy_to_byte_buffer(lhs_array.byte_offset(), lhs_byte_length));
    auto rhs_bytes = MUST(rhs_array.viewed_array_buffer()->copy_to_byte_buffer(rhs_array.byte_offset(), rhs_byte_length));
    return JS::Value(lhs_bytes.bytes() == rhs_bytes.bytes());
}

static bool _is_canonical_nan32(u32 value)
{
    return value == 0x7FC00000 || value == 0xFFC00000;
}

static bool _is_canonical_nan64(u64 value)
{
    return value == 0x7FF8000000000000 || value == 0xFFF8000000000000;
}

TESTJS_GLOBAL_FUNCTION(is_canonical_nan32, isCanonicalNaN32)
{
    auto value = TRY(vm.argument(0).to_u32(vm));
    return _is_canonical_nan32(value);
}

TESTJS_GLOBAL_FUNCTION(is_canonical_nan64, isCanonicalNaN64)
{
    auto value = TRY(vm.argument(0).to_bigint_uint64(vm));
    return _is_canonical_nan64(value);
}

TESTJS_GLOBAL_FUNCTION(is_arithmetic_nan32, isArithmeticNaN32)
{
    auto const bits = TRY(vm.argument(0).to_u32(vm));
    auto const payload = bits & 0x007FFFFF;
    return (bits & 0x7F800000) == 0x7F800000 && payload >= 0x00400000;
}

TESTJS_GLOBAL_FUNCTION(is_arithmetic_nan64, isArithmeticNaN64)
{
    auto const bits = TRY(vm.argument(0).to_bigint_uint64(vm));
    auto const payload = bits & 0x000FFFFFFFFFFFFFULL;
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && payload >= 0x0008000000000000ull;
}

TESTJS_GLOBAL_FUNCTION(test_simd_vector, testSIMDVector)
{
    auto expected = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::Array>(*expected))
        return vm.throw_completion<JS::TypeError>("Expected an Array"_utf16);
    auto& expected_array = static_cast<JS::Array&>(*expected);
    auto got = TRY(vm.argument(1).to_object(vm));
    if (!is<JS::TypedArrayBase>(*got))
        return vm.throw_completion<JS::TypeError>("Expected a TypedArray"_utf16);
    auto& got_array = static_cast<JS::TypedArrayBase&>(*got);
    auto element_size = 128 / TRY(TRY(expected_array.get("length"_utf16_fly_string)).to_u32(vm));
    size_t i = 0;
    for (u32 it_index = 0; it_index < expected_array.indexed_array_like_size(); ++it_index) {
        if (!TRY(expected_array.has_own_property(it_index)))
            continue;
        auto got_value = TRY(got_array.get(i++));
        u64 got = got_value.is_bigint() ? TRY(got_value.to_bigint_uint64(vm)) : (u64)TRY(got_value.to_length(vm));
        auto expect = TRY(expected_array.get(it_index));
        if (expect.is_string()) {
            if (element_size != 32 && element_size != 64)
                return vm.throw_completion<JS::TypeError>("Expected element of size 32 or 64"_utf16);
            auto string = expect.as_string().utf16_string_view().to_utf8_but_should_be_ported_to_utf16();
            if (string == "nan:canonical") {
                auto is_canonical = element_size == 32 ? _is_canonical_nan32(got) : _is_canonical_nan64(got);
                if (!is_canonical)
                    return false;
                continue;
            }
            if (string == "nan:arithmetic") {
                auto is_arithmetic = element_size == 32 ? isnan(bit_cast<float>((u32)got)) : isnan(bit_cast<double>((u64)got));
                if (!is_arithmetic)
                    return false;
                continue;
            }
            return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Bad SIMD float expectation: {}", string));
        }
        u64 expect_value = expect.is_bigint() ? TRY(expect.to_bigint_uint64(vm)) : (u64)TRY(expect.to_length(vm));
        if (got != expect_value)
            return false;
    }
    return true;
}

TESTJS_GLOBAL_FUNCTION(is_valid_funcref_in, isValidFuncrefIn)
{
    auto value = TRY(vm.argument(0).to_length(vm));
    auto module_object = TRY(vm.argument(1).to_object(vm));
    if (!WebAssemblyModule::from_object(*module_object))
        return vm.throw_completion<JS::TypeError>("Expected a WebAssemblyModule"_utf16);
    // NB: A function reference surfaces as its function's address, which is never zero.
    return JS::Value(value != 0);
}

static JS::Value reference_to_js_value(Wasm::Runtime::Value value, Wasm::Runtime::ValueType type)
{
    using namespace Wasm::Runtime;
    if (value.is_null_reference())
        return JS::js_null();
    auto bits = value.reference_bits();
    if (auto host = host_reference_value(bits); host.has_value())
        return JS::Value(static_cast<double>(*host));
    if ((bits & 1) != 0)
        return JS::Value(static_cast<double>(static_cast<i32>(bits) >> 1));
    if (type.heap == HeapKind::Function || type.heap == HeapKind::NoFunction || type.heap == HeapKind::Concrete) {
        return JS::Value(static_cast<double>(bit_cast<FlatPtr>(WebAssemblyModule::store().function_from_reference(bits))));
    }
    return JS::js_undefined();
}

static JS::ThrowCompletionOr<JS::Value> to_js_value(JS::VM& vm, Wasm::Runtime::Value value, Wasm::Runtime::ValueType type)
{
    using namespace Wasm::Runtime;
    switch (type.kind) {
    case ValueKind::I32:
        return JS::Value(static_cast<double>(value.to_i32()));
    case ValueKind::I64:
        return JS::Value(JS::BigInt::create(vm, Crypto::SignedBigInteger { value.to_i64() }));
    case ValueKind::F32:
        return JS::Value(static_cast<double>(value.f32_bits()));
    case ValueKind::F64:
        return JS::Value(JS::BigInt::create(vm, Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { value.f64_bits() } }));
    case ValueKind::V128: {
        auto buffer = TRY(JS::ArrayBuffer::create(*vm.current_realm(), 16));
        u64 halves[2] = { value.low, value.high };
        buffer->overwrite(0, reinterpret_cast<u8 const*>(halves), sizeof(halves));
        return JS::Value(buffer);
    }
    case ValueKind::Reference:
        return reference_to_js_value(value, type);
    }
    VERIFY_NOT_REACHED();
}

static Wasm::Runtime::Entity function_entity(double number)
{
    return bit_cast<GC::ForeignCell*>(static_cast<FlatPtr>(number));
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyModule::get_export)
{
    using namespace Wasm::Runtime;
    auto name = TRY(vm.argument(0).to_utf16_string(vm)).to_utf8_but_should_be_ported_to_utf16();
    auto object = TRY(vm.this_value().to_object(vm));
    auto* instance = WebAssemblyModule::from_object(*object);
    if (!instance)
        return vm.throw_completion<JS::TypeError>("Not a WebAssemblyModule"_utf16);
    auto value = store().find_export(instance->instance(), name);
    if (!value.has_value())
        return vm.throw_completion<JS::TypeError>(Utf16String::formatted("'{}' could not be found", name));
    // NB: A function is its entity's address, which a double holds exactly.
    if (value->kind == ExternKind::Function)
        return JS::Value(static_cast<double>(bit_cast<FlatPtr>(value->entity.ptr())));
    if (value->kind == ExternKind::Global) {
        bool is_mutable = false;
        auto type = store().global_type(value->entity.ptr(), is_mutable);
        auto global = store().global_get(value->entity.ptr());
        switch (type.kind) {
        case ValueKind::I32:
            return JS::Value(static_cast<double>(global.to_i32()));
        case ValueKind::I64:
            return JS::BigInt::create(vm, Crypto::SignedBigInteger { global.to_i64() });
        case ValueKind::F32:
            return JS::Value(static_cast<double>(global.to_f32()));
        case ValueKind::F64:
            return JS::Value(global.to_f64());
        case ValueKind::V128: {
            u64 halves[2] = { global.low, global.high };
            return JS::BigInt::create(vm, Crypto::SignedBigInteger::import_data({ reinterpret_cast<u8 const*>(halves), sizeof(halves) }));
        }
        case ValueKind::Reference:
            return reference_to_js_value(global, type);
        }
    }
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("'{}' does not refer to a function or a global", name));
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyModule::wasm_invoke)
{
    using namespace Wasm::Runtime;
    auto address = function_entity(TRY(vm.argument(0).to_double(vm)));
    auto type = store().function_type(address);

    Vector<Value> arguments;
    if (type.parameters.size() + 1 > vm.argument_count())
        return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Expected {} arguments for call, but found {}", type.parameters.size() + 1, vm.argument_count()));
    size_t index = 1;
    for (auto parameter : type.parameters) {
        auto argument = vm.argument(index++);
        double double_value = 0;
        if (!argument.is_bigint() && !argument.is_object())
            double_value = TRY(argument.to_double(vm));
        switch (parameter.kind) {
        case ValueKind::I32:
            arguments.append(Value::from_i32(static_cast<i32>(static_cast<i64>(double_value))));
            break;
        case ValueKind::I64:
            if (argument.is_bigint())
                arguments.append(Value::from_i64(bit_cast<i64>(TRY(argument.to_bigint_uint64(vm)))));
            else
                arguments.append(Value::from_i64(static_cast<i64>(double_value)));
            break;
        case ValueKind::F32:
            arguments.append(Value::from_f32_bits(static_cast<u32>(double_value)));
            break;
        case ValueKind::F64:
            if (argument.is_bigint())
                arguments.append(Value::from_f64_bits(TRY(argument.to_bigint_uint64(vm))));
            else
                arguments.append(Value::from_f64(double_value));
            break;
        case ValueKind::V128: {
            auto object = MUST(argument.to_object(vm));
            if (!is<JS::TypedArrayBase>(*object))
                return vm.throw_completion<JS::TypeError>("Expected typed array"_utf16);
            auto& array = static_cast<JS::TypedArrayBase&>(*object);
            auto record = JS::make_typed_array_with_buffer_witness_record(array, JS::ArrayBuffer::Order::SeqCst);
            if (JS::typed_array_byte_length(record) < 16)
                return vm.throw_completion<JS::TypeError>("Expected at least 16 bytes"_utf16);
            u64 halves[2] = { 0, 0 };
            array.viewed_array_buffer()->copy_to(array.byte_offset(), { reinterpret_cast<u8*>(halves), sizeof(halves) });
            arguments.append(Value::from_v128(halves[0], halves[1]));
            break;
        }
        case ValueKind::Reference:
            if (argument.is_null()) {
                arguments.append(Value::from_reference(0));
                break;
            }
            switch (parameter.heap) {
            case HeapKind::Function:
                arguments.append(Value::from_reference(store().function_reference(function_entity(double_value))));
                break;
            case HeapKind::Extern:
            case HeapKind::Any:
                arguments.append(Value::from_reference(host_reference_bits(static_cast<u64>(double_value))));
                break;
            default:
                return vm.throw_completion<JS::TypeError>("GC Heap references are not supported"_utf16);
            }
            break;
        }
    }

    auto result = store().invoke(address, arguments);
    if (result.is_error())
        return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Execution trapped: {}", result.error().message));

    auto& values = result.value();
    if (values.is_empty())
        return JS::js_null();
    if (values.size() == 1)
        return to_js_value(vm, values.first(), type.results.first());

    GC::RootVector<JS::Value> js_values;
    for (size_t result_index = 0; result_index < values.size(); ++result_index)
        js_values.append(TRY(to_js_value(vm, values[result_index], type.results[result_index])));
    return JS::Array::create_from(*vm.current_realm(), js_values);
}
