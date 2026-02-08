/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/Hex.h>
#include <AK/MemoryStream.h>
#include <AK/StackInfo.h>
#include <AK/Utf16String.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibFileSystem/FileSystem.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#if !defined(AK_OS_WINDOWS)
#    include <LibLine/Editor.h>
#endif
#include <LibMain/Main.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#if !defined(AK_OS_WINDOWS)
#    include <LibWasm/Wasi.h>
#endif
#include <math.h>
#include <signal.h>

static OwnPtr<Stream> g_stdout {};
static OwnPtr<Wasm::Printer> g_printer {};
static StackInfo g_stack_info;
static Wasm::BytecodeInterpreter g_interpreter(g_stack_info);

struct ParsedValue {
    Wasm::Value value;
    Wasm::ValueType type;
};

static Optional<u128> convert_to_uint(StringView string)
{
    if (string.is_empty())
        return {};

    u128 value = 0;
    auto const characters = string.characters_without_null_termination();

    for (size_t i = 0; i < string.length(); i++) {
        if (characters[i] < '0' || characters[i] > '9')
            return {};

        value *= 10;
        value += u128 { static_cast<u64>(characters[i] - '0'), 0 };
    }
    return value;
}

static Optional<u128> convert_to_uint_from_hex(StringView string)
{
    if (string.is_empty())
        return {};

    u128 value = 0;
    auto const count = string.length();
    auto const upper_bound = NumericLimits<u128>::max();

    for (size_t i = 0; i < count; i++) {
        char digit = string[i];
        if (value > (upper_bound >> 4))
            return {};

        auto digit_val = decode_hex_digit(digit);
        if (digit_val == 255)
            return {};

        value = (value << 4) + digit_val;
    }
    return value;
}

static ErrorOr<ParsedValue> parse_value(StringView spec)
{
    constexpr auto is_sep = [](char c) { return is_ascii_space(c) || c == ':'; };
    // Scalar: 'T.const[:\s]v' (i32.const 42)
    auto parse_scalar = []<typename T>(StringView text) -> ErrorOr<Wasm::Value> {
        if constexpr (IsFloatingPoint<T>) {
            if (text.trim_whitespace().equals_ignoring_ascii_case("nan"sv)) {
                if constexpr (IsSame<T, float>)
                    return Wasm::Value { nanf("") };
                else
                    return Wasm::Value { nan("") };
            }
            if (text.trim_whitespace().equals_ignoring_ascii_case("inf"sv)) {
                if constexpr (IsSame<T, float>)
                    return Wasm::Value { HUGE_VALF };
                else
                    return Wasm::Value { HUGE_VAL };
            }
        }
        if (auto v = text.to_number<T>(); v.has_value())
            return Wasm::Value { *v };
        return Error::from_string_literal("Invalid scalar value");
    };
    // Vector: 'v128.const[:\s]v' (v128.const 0x01000000020000000300000004000000) or 'v(T.const[:\s]v, ...)' (v(i32.const 1, i32.const 2, i32.const 3, i32.const 4))
    auto parse_u128 = [](StringView text) -> ErrorOr<Wasm::Value> {
        u128 value;
        if (text.starts_with("0x"sv)) {
            if (auto v = convert_to_uint_from_hex(text); v.has_value())
                value = *v;
            else
                return Error::from_string_literal("Invalid hex v128 value");
        } else {
            if (auto v = convert_to_uint(text); v.has_value())
                value = *v;
            else
                return Error::from_string_literal("Invalid v128 value");
        }

        return Wasm::Value { value };
    };

    GenericLexer lexer(spec);
    if (lexer.consume_specific("v128.const"sv)) {
        lexer.ignore_while(is_sep);
        // The rest of the string is the value
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_u128(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::V128)
        };
    }

    if (lexer.consume_specific("i8.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_scalar.operator()<i8>(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::I32)
        };
    }
    if (lexer.consume_specific("i16.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_scalar.operator()<i16>(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::I32)
        };
    }
    if (lexer.consume_specific("i32.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_scalar.operator()<i32>(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::I32)
        };
    }
    if (lexer.consume_specific("i64.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_scalar.operator()<i64>(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::I64)
        };
    }
    if (lexer.consume_specific("f32.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_scalar.operator()<float>(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::F32)
        };
    }
    if (lexer.consume_specific("f64.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return ParsedValue {
            .value = TRY(parse_scalar.operator()<double>(text)),
            .type = Wasm::ValueType(Wasm::ValueType::Kind::F64)
        };
    }

    if (lexer.consume_specific("v("sv)) {
        Vector<ParsedValue> values;
        for (;;) {
            lexer.ignore_while(is_sep);
            if (lexer.consume_specific(")"sv))
                break;
            if (lexer.is_eof()) {
                warnln("Expected ')' to close vector");
                break;
            }
            auto value = parse_value(lexer.consume_until(is_any_of(",)"sv)));
            if (value.is_error())
                return value.release_error();
            lexer.consume_specific(',');
            values.append(value.release_value());
        }

        if (values.is_empty())
            return Error::from_string_literal("Empty vector");

        auto element_type = values.first().type;
        for (auto& value : values) {
            if (value.type != element_type)
                return Error::from_string_literal("Mixed types in vector");
        }

        unsigned total_size = 0;
        unsigned width = 0;
        u128 result = 0;
        u128 last_value = 0;
        for (auto& parsed : values) {
            if (total_size >= 128)
                return Error::from_string_literal("Vector too large");

            switch (parsed.type.kind()) {
            case Wasm::ValueType::F32:
            case Wasm::ValueType::I32:
                width = sizeof(u32);
                break;
            case Wasm::ValueType::F64:
            case Wasm::ValueType::I64:
                width = sizeof(u64);
                break;
            case Wasm::ValueType::V128:
            case Wasm::ValueType::FunctionReference:
            case Wasm::ValueType::ExternReference:
            case Wasm::ValueType::ExceptionReference:
            case Wasm::ValueType::TypeUseReference:
            case Wasm::ValueType::UnsupportedHeapReference:
                VERIFY_NOT_REACHED();
            }
            last_value = parsed.value.value();

            result |= last_value << total_size;
            total_size += width * 8;
        }

        if (total_size < 128)
            warnln("Vector value '{}' is only {} bytes wide, repeating last element", spec, total_size);
        while (total_size < 128) {
            // Repeat the last value until we fill the 128 bits
            result |= last_value << total_size;
            total_size += width * 8;
        }

        return ParsedValue {
            .value = Wasm::Value { result },
            .type = Wasm::ValueType(Wasm::ValueType::Kind::V128)
        };
    }

    return Error::from_string_literal("Invalid value");
}

static RefPtr<Wasm::Module> parse(StringView filename)
{
    auto result = Core::MappedFile::map(filename);
    if (result.is_error()) {
        warnln("Failed to open {}: {}", filename, result.error());
        return {};
    }

    auto parse_result = Wasm::Module::parse(*result.value());
    if (parse_result.is_error()) {
        warnln("Something went wrong, either the file is invalid, or there's a bug with LibWasm!");
        warnln("The parse error was {}", Wasm::parse_error_to_byte_string(parse_result.error()));
        return {};
    }
    return parse_result.release_value();
}

static void print_link_error(Wasm::LinkError const& error)
{
    for (auto const& missing : error.missing_imports)
        warnln("Missing import '{}'", missing);
}

template<typename T>
static ErrorOr<T, Wasm::Result> trap_for_js_exception(JS::VM& vm, JS::ThrowCompletionOr<T> const& result)
{
    if (!result.is_error())
        return result.value();

    auto const& completion = result.error();
    auto& exception = completion.value();
    warnln("JS exception: {}", MUST(exception.to_string(vm)));
    return Wasm::Trap { ByteString("JS exception") };
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView filename;
    bool print = false;
    bool print_compiled = false;
    bool attempt_instantiate = false;
    bool export_all_imports = false;
    [[maybe_unused]] bool wasi = false;
    Optional<u64> specific_function_address;
    ByteString exported_function_to_execute;
    Vector<ParsedValue> values_to_push;
    Vector<ByteString> modules_to_link_in;
    Vector<StringView> args_if_wasi;
    Vector<StringView> wasi_preopened_mappings;
    HashMap<Wasm::Linker::Name, Wasm::ExternValue> js_exports;

    Wasm::AbstractMachine machine;
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    Core::ArgsParser parser;
    parser.add_positional_argument(filename, "File name to parse", "file");
    parser.add_option(print, "Print the parsed module", "print", 'p');
    parser.add_option(print_compiled, "Print the compiled module", "print-compiled");
    parser.add_option(specific_function_address, "Optional compiled function address to print", "print-function", 'f', "address");
    parser.add_option(attempt_instantiate, "Attempt to instantiate the module", "instantiate", 'i');
    parser.add_option(exported_function_to_execute, "Attempt to execute the named exported function from the module (implies -i)", "execute", 'e', "name");
    parser.add_option(export_all_imports, "Export noop functions corresponding to imports", "export-noop");
#if !defined(AK_OS_WINDOWS)
    parser.add_option(wasi, "Enable WASI", "wasi", 'w');
#endif
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Export js `function(arg...) { source }` returning T as [module].[function]",
        .long_name = "export-js",
        .short_name = 0,
        .value_name = "module.function(arg:T...):T=source",
        .accept_value = [&](StringView str) {
            GenericLexer lexer(str);
            // [module] <.> [function] <(> {[name] <:> [type]} <)> (<:> [type])? <=> [text]
            auto module = lexer.consume_until('.');
            if (!lexer.consume_specific('.')) {
                warnln("Invalid JS export module in '{}'", str);
                return false;
            }
            auto fn_name = lexer.consume_until(is_any_of("(=:"sv));
            struct Arg {
                Wasm::ValueType type;
                StringView name;
            };
            Vector<Arg> formal_params;
            if (lexer.consume_specific('(')) {
                while (!lexer.consume_specific(')')) {
                    auto name = lexer.consume_until(is_any_of(",:)"sv));
                    if (name.is_empty()) {
                        warnln("Invalid JS export argument name in '{}'", str);
                        return false;
                    }
                    auto type_kind = Wasm::ValueType::I32;
                    if (lexer.consume_specific(':')) {
                        if (lexer.consume_specific("i32"sv)) {
                            type_kind = Wasm::ValueType::I32;
                        } else if (lexer.consume_specific("i64"sv)) {
                            type_kind = Wasm::ValueType::I64;
                        } else if (lexer.consume_specific("f32"sv)) {
                            type_kind = Wasm::ValueType::F32;
                        } else if (lexer.consume_specific("f64"sv)) {
                            type_kind = Wasm::ValueType::F64;
                        } else if (lexer.consume_specific("v128"sv)) {
                            type_kind = Wasm::ValueType::V128;
                        } else {
                            warnln("Invalid JS export argument type in '{}'", str);
                            return false;
                        }
                    }
                    formal_params.append(Arg { Wasm::ValueType(type_kind), name });
                    lexer.consume_specific(',');
                }
            }
            Vector<Wasm::ValueType::Kind> returns;
            if (lexer.consume_specific(':')) {
                if (lexer.consume_specific("i32"sv)) {
                    returns.append(Wasm::ValueType::I32);
                } else if (lexer.consume_specific("i64"sv)) {
                    returns.append(Wasm::ValueType::I64);
                } else if (lexer.consume_specific("f32"sv)) {
                    returns.append(Wasm::ValueType::F32);
                } else if (lexer.consume_specific("f64"sv)) {
                    returns.append(Wasm::ValueType::F64);
                } else if (lexer.consume_specific("v128"sv)) {
                    returns.append(Wasm::ValueType::V128);
                } else {
                    warnln("Invalid JS export return type in '{}'", str);
                    return false;
                }
            }

            if (!lexer.consume_specific('=') || lexer.is_eof()) {
                warnln("Invalid JS export source in '{}'", str);
                return false;
            }

            auto source_text = lexer.consume_all().trim_whitespace();
            StringBuilder builder;
            builder.append("("sv);
            auto first = true;
            for (auto& arg : formal_params) {
                if (!first)
                    builder.append(", "sv);
                first = false;
                builder.append(arg.name);
            }
            builder.appendff(") => {}", source_text);
            auto js_function = builder.to_byte_string();
            auto name = ByteString::formatted("{}.{}", module, fn_name);
            auto script = JS::Script::parse(js_function, realm, name);
            if (script.is_error()) {
                warnln("Failed to parse JS export source '{}':", js_function);
                return false;
            }

            auto js_script = script.release_value();
            JS::Bytecode::Interpreter interp;
            auto maybe_function = interp.run(*js_script);
            if (maybe_function.is_error()) {
                warnln("Failed to run JS export source '{}'", js_function);
                return false;
            }
            auto function_val = maybe_function.release_value();
            if (!function_val.is_function()) {
                warnln("JS export source '{}' did not parse as a function", js_function);
                return false;
            }

            auto& function = function_val.as_function();

            Vector<Wasm::ValueType> results;
            Vector<Wasm::ValueType> params;
            for (auto& type : returns)
                results.append(Wasm::ValueType(type));
            for (auto& arg : formal_params)
                params.append(Wasm::ValueType(arg.type));

            Wasm::FunctionType function_type = { move(params), move(results) };
            auto host_function = Wasm::HostFunction {
                [&vm, &function, formal_params, returns, name](Wasm::Configuration&, Span<Wasm::Value> args) mutable -> Wasm::Result {
                    Vector<JS::Value> js_args;
                    js_args.ensure_capacity(args.size());
                    for (size_t i = 0; i < formal_params.size(); ++i) {
                        auto type = formal_params[i].type;
                        if (i >= args.size()) {
                            warnln("Not enough arguments provided to JS export function '{}'", name);
                            return Wasm::Trap { ByteString("Not enough arguments") };
                        }
                        auto& arg = args[i];
                        switch (type.kind()) {
                        case Wasm::ValueType::I32:
                            js_args.append(JS::Value(arg.to<u32>()));
                            break;
                        case Wasm::ValueType::I64:
                            js_args.append(JS::Value(arg.to<u64>()));
                            break;
                        case Wasm::ValueType::F32:
                            js_args.append(JS::Value(arg.to<f32>()));
                            break;
                        case Wasm::ValueType::F64:
                            js_args.append(JS::Value(arg.to<f64>()));
                            break;
                        case Wasm::ValueType::V128: {
                            auto value = arg.to<u128>();
                            ReadonlyBytes data { bit_cast<u8 const*>(&value), sizeof(u128) };
                            js_args.append(vm->heap().allocate<JS::BigInt>(Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { data } }));
                            break;
                        }
                        default:
                            warnln("Unsupported argument type '{}' for JS export function '{}'", type.kind_name(), name);
                            return Wasm::Trap { ByteString("Unsupported argument type") };
                        }
                    }
                    auto result = TRY(trap_for_js_exception(vm, JS::call(vm, function, JS::js_null(), js_args.span())));
                    if (returns.is_empty())
                        return Wasm::Result { Vector<Wasm::Value> {} };

                    if (returns.size() != 1)
                        return Wasm::Trap { ByteString("NYI") };

                    switch (returns[0]) {
                    case Wasm::ValueType::I32:
                        return Wasm::Result { Vector<Wasm::Value> { Wasm::Value { TRY(trap_for_js_exception(*vm, result.to_u32(vm))) } } };
                    case Wasm::ValueType::I64:
                        return Wasm::Result { Vector<Wasm::Value> { Wasm::Value { TRY(trap_for_js_exception(*vm, result.to_bigint_uint64(vm))) } } };
                    case Wasm::ValueType::F32:
                        return Wasm::Result { Vector<Wasm::Value> { Wasm::Value { static_cast<f32>(TRY(trap_for_js_exception(*vm, result.to_double(vm)))) } } };
                    case Wasm::ValueType::F64:
                        return Wasm::Result { Vector<Wasm::Value> { Wasm::Value { TRY(trap_for_js_exception(*vm, result.to_double(vm))) } } };
                    case Wasm::ValueType::V128: {
                        auto value = TRY(trap_for_js_exception(*vm, result.to_bigint(vm)));
                        u128 out {};
                        Bytes data { bit_cast<u8*>(&out), sizeof(u128) };
                        if (value->big_integer().unsigned_value().export_data(data).size() != data.size()) {
                            dbgln("JS export function '{}' returned a v128 value that is not 128 bits wide", name);
                            return Wasm::Trap { ByteString("Invalid v128 value") };
                        }
                        return Wasm::Result { Vector<Wasm::Value> { Wasm::Value { out } } };
                    }
                    default:
                        warnln("Unsupported return type for JS export function '{}'", name);
                        return Wasm::Trap { ByteString("Unsupported return type") };
                    }
                },
                function_type,
                name,
            };
            auto host_function_instance = machine.store().allocate(move(host_function));
            if (!host_function_instance.has_value()) {
                warnln("Failed to allocate host function instance for '{}'", name);
                return false;
            }
            js_exports.set({ .module = module, .name = fn_name, .type = function_type }, *host_function_instance);
            return true;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Directory mappings to expose via WASI",
        .long_name = "wasi-map-dir",
        .short_name = 0,
        .value_name = "path[:path]",
        .accept_value = [&](StringView str) {
            if (!str.is_empty()) {
                wasi_preopened_mappings.append(str);
                return true;
            }
            return false;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Extra modules to link with, use to resolve imports",
        .long_name = "link",
        .short_name = 'l',
        .value_name = "file",
        .accept_value = [&](StringView str) {
            if (!str.is_empty()) {
                modules_to_link_in.append(str);
                return true;
            }
            return false;
        },
    });
    parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Supply arguments to the function (default=0) (T.const:v or v(T.const:v, ...))",
        .long_name = "arg",
        .short_name = 0,
        .value_name = "value",
        .accept_value = [&](StringView str) -> bool {
            auto result = parse_value(str);
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

    if (!exported_function_to_execute.is_empty())
        attempt_instantiate = true;

    auto parse_result = parse(filename);
    if (parse_result.is_null())
        return 1;

    g_stdout = TRY(Core::File::standard_output());
    g_printer = TRY(try_make<Wasm::Printer>(*g_stdout));

    if (print && !attempt_instantiate) {
        Wasm::Printer printer(*g_stdout);
        printer.print(*parse_result);
    }

    if (attempt_instantiate || print_compiled) {
#if !defined(AK_OS_WINDOWS)
        Optional<Wasm::Wasi::Implementation> wasi_impl;

        if (wasi) {
            wasi_impl.emplace(Wasm::Wasi::Implementation::Details {
                .provide_arguments = [&] {
                    Vector<String> strings;
                    for (auto& string : args_if_wasi)
                        strings.append(String::from_utf8(string).release_value_but_fixme_should_propagate_errors());
                    return strings; },
                .provide_environment = {},
                .provide_preopened_directories = [&] {
                    Vector<Wasm::Wasi::Implementation::MappedPath> paths;
                    for (auto& string : wasi_preopened_mappings) {
                        auto split_index = string.find(':');
                        if (split_index.has_value()) {
                            LexicalPath host_path { FileSystem::real_path(string.substring_view(0, *split_index)).release_value_but_fixme_should_propagate_errors() };
                            LexicalPath mapped_path { string.substring_view(*split_index + 1) };
                            paths.append({move(host_path), move(mapped_path)});
                        } else {
                            LexicalPath host_path { FileSystem::real_path(string).release_value_but_fixme_should_propagate_errors() };
                            LexicalPath mapped_path { string };
                            paths.append({move(host_path), move(mapped_path)});
                        }
                    }
                    return paths; },
            });
        }
#endif

        Core::EventLoop main_loop;
        // First, resolve the linked modules
        Vector<NonnullOwnPtr<Wasm::ModuleInstance>> linked_instances;
        Vector<NonnullRefPtr<Wasm::Module>> linked_modules;
        for (auto& name : modules_to_link_in) {
            auto parse_result = parse(name);
            if (parse_result.is_null()) {
                warnln("Failed to parse linked module '{}'", name);
                return 1;
            }
            linked_modules.append(parse_result.release_nonnull());
            Wasm::Linker linker { linked_modules.last() };
            for (auto& instance : linked_instances)
                linker.link(*instance);
            auto link_result = linker.finish();
            if (link_result.is_error()) {
                warnln("Linking imported module '{}' failed", name);
                print_link_error(link_result.error());
                return 1;
            }
            auto instantiation_result = machine.instantiate(linked_modules.last(), link_result.release_value());
            if (instantiation_result.is_error()) {
                warnln("Instantiation of imported module '{}' failed: {}", name, instantiation_result.error().error);
                return 1;
            }
            linked_instances.append(instantiation_result.release_value());
        }

        Wasm::Linker linker { *parse_result };
        for (auto& instance : linked_instances)
            linker.link(*instance);

#if !defined(AK_OS_WINDOWS)
        if (wasi) {
            HashMap<Wasm::Linker::Name, Wasm::ExternValue> wasi_exports;
            for (auto& entry : linker.unresolved_imports()) {
                if (entry.module != "wasi_snapshot_preview1"sv)
                    continue;
                auto function = wasi_impl->function_by_name(entry.name);
                if (function.is_error()) {
                    dbgln("wasi function {} not implemented :(", entry.name);
                    continue;
                }
                auto address = machine.store().allocate(function.release_value());
                wasi_exports.set(entry, *address);
            }

            linker.link(wasi_exports);
        }
#endif

        linker.link(js_exports);

        if (export_all_imports) {
            HashMap<Wasm::Linker::Name, Wasm::ExternValue> exports;
            for (auto& entry : linker.unresolved_imports()) {
                if (!entry.type.has<Wasm::TypeIndex>())
                    continue;
                auto type = parse_result->type_section().types()[entry.type.get<Wasm::TypeIndex>().value()];

                if (!type.is_function())
                    continue;
                auto& func = type.function();

                auto address = machine.store().allocate(Wasm::HostFunction(
                    [name = entry.name, func = func](auto&, auto arguments) -> Wasm::Result {
                        StringBuilder argument_builder;
                        bool first = true;
                        size_t index = 0;
                        for (auto& argument : arguments) {
                            AllocatingMemoryStream stream;
                            auto value_type = func.parameters()[index];
                            Wasm::Printer { stream }.print(argument, value_type);
                            if (first)
                                first = false;
                            else
                                argument_builder.append(", "sv);
                            auto buffer = ByteBuffer::create_uninitialized(stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
                            stream.read_until_filled(buffer).release_value_but_fixme_should_propagate_errors();
                            argument_builder.append(StringView(buffer).trim_whitespace());
                            ++index;
                        }
                        dbgln("[wasm runtime] Stub function {} was called with the following arguments: {}", name, argument_builder.to_byte_string());
                        Vector<Wasm::Value> result;
                        result.ensure_capacity(func.results().size());
                        for (auto expect_result : func.results())
                            result.append(Wasm::Value(expect_result));
                        return Wasm::Result { move(result) };
                    },
                    func,
                    entry.name));
                exports.set(entry, *address);
            }

            linker.link(exports);
        }

        auto link_result = linker.finish();
        if (link_result.is_error()) {
            warnln("Linking main module failed");
            print_link_error(link_result.error());
            return 1;
        }

        auto result = machine.instantiate(*parse_result, link_result.release_value());
        if (result.is_error()) {
            warnln("Module instantiation failed: {}", result.error().error);
            return 1;
        }
        auto module_instance = result.release_value();

        if (print_compiled) {
            Span<Wasm::FunctionAddress const> functions = module_instance->functions();
            Wasm::FunctionAddress spec = specific_function_address.value_or(0);

            if (specific_function_address.has_value())
                functions = { &spec, 1 };
            for (auto address : functions) {
                auto function = machine.store().get(address)->get_pointer<Wasm::WasmFunction>();
                if (!function)
                    continue;
                auto& expression = function->code().func().body();
                if (expression.compiled_instructions.dispatches.is_empty())
                    continue;

                ByteString export_name;
                for (auto& entry : function->module().exports()) {
                    if (entry.value() == address) {
                        export_name = ByteString::formatted(" '{}'", entry.name());
                        break;
                    }
                }

                TRY(g_stdout->write_until_depleted(ByteString::formatted("Function #{}{} (stack usage = {}):\n", address.value(), export_name, expression.stack_usage_hint())));

                Wasm::Printer printer { *g_stdout, 1 };
                for (size_t ip = 0; ip < expression.compiled_instructions.dispatches.size(); ++ip) {
                    auto& dispatch = expression.compiled_instructions.dispatches[ip];
                    auto& addresses = expression.compiled_instructions.src_dst_mappings[ip];
                    ByteString regs;
                    auto first = true;
                    ssize_t in_count = 0;
                    ssize_t out_count = 0;
#define M(name, _, ins, outs)              \
    case Wasm::Instructions::name.value(): \
        in_count = ins;                    \
        out_count = outs;                  \
        break;
                    switch (dispatch.instruction->opcode().value()) {
                        ENUMERATE_WASM_OPCODES(M)
                    }
#undef M
                    constexpr auto reg_name = [](Wasm::Dispatch::RegisterOrStack reg) -> ByteString {
                        if (reg == Wasm::Dispatch::RegisterOrStack::Stack)
                            return "stack"sv;
                        if (reg >= Wasm::Dispatch::RegisterOrStack::CallRecord)
                            return ByteString::formatted("cr{}", to_underlying(reg) - to_underlying(Wasm::Dispatch::RegisterOrStack::CallRecord));
                        return ByteString::formatted("reg{}", to_underlying(reg));
                    };
                    if (in_count > -1) {
                        for (ssize_t index = 0; index < in_count; ++index) {
                            if (first)
                                regs = ByteString::formatted("{} ({}", regs, reg_name(addresses.sources[index]));
                            else
                                regs = ByteString::formatted("{}, {}", regs, reg_name(addresses.sources[index]));
                            first = false;
                        }
                        if (out_count > 0) {
                            if (first)
                                regs = ByteString::formatted(" () -> {}", reg_name(addresses.destination));
                            else
                                regs = ByteString::formatted("{}) -> {}", regs, reg_name(addresses.destination));
                        } else if (out_count == 0) {
                            if (first)
                                regs = ByteString::formatted(" () -x");
                            else
                                regs = ByteString::formatted("{}) -x", regs);
                        } else {
                            if (first)
                                regs = ByteString::formatted(" () -?");
                            else
                                regs = ByteString::formatted("{}) -?", regs);
                        }
                    } else if (dispatch.instruction->opcode() == Wasm::Instructions::call || dispatch.instruction->opcode() == Wasm::Instructions::call_indirect) {
                        if (addresses.destination != Wasm::Dispatch::RegisterOrStack::Stack)
                            regs = ByteString::formatted("(?) -> {}", reg_name(addresses.destination));
                    }

                    if (regs.is_empty())
                        regs = ByteString::formatted(" {{{:-<34}}}", regs);
                    else
                        regs = ByteString::formatted(" {{{: <33} }}", regs);

                    TRY(g_stdout->write_until_depleted(ByteString::formatted("  [{:>03}]", ip)));
                    TRY(g_stdout->write_until_depleted(regs.bytes()));
                    printer.print(*dispatch.instruction);
                }

                TRY(g_stdout->write_until_depleted("\n"sv.bytes()));
            }
        }

        auto print_func = [&](auto const& address) {
            Wasm::FunctionInstance* fn = machine.store().get(address);
            g_stdout->write_until_depleted(ByteString::formatted("- Function with address {}, ptr = {}\n", address.value(), fn)).release_value_but_fixme_should_propagate_errors();
            if (fn) {
                g_stdout->write_until_depleted(ByteString::formatted("    wasm function? {}\n", fn->has<Wasm::WasmFunction>())).release_value_but_fixme_should_propagate_errors();
                fn->visit(
                    [&](Wasm::WasmFunction const& func) {
                        Wasm::Printer printer { *g_stdout, 3 };
                        g_stdout->write_until_depleted("    type:\n"sv).release_value_but_fixme_should_propagate_errors();
                        printer.print(func.type());
                        g_stdout->write_until_depleted("    code:\n"sv).release_value_but_fixme_should_propagate_errors();
                        printer.print(func.code());
                    },
                    [](Wasm::HostFunction const&) {});
            }
        };
        if (print) {
            // Now, let's dump the functions!
            for (auto& address : module_instance->functions()) {
                print_func(address);
            }
        }

        if (!exported_function_to_execute.is_empty()) {
            Optional<Wasm::FunctionAddress> run_address;
            Vector<Wasm::Value> values;
            for (auto& entry : module_instance->exports()) {
                if (entry.name() == exported_function_to_execute) {
                    if (auto addr = entry.value().get_pointer<Wasm::FunctionAddress>())
                        run_address = *addr;
                }
            }
            if (!run_address.has_value()) {
                warnln("No such exported function, sorry :(");
                return 1;
            }

            auto instance = machine.store().get(*run_address);
            VERIFY(instance);

            if (instance->has<Wasm::HostFunction>()) {
                warnln("Exported function is a host function, cannot run that yet");
                return 1;
            }

            for (auto& param : instance->get<Wasm::WasmFunction>().type().parameters()) {
                if (values_to_push.is_empty()) {
                    values.append(Wasm::Value(param));
                } else if (param == values_to_push.last().type) {
                    values.append(values_to_push.take_last().value);
                } else {
                    warnln("Type mismatch in argument: expected {}, but got {}", param.kind_name(), values_to_push.last().type.kind_name());
                    return 1;
                }
            }

            if (print) {
                outln("Executing ");
                print_func(*run_address);
                outln();
            }

            auto result = machine.invoke(g_interpreter, run_address.value(), move(values));
            if (result.is_trap()) {
                auto trap_reason = result.trap().format();
                if (trap_reason.starts_with("exit:"sv))
                    return -trap_reason.substring_view(5).to_number<i32>().value_or(-1);
                warnln("Execution trapped: {}", trap_reason);
            } else {
                if (!result.values().is_empty())
                    warnln("Returned:");
                auto result_type = instance->get<Wasm::WasmFunction>().type().results();
                size_t index = 0;
                for (auto& value : result.values()) {
                    g_stdout->write_until_depleted("  -> "sv.bytes()).release_value_but_fixme_should_propagate_errors();
                    g_printer->print(value, result_type[index]);
                    ++index;
                }
            }
        }
    }

    return 0;
}
