/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibTest/JavaScriptTestRunner.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/Types.h>
#include <string.h>

TEST_ROOT("Userland/Libraries/LibWasm/Tests");

TESTJS_GLOBAL_FUNCTION(read_binary_wasm_file, readBinaryWasmFile)
{
    auto& realm = *vm.current_realm();

    auto error_code_to_string = [](int code) {
        auto const* error_string = strerror(code);
        return StringView { error_string, strlen(error_string) };
    };

    auto filename = TRY(vm.argument(0).to_byte_string(vm));
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

public:
    explicit WebAssemblyModule(JS::Object& prototype)
        : JS::Object(ConstructWithPrototypeTag::Tag, prototype)
    {
        m_machine.enable_instruction_count_limit();
    }

    static Wasm::AbstractMachine& machine() { return m_machine; }
    Wasm::Module& module() { return *m_module; }
    Wasm::ModuleInstance& module_instance() { return *m_module_instance; }

    static JS::ThrowCompletionOr<WebAssemblyModule*> create(JS::Realm& realm, Wasm::Module module, HashMap<Wasm::Linker::Name, Wasm::ExternValue> const& imports)
    {
        auto& vm = realm.vm();
        auto instance = realm.heap().allocate<WebAssemblyModule>(realm, realm.intrinsics().object_prototype());
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

        Wasm::TableType table_type { Wasm::ValueType(Wasm::ValueType::FunctionReference), Wasm::Limits(10, 20) };
        auto table_address = m_machine.store().allocate(table_type);
        s_spec_test_namespace.set({ "spectest", "table", table_type }, Wasm::ExternValue { *table_address });

        Wasm::MemoryType memory_type { Wasm::Limits(1, 2) };
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
            [](auto&, auto&) -> Wasm::Result {
                // Noop, this just needs to exist.
                return Wasm::Result { Vector<Wasm::Value> {} };
            },
            type });
    }

    static HashMap<Wasm::Linker::Name, Wasm::ExternValue> s_spec_test_namespace;
    static Wasm::AbstractMachine m_machine;
    Optional<Wasm::Module> m_module;
    OwnPtr<Wasm::ModuleInstance> m_module_instance;
};

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
    if (import_value.is_object()) {
        auto& import_object = import_value.as_object();
        for (auto& property : import_object.shape().property_table()) {
            auto value = import_object.get_without_side_effects(property.key);
            if (!value.is_object() || !is<WebAssemblyModule>(value.as_object()))
                continue;
            auto& module_object = static_cast<WebAssemblyModule&>(value.as_object());
            for (auto& entry : module_object.module_instance().exports()) {
                // FIXME: Don't pretend that everything is a function
                imports.set({ property.key.as_string(), entry.name(), Wasm::TypeIndex(0) }, entry.value());
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

void WebAssemblyModule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    define_native_function(realm, "getExport", get_export, 1, JS::default_attributes);
    define_native_function(realm, "invoke", wasm_invoke, 1, JS::default_attributes);
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyModule::get_export)
{
    auto name = TRY(vm.argument(0).to_byte_string(vm));
    auto this_value = vm.this_value();
    auto object = TRY(this_value.to_object(vm));
    if (!is<WebAssemblyModule>(*object))
        return vm.throw_completion<JS::TypeError>("Not a WebAssemblyModule"sv);
    auto& instance = static_cast<WebAssemblyModule&>(*object);
    for (auto& entry : instance.module_instance().exports()) {
        if (entry.name() == name) {
            auto& value = entry.value();
            if (auto ptr = value.get_pointer<Wasm::FunctionAddress>())
                return JS::Value(static_cast<unsigned long>(ptr->value()));
            if (auto v = value.get_pointer<Wasm::GlobalAddress>()) {
                return m_machine.store().get(*v)->value().value().visit(
                    [&](auto const& value) -> JS::Value { return JS::Value(static_cast<double>(value)); },
                    [&](i32 value) { return JS::Value(static_cast<double>(value)); },
                    [&](i64 value) -> JS::Value { return JS::BigInt::create(vm, Crypto::SignedBigInteger { value }); },
                    [&](u128 value) -> JS::Value { return JS::BigInt::create(vm, Crypto::SignedBigInteger::import_data(bit_cast<u8 const*>(&value), sizeof(value))); },
                    [&](Wasm::Reference const& reference) -> JS::Value {
                        return reference.ref().visit(
                            [&](Wasm::Reference::Null const&) -> JS::Value { return JS::js_null(); },
                            [&](auto const& ref) -> JS::Value { return JS::Value(static_cast<double>(ref.address.value())); });
                    });
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
        if (!argument.is_bigint())
            double_value = TRY(argument.to_double(vm));
        switch (param.kind()) {
        case Wasm::ValueType::Kind::I32:
            arguments.append(Wasm::Value(param, static_cast<i64>(double_value)));
            break;
        case Wasm::ValueType::Kind::I64:
            if (argument.is_bigint()) {
                auto value = TRY(argument.to_bigint_int64(vm));
                arguments.append(Wasm::Value(param, value));
            } else {
                arguments.append(Wasm::Value(param, static_cast<i64>(double_value)));
            }
            break;
        case Wasm::ValueType::Kind::F32:
            arguments.append(Wasm::Value(static_cast<float>(double_value)));
            break;
        case Wasm::ValueType::Kind::F64:
            arguments.append(Wasm::Value(static_cast<double>(double_value)));
            break;
        case Wasm::ValueType::Kind::V128: {
            if (!argument.is_bigint()) {
                if (argument.is_number())
                    argument = JS::BigInt::create(vm, Crypto::SignedBigInteger { TRY(argument.to_double(vm)) });
                else
                    argument = TRY(argument.to_bigint(vm));
            }

            u128 bits = 0;
            auto bytes = argument.as_bigint().big_integer().unsigned_value().export_data({ bit_cast<u8*>(&bits), sizeof(bits) });
            VERIFY(!argument.as_bigint().big_integer().is_negative());

            if constexpr (AK::HostIsLittleEndian)
                arguments.append(Wasm::Value(bits << (128 - bytes * 8)));
            else
                arguments.append(Wasm::Value(bits >> (128 - bytes * 8)));
            break;
        }
        case Wasm::ValueType::Kind::FunctionReference:
            if (argument.is_null()) {
                arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::FunctionReference) } }));
                break;
            }
            arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Func { static_cast<u64>(double_value) } }));
            break;
        case Wasm::ValueType::Kind::ExternReference:
            if (argument.is_null()) {
                arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::ExternReference) } }));
                break;
            }
            arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Extern { static_cast<u64>(double_value) } }));
            break;
        case Wasm::ValueType::Kind::NullFunctionReference:
            arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::FunctionReference) } }));
            break;
        case Wasm::ValueType::Kind::NullExternReference:
            arguments.append(Wasm::Value(Wasm::Reference { Wasm::Reference::Null { Wasm::ValueType(Wasm::ValueType::Kind::ExternReference) } }));
            break;
        }
    }

    auto result = WebAssemblyModule::machine().invoke(function_address, arguments);
    if (result.is_trap())
        return vm.throw_completion<JS::TypeError>(TRY_OR_THROW_OOM(vm, String::formatted("Execution trapped: {}", result.trap().reason)));

    if (result.is_completion())
        return result.completion();

    if (result.values().is_empty())
        return JS::js_null();

    auto to_js_value = [&](Wasm::Value const& value) {
        return value.value().visit(
            [](auto const& value) { return JS::Value(static_cast<double>(value)); },
            [](i32 value) { return JS::Value(static_cast<double>(value)); },
            [&](i64 value) { return JS::Value(JS::BigInt::create(vm, Crypto::SignedBigInteger { value })); },
            [&](u128 value) {
                auto unsigned_bigint_value = Crypto::UnsignedBigInteger::import_data(bit_cast<u8 const*>(&value), sizeof(value));
                return JS::Value(JS::BigInt::create(vm, Crypto::SignedBigInteger(move(unsigned_bigint_value), false)));
            },
            [](Wasm::Reference const& reference) {
                return reference.ref().visit(
                    [](Wasm::Reference::Null const&) { return JS::js_null(); },
                    [](auto const& ref) { return JS::Value(static_cast<double>(ref.address.value())); });
            });
    };

    if (result.values().size() == 1)
        return to_js_value(result.values().first());

    return JS::Array::create_from<Wasm::Value>(*vm.current_realm(), result.values(), [&](Wasm::Value value) {
        return to_js_value(value);
    });
}
