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
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibFileSystem/FileSystem.h>
#include <LibLine/Editor.h>
#include <LibMain/Main.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>
#include <LibWasm/Wasi.h>
#include <signal.h>
#include <unistd.h>

RefPtr<Line::Editor> g_line_editor;
static OwnPtr<Stream> g_stdout {};
static OwnPtr<Wasm::Printer> g_printer {};
static bool g_continue { false };
static void (*old_signal)(int);
static StackInfo g_stack_info;
static Wasm::DebuggerBytecodeInterpreter g_interpreter(g_stack_info);

static void sigint_handler(int)
{
    if (!g_continue) {
        signal(SIGINT, old_signal);
        kill(getpid(), SIGINT);
    }
    g_continue = false;
}

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

static ErrorOr<Wasm::Value> parse_value(StringView spec)
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
        return parse_u128(text);
    }

    if (lexer.consume_specific("i8.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return parse_scalar.operator()<i8>(text);
    }
    if (lexer.consume_specific("i16.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return parse_scalar.operator()<i16>(text);
    }
    if (lexer.consume_specific("i32.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return parse_scalar.operator()<i32>(text);
    }
    if (lexer.consume_specific("i64.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return parse_scalar.operator()<i64>(text);
    }
    if (lexer.consume_specific("f32.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return parse_scalar.operator()<float>(text);
    }
    if (lexer.consume_specific("f64.const"sv)) {
        lexer.ignore_while(is_sep);
        auto text = lexer.consume_all();
        return parse_scalar.operator()<double>(text);
    }

    if (lexer.consume_specific("v("sv)) {
        Vector<Wasm::Value> values;
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

        auto element_type = values.first().type();
        for (auto& value : values) {
            if (value.type() != element_type)
                return Error::from_string_literal("Mixed types in vector");
        }

        unsigned total_size = 0;
        unsigned width = 0;
        u128 result = 0;
        u128 last_value = 0;
        for (auto& value : values) {
            if (total_size >= 128)
                return Error::from_string_literal("Vector too large");

            width = value.value().visit(
                [&](Integral auto x) {
                    last_value = u128(x, 0);
                    return sizeof(x);
                },
                [&](float x) {
                    last_value = u128(bit_cast<u32>(x), 0);
                    return sizeof(x);
                },
                [&](double x) {
                    last_value = u128(bit_cast<u64>(x), 0);
                    return sizeof(x);
                },
                [&](auto) -> size_t { VERIFY_NOT_REACHED(); });

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

        return Wasm::Value { result };
    }

    return Error::from_string_literal("Invalid value");
}

static bool post_interpret_hook(Wasm::Configuration&, Wasm::InstructionPointer& ip, Wasm::Instruction const& instr, Wasm::Interpreter const& interpreter)
{
    if (interpreter.did_trap()) {
        g_continue = false;
        warnln("Trapped when executing ip={}", ip);
        g_printer->print(instr);
        warnln("Trap reason: {}", interpreter.trap_reason());
        const_cast<Wasm::Interpreter&>(interpreter).clear_trap();
    }
    return true;
}

static bool pre_interpret_hook(Wasm::Configuration& config, Wasm::InstructionPointer& ip, Wasm::Instruction const& instr)
{
    static bool always_print_stack = false;
    static bool always_print_instruction = false;
    if (always_print_stack)
        config.dump_stack();
    if (always_print_instruction) {
        g_stdout->write_until_depleted(ByteString::formatted("{:0>4} ", ip.value())).release_value_but_fixme_should_propagate_errors();
        g_printer->print(instr);
    }
    if (g_continue)
        return true;
    g_stdout->write_until_depleted(ByteString::formatted("{:0>4} ", ip.value())).release_value_but_fixme_should_propagate_errors();
    g_printer->print(instr);
    ByteString last_command = "";
    for (;;) {
        auto result = g_line_editor->get_line("> ");
        if (result.is_error()) {
            return false;
        }
        auto str = result.release_value();
        g_line_editor->add_to_history(str);
        if (str.is_empty())
            str = last_command;
        else
            last_command = str;
        auto args = str.split_view(' ');
        if (args.is_empty())
            continue;
        auto& cmd = args[0];
        if (cmd.is_one_of("h", "help")) {
            warnln("Wasm shell commands");
            warnln("Toplevel:");
            warnln("- [s]tep                     Run one instruction");
            warnln("- next                       Alias for step");
            warnln("- [c]ontinue                 Execute until a trap or the program exit point");
            warnln("- [p]rint <args...>          Print various things (see section on print)");
            warnln("- call <fn> <args...>        Call the function <fn> with the given arguments");
            warnln("- set <args...>              Set shell option (see section on settings)");
            warnln("- unset <args...>            Unset shell option (see section on settings)");
            warnln("- [h]elp                     Print this help");
            warnln();
            warnln("Print:");
            warnln("- print [s]tack              Print the contents of the stack, including frames and labels");
            warnln("- print [[m]em]ory <index>   Print the contents of the memory identified by <index>");
            warnln("- print [[i]nstr]uction      Print the current instruction");
            warnln("- print [[f]unc]tion <index> Print the function identified by <index>");
            warnln();
            warnln("Settings:");
            warnln("- set print stack            Make the shell print the stack on every instruction executed");
            warnln("- set print [instr]uction    Make the shell print the instruction that will be executed next");
            warnln();
            continue;
        }
        if (cmd.is_one_of("s", "step", "next")) {
            return true;
        }
        if (cmd.is_one_of("p", "print")) {
            if (args.size() < 2) {
                warnln("Print what?");
                continue;
            }
            auto& what = args[1];
            if (what.is_one_of("s", "stack")) {
                config.dump_stack();
                continue;
            }
            if (what.is_one_of("m", "mem", "memory")) {
                if (args.size() < 3) {
                    warnln("print what memory?");
                    continue;
                }
                auto value = args[2].to_number<u64>();
                if (!value.has_value()) {
                    warnln("invalid memory index {}", args[2]);
                    continue;
                }
                auto mem = config.store().get(Wasm::MemoryAddress(value.value()));
                if (!mem) {
                    warnln("invalid memory index {} (not found)", args[2]);
                    continue;
                }
                warnln("{:>32hex-dump}", mem->data().bytes());
                continue;
            }
            if (what.is_one_of("i", "instr", "instruction")) {
                g_printer->print(instr);
                continue;
            }
            if (what.is_one_of("f", "func", "function")) {
                if (args.size() < 3) {
                    warnln("print what function?");
                    continue;
                }
                auto value = args[2].to_number<u64>();
                if (!value.has_value()) {
                    warnln("invalid function index {}", args[2]);
                    continue;
                }
                auto fn = config.store().get(Wasm::FunctionAddress(value.value()));
                if (!fn) {
                    warnln("invalid function index {} (not found)", args[2]);
                    continue;
                }
                if (auto* fn_value = fn->get_pointer<Wasm::HostFunction>()) {
                    warnln("Host function at {:p}", &fn_value->function());
                    continue;
                }
                if (auto* fn_value = fn->get_pointer<Wasm::WasmFunction>()) {
                    g_printer->print(fn_value->code());
                    continue;
                }
            }
        }
        if (cmd == "call"sv) {
            if (args.size() < 2) {
                warnln("call what?");
                continue;
            }
            Optional<Wasm::FunctionAddress> address;
            auto index = args[1].to_number<u64>();
            if (index.has_value()) {
                address = config.frame().module().functions()[index.value()];
            } else {
                auto& name = args[1];
                for (auto& export_ : config.frame().module().exports()) {
                    if (export_.name() == name) {
                        if (auto addr = export_.value().get_pointer<Wasm::FunctionAddress>()) {
                            address = *addr;
                            break;
                        }
                    }
                }
            }

            if (!address.has_value()) {
            failed_to_find:;
                warnln("Could not find a function {}", args[1]);
                continue;
            }

            auto fn = config.store().get(*address);
            if (!fn)
                goto failed_to_find;

            auto type = fn->visit([&](auto& value) { return value.type(); });
            if (type.parameters().size() + 2 != args.size()) {
                warnln("Expected {} arguments for call, but found only {}", type.parameters().size(), args.size() - 2);
                continue;
            }
            Vector<Wasm::Value> values_to_push;
            Vector<Wasm::Value> values;
            auto ok = true;
            for (size_t index = 2; index < args.size(); ++index) {
                auto r = parse_value(args[index]);
                if (r.is_error()) {
                    warnln("Failed to parse argument {}: {}", args[index], r.error());
                    ok = false;
                    break;
                }
                values_to_push.append(r.release_value());
            }
            if (!ok)
                continue;
            for (auto& param : type.parameters()) {
                auto v = values_to_push.take_last();
                if (v.type() != param) {
                    warnln("Type mismatch in argument: expected {}, but got {}", Wasm::ValueType::kind_name(param.kind()), Wasm::ValueType::kind_name(v.type().kind()));
                    ok = false;
                    break;
                }
                values.append(v);
            }
            if (!ok)
                continue;

            Wasm::Result result { Wasm::Trap {} };
            {
                Wasm::BytecodeInterpreter::CallFrameHandle handle { g_interpreter, config };
                result = config.call(g_interpreter, *address, move(values)).assert_wasm_result();
            }
            if (result.is_trap()) {
                warnln("Execution trapped: {}", result.trap().reason);
            } else {
                if (!result.values().is_empty())
                    warnln("Returned:");
                for (auto& value : result.values()) {
                    g_stdout->write_until_depleted("  -> "sv.bytes()).release_value_but_fixme_should_propagate_errors();
                    g_printer->print(value);
                }
            }
            continue;
        }
        if (cmd.is_one_of("set", "unset")) {
            auto value = !cmd.starts_with('u');
            if (args.size() < 3) {
                warnln("(un)set what (to what)?");
                continue;
            }
            if (args[1] == "print"sv) {
                if (args[2] == "stack"sv)
                    always_print_stack = value;
                else if (args[2].is_one_of("instr", "instruction"))
                    always_print_instruction = value;
                else
                    warnln("Unknown print category '{}'", args[2]);
                continue;
            }
            warnln("Unknown set category '{}'", args[1]);
            continue;
        }
        if (cmd.is_one_of("c", "continue")) {
            g_continue = true;
            return true;
        }
        warnln("Command not understood: {}", cmd);
    }
}

static Optional<Wasm::Module> parse(StringView filename)
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

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView filename;
    bool print = false;
    bool attempt_instantiate = false;
    bool debug = false;
    bool export_all_imports = false;
    bool shell_mode = false;
    bool wasi = false;
    ByteString exported_function_to_execute;
    Vector<Wasm::Value> values_to_push;
    Vector<ByteString> modules_to_link_in;
    Vector<StringView> args_if_wasi;
    Vector<StringView> wasi_preopened_mappings;

    Core::ArgsParser parser;
    parser.add_positional_argument(filename, "File name to parse", "file");
    parser.add_option(debug, "Open a debugger", "debug", 'd');
    parser.add_option(print, "Print the parsed module", "print", 'p');
    parser.add_option(attempt_instantiate, "Attempt to instantiate the module", "instantiate", 'i');
    parser.add_option(exported_function_to_execute, "Attempt to execute the named exported function from the module (implies -i)", "execute", 'e', "name");
    parser.add_option(export_all_imports, "Export noop functions corresponding to imports", "export-noop");
    parser.add_option(shell_mode, "Launch a REPL in the module's context (implies -i)", "shell", 's');
    parser.add_option(wasi, "Enable WASI", "wasi", 'w');
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

    if (shell_mode) {
        debug = true;
        attempt_instantiate = true;
    }

    if (!shell_mode && debug && exported_function_to_execute.is_empty()) {
        warnln("Debug what? (pass -e fn)");
        return 1;
    }

    if (debug || shell_mode) {
        old_signal = signal(SIGINT, sigint_handler);
    }

    if (!exported_function_to_execute.is_empty())
        attempt_instantiate = true;

    auto parse_result = parse(filename);
    if (!parse_result.has_value())
        return 1;

    g_stdout = TRY(Core::File::standard_output());
    g_printer = TRY(try_make<Wasm::Printer>(*g_stdout));

    if (print && !attempt_instantiate) {
        Wasm::Printer printer(*g_stdout);
        printer.print(parse_result.value());
    }

    if (attempt_instantiate) {
        Wasm::AbstractMachine machine;
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

        Core::EventLoop main_loop;
        if (debug) {
            g_line_editor = Line::Editor::construct();
            g_interpreter.pre_interpret_hook = pre_interpret_hook;
            g_interpreter.post_interpret_hook = post_interpret_hook;
        }

        // First, resolve the linked modules
        Vector<NonnullOwnPtr<Wasm::ModuleInstance>> linked_instances;
        Vector<Wasm::Module> linked_modules;
        for (auto& name : modules_to_link_in) {
            auto parse_result = parse(name);
            if (!parse_result.has_value()) {
                warnln("Failed to parse linked module '{}'", name);
                return 1;
            }
            linked_modules.append(parse_result.release_value());
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

        Wasm::Linker linker { parse_result.value() };
        for (auto& instance : linked_instances)
            linker.link(*instance);

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

        if (export_all_imports) {
            HashMap<Wasm::Linker::Name, Wasm::ExternValue> exports;
            for (auto& entry : linker.unresolved_imports()) {
                if (!entry.type.has<Wasm::TypeIndex>())
                    continue;
                auto type = parse_result.value().type(entry.type.get<Wasm::TypeIndex>());
                auto address = machine.store().allocate(Wasm::HostFunction(
                    [name = entry.name, type = type](auto&, auto& arguments) -> Wasm::Result {
                        StringBuilder argument_builder;
                        bool first = true;
                        for (auto& argument : arguments) {
                            AllocatingMemoryStream stream;
                            Wasm::Printer { stream }.print(argument);
                            if (first)
                                first = false;
                            else
                                argument_builder.append(", "sv);
                            auto buffer = ByteBuffer::create_uninitialized(stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
                            stream.read_until_filled(buffer).release_value_but_fixme_should_propagate_errors();
                            argument_builder.append(StringView(buffer).trim_whitespace());
                        }
                        dbgln("[wasm runtime] Stub function {} was called with the following arguments: {}", name, argument_builder.to_byte_string());
                        Vector<Wasm::Value> result;
                        result.ensure_capacity(type.results().size());
                        for (auto& result_type : type.results())
                            result.append(Wasm::Value { result_type, 0ull });
                        return Wasm::Result { move(result) };
                    },
                    type,
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
        auto result = machine.instantiate(parse_result.value(), link_result.release_value());
        if (result.is_error()) {
            warnln("Module instantiation failed: {}", result.error().error);
            return 1;
        }
        auto module_instance = result.release_value();

        auto launch_repl = [&] {
            Wasm::Configuration config { machine.store() };
            Wasm::Expression expression { {} };
            config.set_frame(Wasm::Frame {
                *module_instance,
                Vector<Wasm::Value> {},
                expression,
                0,
            });
            Wasm::Instruction instr { Wasm::Instructions::nop };
            Wasm::InstructionPointer ip { 0 };
            g_continue = false;
            pre_interpret_hook(config, ip, instr);
        };

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

        if (shell_mode) {
            launch_repl();
            return 0;
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
                    values.append(Wasm::Value { param, 0ull });
                } else if (param == values_to_push.last().type()) {
                    values.append(values_to_push.take_last());
                } else {
                    warnln("Type mismatch in argument: expected {}, but got {}", Wasm::ValueType::kind_name(param.kind()), Wasm::ValueType::kind_name(values_to_push.last().type().kind()));
                    return 1;
                }
            }

            if (print) {
                outln("Executing ");
                print_func(*run_address);
                outln();
            }

            auto result = machine.invoke(g_interpreter, run_address.value(), move(values)).assert_wasm_result();

            if (debug)
                launch_repl();

            if (result.is_trap()) {
                warnln("Execution trapped: {}", result.trap().reason);
            } else {
                if (!result.values().is_empty())
                    warnln("Returned:");
                for (auto& value : result.values()) {
                    g_stdout->write_until_depleted("  -> "sv.bytes()).release_value_but_fixme_should_propagate_errors();
                    g_printer->print(value);
                }
            }
        }
    }

    return 0;
}
