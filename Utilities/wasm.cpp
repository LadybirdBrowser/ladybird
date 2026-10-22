/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/UFixedBigInt.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/MappedFile.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibMain/Main.h>
#include <LibWasm/Runtime.h>
#include <math.h>

using Wasm::Runtime::Entity;
using Wasm::Runtime::ExternKind;
using Wasm::Runtime::HeapKind;
using Wasm::Runtime::HostError;
using Wasm::Runtime::ValueKind;
using Wasm::Runtime::ValueType;
using WasmValue = Wasm::Runtime::Value;

struct BenchmarkTimings {
    JsonArray parse;
    JsonArray validation;
    JsonArray native_compilation;
    JsonArray instantiation;
    JsonArray execution;

    JsonObject to_json() const
    {
        JsonObject json;
        json.set("parse_time"sv, parse);
        json.set("validation_time"sv, validation);
        json.set("native_compilation_time"sv, native_compilation);
        json.set("instantiation_time"sv, instantiation);
        json.set("execution_time"sv, execution);
        return json;
    }
};

struct ParsedValue {
    WasmValue value;
    ValueType type;
};

struct ImportName {
    ByteString module;
    ByteString name;

    bool operator==(ImportName const&) const = default;
};

template<>
struct AK::Traits<ImportName> : public DefaultTraits<ImportName> {
    static unsigned hash(ImportName const& name) { return pair_int_hash(name.module.hash(), name.name.hash()); }
};

static Optional<u128> convert_to_uint(StringView string)
{
    if (string.is_empty())
        return {};

    u128 value = 0;
    for (auto character : string) {
        if (character < '0' || character > '9')
            return {};
        value *= 10;
        value += u128 { static_cast<u64>(character - '0'), 0 };
    }
    return value;
}

static Optional<u128> convert_to_uint_from_hex(StringView string)
{
    if (string.is_empty())
        return {};

    u128 value = 0;
    for (auto digit : string) {
        if (value > (NumericLimits<u128>::max() >> 4))
            return {};
        auto digit_value = decode_hex_digit(digit);
        if (digit_value == 255)
            return {};
        value = (value << 4) + digit_value;
    }
    return value;
}

static WasmValue v128_value(u128 value)
{
    return WasmValue::from_v128(value.low(), value.high());
}

static ErrorOr<ParsedValue> parse_value(StringView spec)
{
    constexpr auto is_separator = [](char c) { return is_ascii_space(c) || c == ':'; };

    // Scalar: 'T.const[:\s]v' (i32.const 42)
    auto parse_integer = []<typename T>(StringView text) -> ErrorOr<T> {
        if (auto value = text.to_number<T>(); value.has_value())
            return *value;
        return Error::from_string_literal("Invalid scalar value");
    };
    auto parse_float = []<typename T>(StringView text) -> ErrorOr<T> {
        if (text.trim_whitespace().equals_ignoring_ascii_case("nan"sv))
            return static_cast<T>(NAN);
        if (text.trim_whitespace().equals_ignoring_ascii_case("inf"sv))
            return static_cast<T>(INFINITY);
        if (auto value = text.to_number<T>(); value.has_value())
            return *value;
        return Error::from_string_literal("Invalid scalar value");
    };

    // Vector: 'v128.const[:\s]v' (v128.const 0x01000000020000000300000004000000) or 'v(T.const[:\s]v, ...)'.
    auto parse_u128 = [](StringView text) -> ErrorOr<u128> {
        auto value = text.starts_with("0x"sv) ? convert_to_uint_from_hex(text.substring_view(2)) : convert_to_uint(text);
        if (!value.has_value())
            return Error::from_string_literal("Invalid v128 value");
        return *value;
    };

    GenericLexer lexer(spec);
    auto rest = [&] {
        lexer.ignore_while(is_separator);
        return lexer.consume_all();
    };
    if (lexer.consume_specific("v128.const"sv))
        return ParsedValue { v128_value(TRY(parse_u128(rest()))), ValueType::scalar(ValueKind::V128) };
    if (lexer.consume_specific("i8.const"sv))
        return ParsedValue { WasmValue::from_i32(TRY(parse_integer.operator()<i8>(rest()))), ValueType::scalar(ValueKind::I32) };
    if (lexer.consume_specific("i16.const"sv))
        return ParsedValue { WasmValue::from_i32(TRY(parse_integer.operator()<i16>(rest()))), ValueType::scalar(ValueKind::I32) };
    if (lexer.consume_specific("i32.const"sv))
        return ParsedValue { WasmValue::from_i32(TRY(parse_integer.operator()<i32>(rest()))), ValueType::scalar(ValueKind::I32) };
    if (lexer.consume_specific("i64.const"sv))
        return ParsedValue { WasmValue::from_i64(TRY(parse_integer.operator()<i64>(rest()))), ValueType::scalar(ValueKind::I64) };
    if (lexer.consume_specific("f32.const"sv))
        return ParsedValue { WasmValue::from_f32(TRY(parse_float.operator()<float>(rest()))), ValueType::scalar(ValueKind::F32) };
    if (lexer.consume_specific("f64.const"sv))
        return ParsedValue { WasmValue::from_f64(TRY(parse_float.operator()<double>(rest()))), ValueType::scalar(ValueKind::F64) };

    if (lexer.consume_specific("v("sv)) {
        Vector<ParsedValue> values;
        for (;;) {
            lexer.ignore_while(is_separator);
            if (lexer.consume_specific(")"sv))
                break;
            if (lexer.is_eof()) {
                warnln("Expected ')' to close vector");
                break;
            }
            values.append(TRY(parse_value(lexer.consume_until(is_any_of(",)"sv)))));
            lexer.consume_specific(',');
        }
        if (values.is_empty())
            return Error::from_string_literal("Empty vector");

        auto element_kind = values.first().type.kind;
        unsigned width = 0;
        switch (element_kind) {
        case ValueKind::I32:
        case ValueKind::F32:
            width = 32;
            break;
        case ValueKind::I64:
        case ValueKind::F64:
            width = 64;
            break;
        default:
            return Error::from_string_literal("Invalid vector element");
        }

        u128 result = 0;
        u128 last_value = 0;
        unsigned total_size = 0;
        for (auto& parsed : values) {
            if (parsed.type.kind != element_kind)
                return Error::from_string_literal("Mixed types in vector");
            if (total_size >= 128)
                return Error::from_string_literal("Vector too large");
            last_value = width == 32 ? u128 { parsed.value.low & 0xffff'ffff, 0 } : u128 { parsed.value.low, 0 };
            result |= last_value << total_size;
            total_size += width;
        }
        if (total_size < 128)
            warnln("Vector value '{}' is only {} bytes wide, repeating last element", spec, total_size / 8);
        while (total_size < 128) {
            result |= last_value << total_size;
            total_size += width;
        }
        return ParsedValue { v128_value(result), ValueType::scalar(ValueKind::V128) };
    }

    return Error::from_string_literal("Invalid value");
}

static RefPtr<Wasm::Runtime::Module> compile(StringView filename)
{
    auto file = Core::MappedFile::map(filename);
    if (file.is_error()) {
        warnln("Failed to open {}: {}", filename, file.error());
        return {};
    }
    auto module = Wasm::Runtime::Module::compile(file.value()->bytes());
    if (module.is_error()) {
        warnln("Failed to compile {}: {}", filename, module.error());
        return {};
    }
    return module.release_value();
}

static ByteString format_value(WasmValue value, ValueType type)
{
    switch (type.kind) {
    case ValueKind::I32:
        return ByteString::formatted("{}", value.to_i32());
    case ValueKind::I64:
        return ByteString::formatted("{}", value.to_i64());
    case ValueKind::F32:
        return ByteString::formatted("{}", value.to_f32());
    case ValueKind::F64:
        return ByteString::formatted("{}", value.to_f64());
    case ValueKind::V128:
        return ByteString::formatted("v128({:x})", u128 { value.low, value.high });
    case ValueKind::Reference:
        if (value.is_null_reference())
            return "addr(null)";
        return ByteString::formatted("addr({:#x})", value.reference_bits());
    }
    VERIFY_NOT_REACHED();
}

static Optional<ValueKind> consume_scalar_type(GenericLexer& lexer)
{
    if (lexer.consume_specific("i32"sv))
        return ValueKind::I32;
    if (lexer.consume_specific("i64"sv))
        return ValueKind::I64;
    if (lexer.consume_specific("f32"sv))
        return ValueKind::F32;
    if (lexer.consume_specific("f64"sv))
        return ValueKind::F64;
    if (lexer.consume_specific("v128"sv))
        return ValueKind::V128;
    return {};
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView filename;
    bool attempt_instantiate = false;
    bool export_all_imports = false;
    bool benchmark_timings = false;
    bool wasi = false;
    ByteString exported_function_to_execute;
    Vector<ParsedValue> values_to_push;
    // The modules to link in, and the module name importers know each by.
    Vector<ImportName> modules_to_link_in;
    Vector<StringView> args_if_wasi;
    Vector<StringView> wasi_preopened_mappings;
    HashMap<ImportName, Wasm::Runtime::Extern> js_exports;

    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;
    IGNORE_USE_IN_ESCAPING_LAMBDA Wasm::Runtime::Store store { vm->heap() };
    // NB: The entities the utility allocates live until it exits.
    Vector<GC::Root<GC::ForeignCell>> roots;
    auto keep = [&](Entity entity) {
        roots.append(GC::Root<GC::ForeignCell> { entity });
        return entity;
    };
    IGNORE_USE_IN_ESCAPING_LAMBDA Entity main_instance = nullptr;

    Core::ArgsParser parser;
    parser.add_positional_argument(filename, "File name to run", "file");
    parser.add_option(attempt_instantiate, "Attempt to instantiate the module", "instantiate", 'i');
    parser.add_option(exported_function_to_execute, "Attempt to execute the named exported function from the module (implies -i)", "execute", 'e', "name");
    parser.add_option(export_all_imports, "Export noop functions corresponding to imports", "export-noop");
    parser.add_option(benchmark_timings, "Emit machine-readable Wasm phase timings to stderr", "benchmark-timings");
    parser.add_option(wasi, "Enable WASI", "wasi", 'w');
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Export js `function(arg...) { source }` returning T as [module].[function]",
        .long_name = "export-js",
        .short_name = 0,
        .value_name = "module.function(arg:T...):T=source",
        .accept_value = [&](StringView text) {
            GenericLexer lexer(text);
            // [module] <.> [function] <(> {[name] <:> [type]} <)> (<:> [type])? <=> [text]
            auto module = lexer.consume_until('.');
            if (!lexer.consume_specific('.')) {
                warnln("Invalid JS export module in '{}'", text);
                return false;
            }
            auto function_name = lexer.consume_until(is_any_of("(=:"sv));
            struct Parameter {
                ValueKind kind;
                StringView name;
            };
            Vector<Parameter> parameters;
            if (lexer.consume_specific('(')) {
                while (!lexer.consume_specific(')')) {
                    auto name = lexer.consume_until(is_any_of(",:)"sv));
                    if (name.is_empty()) {
                        warnln("Invalid JS export argument name in '{}'", text);
                        return false;
                    }
                    auto kind = ValueKind::I32;
                    if (lexer.consume_specific(':')) {
                        auto type = consume_scalar_type(lexer);
                        if (!type.has_value()) {
                            warnln("Invalid JS export argument type in '{}'", text);
                            return false;
                        }
                        kind = *type;
                    }
                    parameters.append({ kind, name });
                    lexer.consume_specific(',');
                }
            }
            Optional<ValueKind> result_kind;
            if (lexer.consume_specific(':')) {
                result_kind = consume_scalar_type(lexer);
                if (!result_kind.has_value()) {
                    warnln("Invalid JS export return type in '{}'", text);
                    return false;
                }
            }
            if (!lexer.consume_specific('=') || lexer.is_eof()) {
                warnln("Invalid JS export source in '{}'", text);
                return false;
            }

            Utf16StringBuilder builder;
            builder.append_ascii("(function ("sv);
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (index != 0)
                    builder.append_ascii(", "sv);
                auto parameter_name = Utf16String::from_utf8(parameters[index].name);
                builder.append(parameter_name.utf16_view());
            }
            builder.append_ascii(") { return ("sv);
            auto source = Utf16String::from_utf8(lexer.consume_all().trim_whitespace());
            builder.append(source.utf16_view());
            builder.append_ascii("); })"sv);
            auto name = ByteString::formatted("{}.{}", module, function_name);
            auto script = JS::Script::parse(builder.to_string(), realm, name);
            if (script.is_error()) {
                warnln("Failed to parse JS export source of '{}'", name);
                return false;
            }
            auto function_value = vm->run(*script.release_value());
            if (function_value.is_error() || !function_value.value().is_function()) {
                warnln("JS export source of '{}' did not evaluate to a function", name);
                return false;
            }
            auto function = function_value.value();

            Wasm::Runtime::FunctionType function_type;
            for (auto& parameter : parameters)
                function_type.parameters.append(ValueType::scalar(parameter.kind));
            if (result_kind.has_value())
                function_type.results.append(ValueType::scalar(*result_kind));

            auto host_function = store.allocate_host_function(function_type, [&vm, &realm, function, &store, &main_instance, function_type, name](Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results) mutable -> ErrorOr<void, HostError> {
                GC::RootVector<JS::Value> js_arguments;
                for (size_t index = 0; index < arguments.size(); ++index) {
                    auto argument = arguments[index];
                    switch (function_type.parameters[index].kind) {
                    case ValueKind::I32:
                        js_arguments.append(JS::Value(static_cast<u32>(argument.to_i32())));
                        break;
                    case ValueKind::I64:
                        js_arguments.append(JS::Value(static_cast<double>(static_cast<u64>(argument.to_i64()))));
                        break;
                    case ValueKind::F32:
                        js_arguments.append(JS::Value(static_cast<double>(argument.to_f32())));
                        break;
                    case ValueKind::F64:
                        js_arguments.append(JS::Value(argument.to_f64()));
                        break;
                    case ValueKind::V128: {
                        u64 halves[2] = { argument.low, argument.high };
                        js_arguments.append(JS::BigInt::create(*vm, Crypto::SignedBigInteger { Crypto::UnsignedBigInteger::import_data({ reinterpret_cast<u8 const*>(halves), sizeof(halves) }) }));
                        break;
                    }
                    case ValueKind::Reference:
                        return HostError { "Unsupported argument type" };
                    }
                }

                // The function's `this` can call back into the main instance's exports.
                auto reentry_this = JS::Object::create(realm, realm.intrinsics().object_prototype());
                auto invoke = JS::NativeFunction::create(
                    realm, [&store, &main_instance](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                        if (!main_instance)
                            return vm.throw_completion<JS::TypeError>("wasm reentry: instance not ready yet"_utf16);
                        auto export_name = TRY(vm.argument(0).to_utf16_string(vm)).to_byte_string();
                        auto callee = store.find_export(main_instance, export_name);
                        if (!callee.has_value() || callee->kind != ExternKind::Function)
                            return vm.throw_completion<JS::TypeError>(Utf16String::formatted("wasm reentry: no exported function '{}'", export_name));
                        auto type = store.function_type(callee->entity.ptr());
                        Wasm::Runtime::Values wasm_arguments;
                        for (size_t index = 0; index < type.parameters.size(); ++index) {
                            auto value = vm.argument(index + 1);
                            switch (type.parameters[index].kind) {
                            case ValueKind::I32:
                                wasm_arguments.append(WasmValue::from_i32(static_cast<i32>(TRY(value.to_u32(vm)))));
                                break;
                            case ValueKind::I64:
                                wasm_arguments.append(WasmValue::from_i64(static_cast<i64>(TRY(value.to_bigint_uint64(vm)))));
                                break;
                            case ValueKind::F32:
                                wasm_arguments.append(WasmValue::from_f32(static_cast<float>(TRY(value.to_double(vm)))));
                                break;
                            case ValueKind::F64:
                                wasm_arguments.append(WasmValue::from_f64(TRY(value.to_double(vm))));
                                break;
                            default:
                                return vm.throw_completion<JS::TypeError>("wasm reentry: unsupported parameter type"_utf16);
                            }
                        }
                        auto result = store.invoke(callee->entity.ptr(), wasm_arguments);
                        if (result.is_error())
                            return vm.throw_completion<JS::TypeError>(Utf16String::formatted("wasm reentry trapped: {}", result.error().message));
                        if (result.value().is_empty())
                            return JS::js_undefined();
                        if (type.results.size() > 1)
                            return vm.throw_completion<JS::TypeError>("wasm reentry: multi-value results are not yet supported"_utf16);
                        auto returned = result.value().first();
                        switch (type.results.first().kind) {
                        case ValueKind::I32:
                            return JS::Value(returned.to_i32());
                        case ValueKind::I64:
                            return JS::Value(static_cast<double>(returned.to_i64()));
                        case ValueKind::F32:
                            return JS::Value(static_cast<double>(returned.to_f32()));
                        case ValueKind::F64:
                            return JS::Value(returned.to_f64());
                        default:
                            return JS::js_undefined();
                        }
                    },
                    1, "invoke"_utf16);
                reentry_this->define_direct_property("invoke"_utf16, invoke, JS::default_attributes);

                auto result = JS::call(*vm, function, reentry_this, js_arguments.span());
                if (result.is_error()) {
                    warnln("JS exception: {}", MUST(result.error_value().to_utf16_string(*vm)));
                    return HostError { "JS exception" };
                }
                if (function_type.results.is_empty())
                    return {};

                auto value = result.value();
                auto converted = [&]() -> JS::ThrowCompletionOr<WasmValue> {
                    switch (function_type.results.first().kind) {
                    case ValueKind::I32:
                        return WasmValue::from_i32(static_cast<i32>(TRY(value.to_u32(*vm))));
                    case ValueKind::I64:
                        return WasmValue::from_i64(static_cast<i64>(TRY(value.to_bigint_uint64(*vm))));
                    case ValueKind::F32:
                        return WasmValue::from_f32(static_cast<float>(TRY(value.to_double(*vm))));
                    case ValueKind::F64:
                        return WasmValue::from_f64(TRY(value.to_double(*vm)));
                    case ValueKind::V128: {
                        auto bigint = TRY(value.to_bigint(*vm));
                        u64 halves[2] = {};
                        (void)bigint->big_integer().unsigned_value().export_data({ reinterpret_cast<u8*>(halves), sizeof(halves) });
                        return WasmValue::from_v128(halves[0], halves[1]);
                    }
                    case ValueKind::Reference:
                        break;
                    }
                    VERIFY_NOT_REACHED();
                }();
                if (converted.is_error()) {
                    warnln("JS export function '{}' returned a value of the wrong type", name);
                    return HostError { "JS exception" };
                }
                results[0] = converted.release_value();
                return {};
            });
            js_exports.set({ module, function_name }, { ExternKind::Function, keep(host_function) });
            return true;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Directory mappings to expose via WASI",
        .long_name = "wasi-map-dir",
        .short_name = 0,
        .value_name = "path[:path]",
        .accept_value = [&](StringView text) {
            if (text.is_empty())
                return false;
            wasi_preopened_mappings.append(text);
            return true;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Extra modules to link with, whose exports resolve imports of the module name (default: the file's name without its extension)",
        .long_name = "link",
        .short_name = 'l',
        .value_name = "[name=]file",
        .accept_value = [&](StringView text) {
            if (text.is_empty())
                return false;
            if (auto separator = text.find('='); separator.has_value()) {
                modules_to_link_in.append({ text.substring_view(0, *separator), text.substring_view(*separator + 1) });
                return true;
            }
            modules_to_link_in.append({ LexicalPath::title(text), text });
            return true;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Supply arguments to the function (default=0) (T.const:v or v(T.const:v, ...))",
        .long_name = "arg",
        .short_name = 0,
        .value_name = "value",
        .accept_value = [&](StringView text) -> bool {
            auto result = parse_value(text);
            if (result.is_error()) {
                warnln("Failed to parse value: {}", result.error());
                return false;
            }
            values_to_push.append(result.release_value());
            return true;
        },
    });
    parser.add_positional_argument(args_if_wasi, "Arguments to pass to the WASI module", "args", Core::ArgsParser::Required::No);
    parser.parse(arguments);

    BenchmarkTimings timings;
    ScopeGuard dump_benchmark_timings = [&] {
        if (benchmark_timings)
            warnln("wasm-benchmark-timings: {}", timings.to_json().serialized());
    };
    auto timed = [&](JsonArray& phase, auto&& callback) {
        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        auto result = callback();
        if (benchmark_timings)
            phase.must_append(timer.elapsed_time().to_seconds_f64());
        return result;
    };

    if (!exported_function_to_execute.is_empty())
        attempt_instantiate = true;

    // NB: Compiling a module parses and validates it.
    auto module = timed(timings.parse, [&] { return compile(filename); });
    if (!module)
        return 1;
    if (!attempt_instantiate)
        return 0;

    Core::EventLoop::initialize_for_current_thread();

    OwnPtr<Wasm::Runtime::Wasi> wasi_implementation;
    if (wasi) {
        Vector<ByteString> wasi_arguments;
        for (auto argument : args_if_wasi)
            wasi_arguments.append(argument);
        Vector<Wasm::Runtime::Wasi::Preopen> preopens;
        for (auto mapping : wasi_preopened_mappings) {
            auto split_index = mapping.find(':');
            auto host_path = split_index.has_value() ? mapping.substring_view(0, *split_index) : mapping;
            auto guest_path = split_index.has_value() ? mapping.substring_view(*split_index + 1) : mapping;
            preopens.append({ TRY(FileSystem::real_path(host_path)), guest_path });
        }
        wasi_implementation = make<Wasm::Runtime::Wasi>(wasi_arguments, ReadonlySpan<ByteString> {}, preopens);
    }

    // Each linked module's exports resolve the imports of the ones after it, and of the main module, of its module name
    // and the export's name and kind.
    HashMap<ImportName, Wasm::Runtime::Extern> linked_exports;
    auto resolve_imports = [&](Wasm::Runtime::Module const& module_to_link) -> Optional<Vector<Wasm::Runtime::Extern>> {
        Vector<Wasm::Runtime::Extern> imports;
        bool missing = false;
        for (size_t index = 0; index < module_to_link.import_count(); ++index) {
            auto import = module_to_link.import(index);
            if (auto value = linked_exports.get({ import.module, import.name }); value.has_value() && value->kind == import.type.kind) {
                imports.append(*value);
                continue;
            }
            if (wasi_implementation && import.module == Wasm::Runtime::Wasi::module_name) {
                if (auto function = wasi_implementation->allocate_function(store, import.name); function.has_value()) {
                    imports.append({ ExternKind::Function, keep(*function) });
                    continue;
                }
                dbgln("wasi function {} not implemented :(", import.name);
            }
            if (auto value = js_exports.get({ import.module, import.name }); value.has_value()) {
                imports.append(*value);
                continue;
            }
            if (export_all_imports) {
                Optional<Entity> stub;
                switch (import.type.kind) {
                case ExternKind::Function: {
                    auto type = module_to_link.function_type(import.type.type_index);
                    if (!type.has_value())
                        break;
                    stub = store.allocate_host_function(*type, [name = ByteString(import.name), type = *type](auto&, ReadonlySpan<WasmValue> stub_arguments, Span<WasmValue>) -> ErrorOr<void, HostError> {
                        StringBuilder builder;
                        for (size_t argument = 0; argument < stub_arguments.size(); ++argument) {
                            if (argument != 0)
                                builder.append(", "sv);
                            builder.append(format_value(stub_arguments[argument], type.parameters[argument]));
                        }
                        dbgln("[wasm runtime] Stub function {} was called with the following arguments: {}", name, builder.string_view());
                        return {};
                    });
                    break;
                }
                case ExternKind::Table:
                    stub = store.allocate_table(import.type.value_type, import.type.limits, 0);
                    break;
                case ExternKind::Memory:
                    stub = store.allocate_memory(import.type.limits);
                    break;
                case ExternKind::Global:
                    stub = store.allocate_global(import.type.value_type, import.type.is_mutable, WasmValue::default_for(import.type.value_type));
                    break;
                case ExternKind::Tag:
                    break;
                }
                if (stub.has_value()) {
                    imports.append({ import.type.kind, keep(*stub) });
                    continue;
                }
            }
            warnln("Missing import '{}::{}'", import.module, import.name);
            missing = true;
        }
        if (missing)
            return {};
        return imports;
    };
    auto instantiate = [&](Wasm::Runtime::Module const& module_to_instantiate, StringView name) -> Optional<Entity> {
        auto imports = resolve_imports(module_to_instantiate);
        if (!imports.has_value()) {
            warnln("Linking {} failed", name);
            return {};
        }
        auto instance = timed(timings.instantiation, [&] { return store.instantiate(module_to_instantiate, *imports); });
        if (instance.is_error()) {
            warnln("Instantiation of {} failed: {}", name, instance.error().message);
            return {};
        }
        return keep(instance.value());
    };

    for (auto& [module_name, name] : modules_to_link_in) {
        auto linked_module = timed(timings.parse, [&] { return compile(name); });
        if (!linked_module) {
            warnln("Failed to compile linked module '{}'", name);
            return 1;
        }
        auto instance = instantiate(*linked_module, name);
        if (!instance.has_value())
            return 1;
        for (auto& entry : store.instance_exports(*instance))
            linked_exports.set({ module_name, entry.name }, entry.value);
    }

    auto instance = instantiate(*module, filename);
    if (!instance.has_value())
        return 1;
    main_instance = *instance;
    if (wasi_implementation) {
        if (auto memory = store.find_export(main_instance, "memory"sv); memory.has_value() && memory->kind == ExternKind::Memory)
            wasi_implementation->set_memory(memory->entity.ptr());
    }

    if (exported_function_to_execute.is_empty())
        return 0;

    auto function = store.find_export(main_instance, exported_function_to_execute);
    if (!function.has_value() || function->kind != ExternKind::Function) {
        warnln("No such exported function, sorry :(");
        return 1;
    }

    auto type = store.function_type(function->entity.ptr());
    Wasm::Runtime::Values values;
    for (auto& parameter : type.parameters) {
        if (values_to_push.is_empty()) {
            values.append(WasmValue::default_for(parameter));
        } else if (parameter.kind == values_to_push.last().type.kind) {
            values.append(values_to_push.take_last().value);
        } else {
            warnln("Type mismatch in argument");
            return 1;
        }
    }

    auto result = timed(timings.execution, [&] { return store.invoke(function->entity.ptr(), values); });
    if (result.is_error()) {
        if (wasi_implementation) {
            if (auto exit_code = wasi_implementation->exit_code(); exit_code.has_value())
                return *exit_code;
        }
        warnln("Execution trapped: {}", result.error().message);
        return 0;
    }

    if (!result.value().is_empty())
        warnln("Returned:");
    for (size_t index = 0; index < result.value().size(); ++index)
        outln("  -> {}", format_value(result.value()[index], type.results[index]));
    return 0;
}
