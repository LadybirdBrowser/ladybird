/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibTest/JavaScriptTestRunner.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/Types.h>
#include <string.h>

TEST_ROOT("Libraries/LibWasm/Tests");

TESTJS_GLOBAL_FUNCTION(read_binary_wasm_file, readBinaryWasmFile)
{
    auto& realm = *vm.current_realm();

    auto error_code_to_string = [](int code) {
        auto const* error_string = strerror(code);
        return StringView { error_string, strlen(error_string) };
    };

    auto filename = TRY(vm.argument(0).to_string(vm));
    auto file = Core::File::open(filename, Core::File::OpenMode::Read);
    if (file.is_error())
        return vm.throw_completion<JS::TypeError>(error_code_to_string(file.error().code()));

    auto file_size = file.value()->size();
    if (file_size.is_error())
        return vm.throw_completion<JS::TypeError>(error_code_to_string(file_size.error().code()));

    auto array = TRY(JS::Uint8Array::create(realm, file_size.value()));

    auto read = file.value()->read_until_filled(array->data());
    if (read.is_error())
        return vm.throw_completion<JS::TypeError>(error_code_to_string(read.error().code()));

    return JS::Value(array);
}

class WebAssemblyModule final : public JS::Object {
    JS_OBJECT(WebAssemblyModule, JS::Object);
    GC_DECLARE_ALLOCATOR(WebAssemblyModule);

public:
    explicit WebAssemblyModule(JS::Object& prototype)
        : JS::Object(ConstructWithPrototypeTag::Tag, prototype)
    {
        m_machine.enable_instruction_count_limit();
    }

    static Wasm::AbstractMachine& machine() { return m_machine; }
    Wasm::Module& module() { return *m_module; }
    Wasm::ModuleInstance& module_instance() { return *m_module_instance; }

    static JS::ThrowCompletionOr<WebAssemblyModule*> create(JS::Realm& realm, NonnullRefPtr<Wasm::Module> module, HashMap<Wasm::Linker::Name, Wasm::ExternValue> const& imports)
    {
        auto& vm = realm.vm();
        auto instance = realm.create<WebAssemblyModule>(realm.intrinsics().object_prototype());
        instance->m_module = move(module);
        Wasm::Linker linker(*instance->m_module);
        linker.link(imports);
        linker.link(spec_test_namespace());
        auto link_result = linker.finish();
        if (link_result.is_error())
            return vm.throw_completion<JS::TypeError>("Link failed"sv);
        auto result = machine().instantiate(*instance->m_module, link_result.release_value());
        if (result.is_error())
            return vm.throw_completion<JS::TypeError>(result.release_error().error);
        instance->m_module_instance = result.release_value();
        return instance.ptr();
    }
    void initialize(JS::Realm&) override;

    ~WebAssemblyModule() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(get_export);
    JS_DECLARE_NATIVE_FUNCTION(wasm_invoke);

    static HashMap<Wasm::Linker::Name, Wasm::ExternValue> const& spec_test_namespace()
    {
        Wasm::FunctionType print_type { {}, {} };
        auto address_print = alloc_noop_function(print_type);
        s_spec_test_namespace.set({ "spectest", "print", print_type }, Wasm::ExternValue { *address_print });

        Wasm::FunctionType print_i32_type { { Wasm::ValueType(Wasm::ValueType::I32) }, {} };
        auto address_i32 = alloc_noop_function(print_i32_type);
        s_spec_test_namespace.set({ "spectest", "print_i32", print_i32_type }, Wasm::ExternValue { *address_i32 });

        Wasm::FunctionType print_i64_type { { Wasm::ValueType(Wasm::ValueType::I64) }, {} };
        auto address_i64 = alloc_noop_function(print_i64_type);
        s_spec_test_namespace.set({ "spectest", "print_i64", print_i64_type }, Wasm::ExternValue { *address_i64 });

        Wasm::FunctionType print_f32_type { { Wasm::ValueType(Wasm::ValueType::F32) }, {} };
        auto address_f32 = alloc_noop_function(print_f32_type);
        s_spec_test_namespace.set({ "spectest", "print_f32", print_f32_type }, Wasm::ExternValue { *address_f32 });

        Wasm::FunctionType print_f64_type { { Wasm::ValueType(Wasm::ValueType::F64) }, {} };
        auto address_f64 = alloc_noop_function(print_f64_type);
        s_spec_test_namespace.set({ "spectest", "print_f64", print_f64_type }, Wasm::ExternValue { *address_f64 });

        Wasm::FunctionType print_i32_f32_type { { Wasm::ValueType(Wasm::ValueType::I32), Wasm::ValueType(Wasm::ValueType::F32) }, {} };
        auto address_i32_f32 = alloc_noop_function(print_i32_f32_type);
        s_spec_test_namespace.set({ "spectest", "print_i32_f32", print_i32_f32_type }, Wasm::ExternValue { *address_i32_f32 });

        Wasm::FunctionType print_f64_f64_type { { Wasm::ValueType(Wasm::ValueType::F64), Wasm::ValueType(Wasm::ValueType::F64) }, {} };
        auto address_f64_f64 = alloc_noop_function(print_f64_f64_type);
        s_spec_test_namespace.set({ "spectest", "print_f64_f64", print_f64_f64_type }, Wasm::ExternValue { *address_f64_f64 });

        Wasm::TableType table_type { Wasm::ValueType(Wasm::ValueType::FunctionReference), Wasm::Limits(Wasm::AddressType::I32, 10, 20) };
        auto table_address = m_machine.store().allocate(table_type);
        s_spec_test_namespace.set({ "spectest", "table", table_type }, Wasm::ExternValue { *table_address });

        Wasm::MemoryType memory_type { Wasm::Limits(Wasm::AddressType::I32, 1, 2) };
        auto memory_address = m_machine.store().allocate(memory_type);
        s_spec_test_namespace.set({ "spectest", "memory", memory_type }, Wasm::ExternValue { *memory_address });

        Wasm::GlobalType global_i32 { Wasm::ValueType(Wasm::ValueType::I32), false };
        auto global_i32_address = m_machine.store().allocate(global_i32, Wasm::Value(666));
        s_spec_test_namespace.set({ "spectest", "global_i32", global_i32 }, Wasm::ExternValue { *global_i32_address });

        Wasm::GlobalType global_i64 { Wasm::ValueType(Wasm::ValueType::I64), false };
        auto global_i64_address = m_machine.store().allocate(global_i64, Wasm::Value((i64)666));
        s_spec_test_namespace.set({ "spectest", "global_i64", global_i64 }, Wasm::ExternValue { *global_i64_address });

        Wasm::GlobalType global_f32 { Wasm::ValueType(Wasm::ValueType::F32), false };
        auto global_f32_address = m_machine.store().allocate(global_f32, Wasm::Value(666.6f));
        s_spec_test_namespace.set({ "spectest", "global_f32", global_f32 }, Wasm::ExternValue { *global_f32_address });

        Wasm::GlobalType global_f64 { Wasm::ValueType(Wasm::ValueType::F64), false };
        auto global_f64_address = m_machine.store().allocate(global_f64, Wasm::Value(666.6));
        s_spec_test_namespace.set({ "spectest", "global_f64", global_f64 }, Wasm::ExternValue { *global_f64_address });

        return s_spec_test_namespace;
    }

    static Optional<Wasm::FunctionAddress> alloc_noop_function(Wasm::FunctionType type)
    {
        return m_machine.store().allocate(Wasm::HostFunction {
            [](auto&, auto) -> Wasm::Result {
                // Noop, this just needs to exist.
                return Wasm::Result { Vector<Wasm::Value> {} };
            },
            type,
            "__TEST" });
    }

    static HashMap<Wasm::Linker::Name, Wasm::ExternValue> s_spec_test_namespace;
    static Wasm::AbstractMachine m_machine;
    RefPtr<Wasm::Module> m_module;
    OwnPtr<Wasm::ModuleInstance> m_module_instance;
};

GC_DEFINE_ALLOCATOR(WebAssemblyModule);

Wasm::AbstractMachine WebAssemblyModule::m_machine;
HashMap<Wasm::Linker::Name, Wasm::ExternValue> WebAssemblyModule::s_spec_test_namespace;

TESTJS_GLOBAL_FUNCTION(parse_webassembly_module, parseWebAssemblyModule)
{
    auto& realm = *vm.current_realm();
    auto object = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::Uint8Array>(*object))
        return vm.throw_completion<JS::TypeError>("Expected a Uint8Array argument to parse_webassembly_module"sv);
    auto& array = static_cast<JS::Uint8Array&>(*object);
    FixedMemoryStream stream { array.data() };
    auto result = Wasm::Module::parse(stream);
    if (result.is_error())
        return vm.throw_completion<JS::SyntaxError>(Wasm::parse_error_to_byte_string(result.error()));

    HashMap<Wasm::Linker::Name, Wasm::ExternValue> imports;
    auto import_value = vm.argument(1);
    if (auto import_object = import_value.template as_if<JS::Object>()) {
        for (auto const& property : import_object->shape().property_table()) {
            auto module_object = import_object->get_without_side_effects(property.key).as_if<WebAssemblyModule>();
            if (!module_object)
                continue;
            for (auto& entry : module_object->module_instance().exports()) {
                // FIXME: Don't pretend that everything is a function
                imports.set({ property.key.as_string().to_utf16_string().to_byte_string(), entry.name(), Wasm::TypeIndex(0) }, entry.value());
            }
        }
    }

    return JS::Value(TRY(WebAssemblyModule::create(realm, result.release_value(), imports)));
}

TESTJS_GLOBAL_FUNCTION(compare_typed_arrays, compareTypedArrays)
{
    auto lhs = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::TypedArrayBase>(*lhs))
        return vm.throw_completion<JS::TypeError>("Expected a TypedArray"sv);
    auto& lhs_array = static_cast<JS::TypedArrayBase&>(*lhs);
    auto rhs = TRY(vm.argument(1).to_object(vm));
    if (!is<JS::TypedArrayBase>(*rhs))
        return vm.throw_completion<JS::TypeError>("Expected a TypedArray"sv);
    auto& rhs_array = static_cast<JS::TypedArrayBase&>(*rhs);
    return JS::Value(lhs_array.viewed_array_buffer()->buffer() == rhs_array.viewed_array_buffer()->buffer());
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

TESTJS_GLOBAL_FUNCTION(is_valid_funcref_in, isValidFuncrefIn)
{
    auto value = TRY(vm.argument(0).to_index(vm));
    auto module_object = TRY(vm.argument(1).to_object(vm));
    if (!is<WebAssemblyModule>(*module_object))
        return vm.throw_completion<JS::TypeError>("Expected a WebAssemblyModule"sv);
    auto& module = static_cast<WebAssemblyModule&>(*module_object);
    return JS::Value(module.machine().store().get(Wasm::FunctionAddress { value }) != nullptr);
}

TESTJS_GLOBAL_FUNCTION(test_simd_vector, testSIMDVector)
{
    auto expected = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::Array>(*expected))
        return vm.throw_completion<JS::TypeError>("Expected an Array"sv);
    auto& expected_array = static_cast<JS::Array&>(*expected);
    auto got = TRY(vm.argument(1).to_object(vm));
    if (!is<JS::TypedArrayBase>(*got))
        return vm.throw_completion<JS::TypeError>("Expected a TypedArray"sv);
    auto& got_array = static_cast<JS::TypedArrayBase&>(*got);
    auto element_size = 128 / TRY(TRY(expected_array.get("length"_utf16_fly_string)).to_u32(vm));
    size_t i = 0;
    for (auto it = expected_array.indexed_properties().begin(false); it != expected_array.indexed_properties().end(); ++it) {
        auto got_value = TRY(got_array.get(i++));
        u64 got = got_value.is_bigint() ? TRY(got_value.to_bigint_uint64(vm)) : (u64)TRY(got_value.to_index(vm));
        auto expect = TRY(expected_array.get(it.index()));
        if (expect.is_string()) {
            if (element_size != 32 && element_size != 64)
                return vm.throw_completion<JS::TypeError>("Expected element of size 32 or 64"sv);
            auto string = expect.as_string().utf8_string();
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
            return vm.throw_completion<JS::TypeError>(ByteString::formatted("Bad SIMD float expectation: {}", string));
        }
        u64 expect_value = expect.is_bigint() ? TRY(expect.to_bigint_uint64(vm)) : (u64)TRY(expect.to_index(vm));
        if (got != expect_value)
            return false;
    }
    return true;
}

void WebAssemblyModule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    define_native_function(realm, "getExport"_utf16_fly_string, get_export, 1, JS::default_attributes);
    define_native_function(realm, "invoke"_utf16_fly_string, wasm_invoke, 1, JS::default_attributes);
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyModule::get_export)
{
    auto name = TRY(vm.argument(0).to_string(vm));
    auto this_value = vm.this_value();
    auto object = TRY(this_value.to_object(vm));
    if (!is<WebAssemblyModule>(*object))
        return vm.throw_completion<JS::TypeError>("Not a WebAssemblyModule"sv);
    auto& instance = static_cast<WebAssemblyModule&>(*object);
    for (auto& entry : instance.module_instance().exports()) {
        if (entry.name() == name.to_byte_string()) {
            auto& value = entry.value();
            if (auto ptr = value.get_pointer<Wasm::FunctionAddress>())
                return JS::Value(static_cast<unsigned long>(ptr->value()));
            if (auto v = value.get_pointer<Wasm::GlobalAddress>()) {
                auto global = m_machine.store().get(*v);
                switch (global->type().type().kind()) {
                case Wasm::ValueType::I32:
                    return JS::Value(static_cast<double>(global->value().to<i32>()));
                case Wasm::ValueType::I64:
                    return JS::BigInt::create(vm, Crypto::SignedBigInteger { global->value().to<i64>() });
                case Wasm::ValueType::F32:
                    return JS::Value(static_cast<double>(global->value().to<float>()));
                case Wasm::ValueType::F64:
                    return JS::Value(global->value().to<double>());
                case Wasm::ValueType::V128: {
                    auto value = global->value().to<u128>();
                    return JS::BigInt::create(vm, Crypto::SignedBigInteger::import_data(value.bytes()));
                }
                case Wasm::ValueType::FunctionReference:
                case Wasm::ValueType::ExternReference:
                case Wasm::ValueType::ExceptionReference: {
                    auto ref = global->value().to<Wasm::Reference>();
                    return ref.ref().visit(
                        [&](Wasm::Reference::Null const&) -> JS::Value { return JS::js_null(); },
                        [](Wasm::Reference::Exception const&) -> JS::Value { return JS::js_undefined(); },
                        [&](auto const& ref) -> JS::Value { return JS::Value(static_cast<double>(ref.address.value())); });
                }
                case Wasm::ValueType::TypeUseReference:
                case Wasm::ValueType::UnsupportedHeapReference:
                    return vm.throw_completion<JS::TypeError>("Unsupported heap reference"sv);
                }
            }
            return vm.throw_completion<JS::TypeError>(TRY_OR_THROW_OOM(vm, String::formatted("'{}' does not refer to a function or a global", name)));
        }
    }
    return vm.throw_completion<JS::TypeError>(TRY_OR_THROW_OOM(vm, String::formatted("'{}' could not be found", name)));
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyModule::wasm_invoke)
{
    auto address = static_cast<unsigned long>(TRY(vm.argument(0).to_double(vm)));
    Wasm::FunctionAddress function_address { address };
    auto function_instance = WebAssemblyModule::machine().store().get(function_address);
    if (!function_instance)
        return vm.throw_completion<JS::TypeError>("Invalid function address"sv);

    Wasm::FunctionType const* type { nullptr };
    function_instance->visit([&](auto& value) { type = &value.type(); });
    if (!type)
        return vm.throw_completion<JS::TypeError>("Invalid function found at given address"sv);

    Vector<Wasm::Value> arguments;
    if (type->parameters().size() + 1 > vm.argument_count())
        return vm.throw_completion<JS::TypeError>(TRY_OR_THROW_OOM(vm, String::formatted("Expected {} arguments for call, but found {}", type->parameters().size() + 1, vm.argument_count())));
    size_t index = 1;
    for (auto& param : type->parameters()) {
        auto argument = vm.argument(index++);
        double double_value = 0;
        if (!argument.is_bigint() && !argument.is_object())
            double_value = TRY(argument.to_double(vm));
        switch (param.kind()) {
        case Wasm::ValueType::Kind::I32:
            arguments.append(Wasm::Value(static_cast<i64>(double_value)));
            break;
        case Wasm::ValueType::Kind::I64:
            if (argument.is_bigint()) {
                auto value = TRY(argument.to_bigint_int64(vm));
                arguments.append(Wasm::Value(value));
            } else {
                arguments.append(Wasm::Value(static_cast<i64>(double_value)));
            }
            break;
        case Wasm::ValueType::Kind::F32:
            arguments.append(Wasm::Value(bit_cast<float>(static_cast<u32>(double_value))));
            break;
        case Wasm::ValueType::Kind::F64:
            if (argument.is_bigint()) {
                auto value = TRY(argument.to_bigint_uint64(vm));
                arguments.append(Wasm::Value(bit_cast<double>(value)));
            } else {
                arguments.append(Wasm::Value(double_value));
            }
            break;
        case Wasm::ValueType::Kind::V128: {
            auto object = MUST(argument.to_object(vm));
            if (!is<JS::TypedArrayBase>(*object))
                return vm.throw_completion<JS::TypeError>("Expected typed array"sv);
            auto& array = static_cast<JS::TypedArrayBase&>(*object);
            u128 bits = 0;
            auto* ptr = bit_cast<u8*>(&bits);
            memcpy(ptr, array.viewed_array_buffer()->buffer().data(), 16);
            arguments.append(Wasm::Value(bits));
            break;
        }
        case Wasm::ValueType::Kind::FunctionReference: {
            if (argument.is_null()) {
                arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::FunctionReference) } }));
                break;
            }
            Wasm::FunctionAddress addr = static_cast<u64>(double_value);
            arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Func { addr, machine().store().get_module_for(addr) } }));
            break;
        }
        case Wasm::ValueType::Kind::ExternReference:
            if (argument.is_null()) {
                arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::ExternReference) } }));
                break;
            }
            arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Extern { static_cast<u64>(double_value) } }));
            break;
        case Wasm::ValueType::Kind::ExceptionReference:
            if (argument.is_null())
                arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::ExceptionReference) } }));
            else
                return vm.throw_completion<JS::TypeError>("Exception references are not supported"sv);
            break;
        case Wasm::ValueType::Kind::TypeUseReference:
            if (argument.is_null())
                arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::TypeUseReference, param.unsafe_typeindex()) } }));
            else
                return vm.throw_completion<JS::TypeError>("GC Heap references are not supported"sv);
            break;
        case Wasm::ValueType::Kind::UnsupportedHeapReference:
            return vm.throw_completion<JS::TypeError>("GC Heap references are not supported"sv);
        }
    }

    auto functype = WebAssemblyModule::machine().store().get(function_address)->visit([&](auto& func) { return func.type(); });
    auto result = WebAssemblyModule::machine().invoke(function_address, arguments);
    if (result.is_trap()) {
        if (auto ptr = result.trap().data.get_pointer<Wasm::ExternallyManagedTrap>())
            return ptr->unsafe_external_object_as<JS::Completion>();
        return vm.throw_completion<JS::TypeError>(TRY_OR_THROW_OOM(vm, String::formatted("Execution trapped: {}", result.trap().format())));
    }

    if (result.values().is_empty())
        return JS::js_null();

    auto to_js_value = [&](Wasm::Value const& value, Wasm::ValueType type) -> JS::ThrowCompletionOr<JS::Value> {
        switch (type.kind()) {
        case Wasm::ValueType::I32:
            return JS::Value(static_cast<double>(value.to<i32>()));
        case Wasm::ValueType::I64:
            return JS::Value(JS::BigInt::create(vm, Crypto::SignedBigInteger { value.to<i64>() }));
        case Wasm::ValueType::F32:
            return JS::Value(static_cast<double>(bit_cast<u32>(value.to<float>())));
        case Wasm::ValueType::F64:
            return JS::Value(JS::BigInt::create(vm, Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { bit_cast<u64>(value.to<double>()) } }));
        case Wasm::ValueType::V128: {
            u128 val = value.to<u128>();
            // FIXME: remove the MUST here
            auto buf = MUST(JS::ArrayBuffer::create(*vm.current_realm(), 16));
            memcpy(buf->buffer().data(), val.bytes().data(), 16);
            return JS::Value(buf);
        }
        case Wasm::ValueType::FunctionReference:
        case Wasm::ValueType::ExternReference:
            return (value.to<Wasm::Reference>()).ref().visit([&](Wasm::Reference::Null) { return JS::js_null(); }, [&](Wasm::Reference::Exception) { return JS::Value(); }, [&](auto const& ref) { return JS::Value(static_cast<double>(ref.address.value())); });
        case Wasm::ValueType::ExceptionReference:
            return JS::js_null();
        case Wasm::ValueType::TypeUseReference:
            return JS::js_null();
        case Wasm::ValueType::UnsupportedHeapReference:
            return vm.throw_completion<JS::TypeError>("Unsupported heap reference"sv);
        }
        VERIFY_NOT_REACHED();
    };

    if (result.values().size() == 1)
        return to_js_value(result.values().first(), functype.results().first());

    size_t i = 0;
    return JS::Array::create_from<Wasm::Value>(*vm.current_realm(), result.values(), [&](Wasm::Value value) {
        auto value_type = type->results()[i++];
        return MUST(to_js_value(value, value_type));
    });
}
