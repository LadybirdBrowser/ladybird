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
 
 struct ParsedValue {
     Wasm::Value value;
     Wasm::ValueType type;
 };
 
 static void sigint_handler(int)
 {
     if (!g_continue) {
         signal(SIGINT, old_signal);
         kill(getpid(), SIGINT);
     }
     g_continue = false;
 }
 
 static Optional<u128> convert_to_uint(StringView string_view)
 {
     if (string_view.is_empty())
         return {};
 
     u128 value = 0;
     auto characters = string_view.characters_without_null_termination();
     for (size_t i = 0; i < string_view.length(); i++) {
         if (characters[i] < '0' || characters[i] > '9')
             return {};
 
         value *= 10;
         value += u128 { static_cast<u64>(characters[i] - '0'), 0 };
     }
     return value;
 }
 
 static Optional<u128> convert_to_uint_from_hex(StringView string_view)
 {
     if (string_view.is_empty())
         return {};
 
     u128 value = 0;
     auto const count = string_view.length();
     auto const upper_bound = NumericLimits<u128>::max();
 
     for (size_t i = 0; i < count; i++) {
         char digit = string_view[i];
         if (value > (upper_bound >> 4))
             return {};
 
         auto digit_val = decode_hex_digit(digit);
         if (digit_val == 255)
             return {};
 
         value = (value << 4) + digit_val;
     }
     return value;
 }
 
 static ErrorOr<ParsedValue> parse_value_string(StringView spec)
 {
     constexpr auto is_separator = [](char c) { return is_ascii_space(c) || c == ':'; };
 
     auto parse_scalar = []<typename T>(StringView text) -> ErrorOr<Wasm::Value> {
         // Handle NaN/Inf for floating-point types
         if constexpr (IsFloatingPoint<T>) {
             auto trimmed = text.trim_whitespace();
             if (trimmed.equals_ignoring_ascii_case("nan"sv)) {
                 if constexpr (IsSame<T, float>)
                     return Wasm::Value { nanf("") };
                 else
                     return Wasm::Value { nan("") };
             }
             if (trimmed.equals_ignoring_ascii_case("inf"sv)) {
                 if constexpr (IsSame<T, float>)
                     return Wasm::Value { HUGE_VALF };
                 else
                     return Wasm::Value { HUGE_VAL };
             }
         }
 
         // Numeric conversion
         auto maybe_value = text.to_number<T>();
         if (!maybe_value.has_value())
             return Error::from_string_literal("Invalid scalar value");
         return Wasm::Value { *maybe_value };
     };
 
     // Vector parse: v128.const, or v(...)
     auto parse_u128_value = [](StringView text) -> ErrorOr<Wasm::Value> {
         u128 vector_value;
         if (text.starts_with("0x"sv)) {
             auto maybe_hex = convert_to_uint_from_hex(text);
             if (!maybe_hex.has_value())
                 return Error::from_string_literal("Invalid hex v128 value");
             vector_value = *maybe_hex;
         } else {
             auto maybe_val = convert_to_uint(text);
             if (!maybe_val.has_value())
                 return Error::from_string_literal("Invalid v128 value");
             vector_value = *maybe_val;
         }
         return Wasm::Value { vector_value };
     };
 
     GenericLexer lexer(spec);
     if (lexer.consume_specific("v128.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_u128_value(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::V128)
         };
     }
 
     if (lexer.consume_specific("i8.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_scalar.operator()<i8>(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::I32)
         };
     }
     if (lexer.consume_specific("i16.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_scalar.operator()<i16>(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::I32)
         };
     }
     if (lexer.consume_specific("i32.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_scalar.operator()<i32>(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::I32)
         };
     }
     if (lexer.consume_specific("i64.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_scalar.operator()<i64>(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::I64)
         };
     }
     if (lexer.consume_specific("f32.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_scalar.operator()<float>(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::F32)
         };
     }
     if (lexer.consume_specific("f64.const"sv)) {
         lexer.ignore_while(is_separator);
         auto text = lexer.consume_all();
         return ParsedValue {
             .value = TRY(parse_scalar.operator()<double>(text)),
             .type = Wasm::ValueType(Wasm::ValueType::Kind::F64)
         };
     }
 
     // Parse vector of typed values
     if (lexer.consume_specific("v("sv)) {
         Vector<ParsedValue> parsed_values;
         for (;;) {
             lexer.ignore_while(is_separator);
             if (lexer.consume_specific(")"sv))
                 break;
             if (lexer.is_eof()) {
                 warnln("Expected ')' to close vector");
                 break;
             }
             auto consumed_text = lexer.consume_until(is_any_of(",)"sv));
             auto value = parse_value_string(consumed_text);
             if (value.is_error())
                 return value.release_error();
             lexer.consume_specific(',');
             parsed_values.append(value.release_value());
         }
 
         if (parsed_values.is_empty())
             return Error::from_string_literal("Empty vector");
 
         // Ensure all elements have the same type
         auto element_type = parsed_values.first().type;
         for (auto& val : parsed_values) {
             if (val.type != element_type)
                 return Error::from_string_literal("Mixed types in vector");
         }
 
         unsigned total_size = 0;
         unsigned width = 0;
         u128 result_bits = 0;
         u128 last_value = 0;
         for (auto& parsed : parsed_values) {
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
                 VERIFY_NOT_REACHED();
             }
             last_value = parsed.value.value();
             result_bits |= last_value << total_size;
             total_size += width * 8;
         }
 
         // Fill with repeated last element if vector is under 128 bits
         if (total_size < 128)
             warnln("Vector '{}' is only {} bits wide, repeating last element", spec, total_size);
         while (total_size < 128) {
             result_bits |= last_value << total_size;
             total_size += width * 8;
         }
 
         return ParsedValue {
             .value = Wasm::Value { result_bits },
             .type = Wasm::ValueType(Wasm::ValueType::Kind::V128)
         };
     }
 
     return Error::from_string_literal("Invalid value");
 }
 
 static bool post_interpret_hook(Wasm::Configuration&, Wasm::InstructionPointer& ip, Wasm::Instruction const& instruction, Wasm::Interpreter const& interpreter)
 {
     if (interpreter.did_trap()) {
         g_continue = false;
         warnln("Trapped when executing ip={}", ip);
         g_printer->print(instruction);
         warnln("Trap reason: {}", interpreter.trap_reason());
         const_cast<Wasm::Interpreter&>(interpreter).clear_trap();
     }
     return true;
 }
 
 static bool pre_interpret_hook(Wasm::Configuration& config, Wasm::InstructionPointer& ip, Wasm::Instruction const& instruction)
 {
     static bool always_print_stack = false;
     static bool always_print_instruction = false;
 
     if (always_print_stack)
         config.dump_stack();
     if (always_print_instruction) {
         g_stdout->write_until_depleted(ByteString::formatted("{:0>4} ", ip.value())).release_value_but_fixme_should_propagate_errors();
         g_printer->print(instruction);
     }
 
     if (g_continue)
         return true;
 
     g_stdout->write_until_depleted(ByteString::formatted("{:0>4} ", ip.value())).release_value_but_fixme_should_propagate_errors();
     g_printer->print(instruction);
     ByteString last_command;
     for (;;) {
         auto result = g_line_editor->get_line("> ");
         if (result.is_error())
             return false;
 
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
             warnln("- [h]elp                     Print this help\n");
             warnln("Print:");
             warnln("- print [s]tack              Print the contents of the stack, including frames and labels");
             warnln("- print [[m]em]ory <index>   Print the contents of the memory identified by <index>");
             warnln("- print [[i]nstr]uction      Print the current instruction");
             warnln("- print [[f]unc]tion <index> Print the function identified by <index>\n");
             warnln("Settings:");
             warnln("- set print stack            Make the shell print the stack on every instruction executed");
             warnln("- set print [instr]uction    Make the shell print the instruction that will be executed next");
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
                 auto mem_index = args[2].to_number<u64>();
                 if (!mem_index.has_value()) {
                     warnln("invalid memory index {}", args[2]);
                     continue;
                 }
                 auto memory = config.store().get(Wasm::MemoryAddress(mem_index.value()));
                 if (!memory) {
                     warnln("invalid memory index {} (not found)", args[2]);
                     continue;
                 }
                 warnln("{:>32hex-dump}", memory->data().bytes());
                 continue;
             }
             if (what.is_one_of("i", "instr", "instruction")) {
                 g_printer->print(instruction);
                 continue;
             }
             if (what.is_one_of("f", "func", "function")) {
                 if (args.size() < 3) {
                     warnln("print what function?");
                     continue;
                 }
                 auto func_index = args[2].to_number<u64>();
                 if (!func_index.has_value()) {
                     warnln("invalid function index {}", args[2]);
                     continue;
                 }
                 auto func = config.store().get(Wasm::FunctionAddress(func_index.value()));
                 if (!func) {
                     warnln("invalid function index {} (not found)", args[2]);
                     continue;
                 }
                 if (auto* host_fn = func->get_pointer<Wasm::HostFunction>()) {
                     warnln("Host function at {:p}", &host_fn->function());
                     continue;
                 }
                 if (auto* wasm_fn = func->get_pointer<Wasm::WasmFunction>()) {
                     g_printer->print(wasm_fn->code());
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
             auto index_maybe = args[1].to_number<u64>();
             if (index_maybe.has_value()) {
                 address = config.frame().module().functions()[index_maybe.value()];
             } else {
                 // Try to find by function name
                 auto& name = args[1];
                 for (auto& export_entry : config.frame().module().exports()) {
                     if (export_entry.name() == name) {
                         if (auto fn_address = export_entry.value().get_pointer<Wasm::FunctionAddress>()) {
                             address = *fn_address;
                             break;
                         }
                     }
                 }
             }
 
             if (!address.has_value()) {
                 warnln("Could not find a function {}", args[1]);
                 continue;
             }
 
             auto fn = config.store().get(*address);
             if (!fn) {
                 warnln("Could not find a function {}", args[1]);
                 continue;
             }
 
             auto type = fn->visit([&](auto& value) { return value.type(); });
             if (type.parameters().size() + 2 != args.size()) {
                 warnln("Expected {} arguments for call, but found {}", type.parameters().size(), args.size() - 2);
                 continue;
             }
 
             Vector<ParsedValue> temp_vectors;
             Vector<Wasm::Value> call_values;
             bool parse_failed = false;
 
             for (size_t i = 2; i < args.size(); ++i) {
                 auto parse_result = parse_value_string(args[i]);
                 if (parse_result.is_error()) {
                     warnln("Failed to parse argument {}: {}", args[i], parse_result.error());
                     parse_failed = true;
                     break;
                 }
                 temp_vectors.append(parse_result.release_value());
             }
             if (parse_failed)
                 continue;
 
             // Type-check the arguments
             for (auto param : type.parameters()) {
                 auto val = temp_vectors.take_last();
                 if (val.type != param) {
                     warnln("Type mismatch in argument: expected {}, but got {}",
                            Wasm::ValueType::kind_name(param.kind()),
                            Wasm::ValueType::kind_name(val.type.kind()));
                     parse_failed = true;
                     break;
                 }
                 call_values.append(val.value);
             }
             if (parse_failed)
                 continue;
 
             Wasm::Result call_result { Wasm::Trap {} };
             {
                 Wasm::BytecodeInterpreter::CallFrameHandle handle { g_interpreter, config };
                 call_result = config.call(g_interpreter, *address, move(call_values)).assert_wasm_result();
             }
             if (call_result.is_trap()) {
                 warnln("Execution trapped: {}", call_result.trap().reason);
             } else {
                 if (!call_result.values().is_empty())
                     warnln("Returned:");
                 size_t idx = 0;
                 for (auto& result_value : call_result.values()) {
                     g_stdout->write_until_depleted("  -> "sv.bytes()).release_value_but_fixme_should_propagate_errors();
                     g_printer->print(result_value, type.results()[idx]);
                     ++idx;
                 }
             }
             continue;
         }
         if (cmd.is_one_of("set", "unset")) {
             bool enable_option = !cmd.starts_with('u');
             if (args.size() < 3) {
                 warnln("(un)set what (to what)?");
                 continue;
             }
             if (args[1] == "print"sv) {
                 if (args[2] == "stack"sv)
                     always_print_stack = enable_option;
                 else if (args[2].is_one_of("instr", "instruction"))
                     always_print_instruction = enable_option;
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
 
 static RefPtr<Wasm::Module> parse_wasm_file(StringView filepath)
 {
     auto result = Core::MappedFile::map(filepath);
     if (result.is_error()) {
         warnln("Failed to open {}: {}", filepath, result.error());
         return {};
     }
 
     auto parse_result = Wasm::Module::parse(*result.value());
     if (parse_result.is_error()) {
         warnln("Something went wrong while parsing (invalid file or potential LibWasm bug)!");
         warnln("Parse error: {}", Wasm::parse_error_to_byte_string(parse_result.error()));
         return {};
     }
     return parse_result.release_value();
 }
 
 static void display_link_error(Wasm::LinkError const& error)
 {
     for (auto const& missing : error.missing_imports)
         warnln("Missing import '{}'", missing);
 }
 
 ErrorOr<int> serenity_main(Main::Arguments arguments)
 {
     StringView input_filename;
     bool should_print = false;
     bool should_instantiate = false;
     bool is_debug_mode = false;
     bool should_export_noop = false;
     bool is_shell_mode = false;
     bool is_wasi_enabled = false;
 
     ByteString function_to_execute;
     Vector<ParsedValue> values_to_push;
     Vector<ByteString> modules_to_link;
     Vector<StringView> wasi_args;
     Vector<StringView> wasi_mapped_dirs;
 
     Core::ArgsParser parser;
     parser.add_positional_argument(input_filename, "File name to parse", "file");
     parser.add_option(is_debug_mode, "Open a debugger", "debug", 'd');
     parser.add_option(should_print, "Print the parsed module", "print", 'p');
     parser.add_option(should_instantiate, "Instantiate the module", "instantiate", 'i');
     parser.add_option(function_to_execute,
                       "Execute the named exported function from the module (implies -i)", "execute", 'e', "name");
     parser.add_option(should_export_noop, "Export noop functions corresponding to imports", "export-noop");
     parser.add_option(is_shell_mode, "Launch a REPL (implies -i)", "shell", 's');
     parser.add_option(is_wasi_enabled, "Enable WASI", "wasi", 'w');
     parser.add_option(Core::ArgsParser::Option {
         .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
         .help_string = "Directory mappings to expose via WASI",
         .long_name = "wasi-map-dir",
         .short_name = 0,
         .value_name = "path[:path]",
         .accept_value = [&](StringView text) {
             if (!text.is_empty()) {
                 wasi_mapped_dirs.append(text);
                 return true;
             }
             return false;
         },
     });
     parser.add_option(Core::ArgsParser::Option {
         .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
         .help_string = "Extra modules to link with (use to resolve imports)",
         .long_name = "link",
         .short_name = 'l',
         .value_name = "file",
         .accept_value = [&](StringView text) {
             if (!text.is_empty()) {
                 modules_to_link.append(text);
                 return true;
             }
             return false;
         },
     });
     parser.add_option(Core::ArgsParser::Option {
         .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
         .help_string = "Supply arguments (default=0) in the form T.const:v or v(T.const:v,...)",
         .long_name = "arg",
         .short_name = 0,
         .value_name = "value",
         .accept_value = [&](StringView text) -> bool {
             auto result = parse_value_string(text);
             if (result.is_error()) {
                 warnln("Failed to parse value: {}", result.error());
                 return false;
             }
             values_to_push.append(result.release_value());
             return true;
         },
     });
 
     parser.add_positional_argument(wasi_args, "Arguments to pass to the WASI module", "args", Core::ArgsParser::Required::No);
     parser.parse(arguments);
 
     if (is_shell_mode) {
         is_debug_mode = true;
         should_instantiate = true;
     }
     if (!is_shell_mode && is_debug_mode && function_to_execute.is_empty()) {
         warnln("Debug what? (pass -e <function>)");
         return 1;
     }
     if (is_debug_mode || is_shell_mode)
         old_signal = signal(SIGINT, sigint_handler);
 
     if (!function_to_execute.is_empty())
         should_instantiate = true;
 
     auto module_parse_result = parse_wasm_file(input_filename);
     if (module_parse_result.is_null())
         return 1;
 
     g_stdout = TRY(Core::File::standard_output());
     g_printer = TRY(try_make<Wasm::Printer>(*g_stdout));
 
     // Print the module, if requested and no instantiation is required.
     if (should_print && !should_instantiate) {
         Wasm::Printer printer(*g_stdout);
         printer.print(*module_parse_result);
     }
 
     // Handle instantiation
     if (should_instantiate) {
         Wasm::AbstractMachine abstract_machine;
         Optional<Wasm::Wasi::Implementation> wasi_impl;
 
         if (is_wasi_enabled) {
             wasi_impl.emplace(Wasm::Wasi::Implementation::Details {
                 .provide_arguments = [&] {
                     Vector<String> strings;
                     for (auto& utf8_str : wasi_args)
                         strings.append(String::from_utf8(utf8_str).release_value_but_fixme_should_propagate_errors());
                     return strings;
                 },
                 .provide_environment = {},
                 .provide_preopened_directories = [&] {
                     Vector<Wasm::Wasi::Implementation::MappedPath> paths;
                     for (auto& mapping : wasi_mapped_dirs) {
                         auto maybe_split = mapping.find(':');
                         if (maybe_split.has_value()) {
                             LexicalPath host_path { FileSystem::real_path(mapping.substring_view(0, *maybe_split)).release_value_but_fixme_should_propagate_errors() };
                             LexicalPath mapped_path { mapping.substring_view(*maybe_split + 1) };
                             paths.append({ move(host_path), move(mapped_path) });
                         } else {
                             LexicalPath host_path { FileSystem::real_path(mapping).release_value_but_fixme_should_propagate_errors() };
                             LexicalPath mapped_path { mapping };
                             paths.append({ move(host_path), move(mapped_path) });
                         }
                     }
                     return paths;
                 },
             });
         }
 
         Core::EventLoop main_loop;
         if (is_debug_mode) {
             g_line_editor = Line::Editor::construct();
             g_interpreter.pre_interpret_hook = pre_interpret_hook;
             g_interpreter.post_interpret_hook = post_interpret_hook;
         }
 
         // Link extra modules
         Vector<NonnullOwnPtr<Wasm::ModuleInstance>> linked_instances;
         Vector<NonnullRefPtr<Wasm::Module>> linked_modules;
         for (auto& name : modules_to_link) {
             auto linked_parse_result = parse_wasm_file(name);
             if (linked_parse_result.is_null()) {
                 warnln("Failed to parse linked module '{}'", name);
                 return 1;
             }
             linked_modules.append(linked_parse_result.release_nonnull());
             Wasm::Linker linker { linked_modules.last() };
             for (auto& instance : linked_instances)
                 linker.link(*instance);
 
             auto link_result = linker.finish();
             if (link_result.is_error()) {
                 warnln("Linking imported module '{}' failed", name);
                 display_link_error(link_result.error());
                 return 1;
             }
             auto instantiation_result = abstract_machine.instantiate(linked_modules.last(), link_result.release_value());
             if (instantiation_result.is_error()) {
                 warnln("Instantiation of imported module '{}' failed: {}", name, instantiation_result.error().error);
                 return 1;
             }
             linked_instances.append(instantiation_result.release_value());
         }
 
         // Build main module link
         Wasm::Linker main_linker { *module_parse_result };
         for (auto& instance : linked_instances)
             main_linker.link(*instance);
 
         if (is_wasi_enabled) {
             HashMap<Wasm::Linker::Name, Wasm::ExternValue> wasi_exports;
             for (auto& entry : main_linker.unresolved_imports()) {
                 if (entry.module != "wasi_snapshot_preview1"sv)
                     continue;
                 auto function = wasi_impl->function_by_name(entry.name);
                 if (function.is_error()) {
                     dbgln("wasi function '{}' not implemented", entry.name);
                     continue;
                 }
                 auto address = abstract_machine.store().allocate(function.release_value());
                 wasi_exports.set(entry, *address);
             }
             main_linker.link(wasi_exports);
         }
 
         if (should_export_noop) {
             HashMap<Wasm::Linker::Name, Wasm::ExternValue> exports;
             for (auto& entry : main_linker.unresolved_imports()) {
                 if (!entry.type.has<Wasm::TypeIndex>())
                     continue;
 
                 auto type_entry = module_parse_result->type_section().types()[entry.type.get<Wasm::TypeIndex>().value()];
                 auto address = abstract_machine.store().allocate(Wasm::HostFunction(
                     [name = entry.name, type = type_entry](auto&, auto& arguments) -> Wasm::Result {
                         StringBuilder builder;
                         bool first = true;
                         size_t idx = 0;
                         for (auto& argument : arguments) {
                             AllocatingMemoryStream temp_stream;
                             auto val_type = type.parameters()[idx];
                             Wasm::Printer { temp_stream }.print(argument, val_type);
                             if (first)
                                 first = false;
                             else
                                 builder.append(", "sv);
 
                             auto buffer = ByteBuffer::create_uninitialized(temp_stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
                             temp_stream.read_until_filled(buffer).release_value_but_fixme_should_propagate_errors();
                             builder.append(StringView(buffer).trim_whitespace());
                             ++idx;
                         }
                         dbgln("[wasm runtime] Stub function '{}' called with arguments: {}", name, builder.to_byte_string());
                         Vector<Wasm::Value> results;
                         results.ensure_capacity(type.results().size());
                         for (auto result_type : type.results())
                             results.append(Wasm::Value(result_type));
 
                         return Wasm::Result { move(results) };
                     },
                     type_entry,
                     entry.name));
                 exports.set(entry, *address);
             }
             main_linker.link(exports);
         }
 
         auto link_result = main_linker.finish();
         if (link_result.is_error()) {
             warnln("Linking main module failed");
             display_link_error(link_result.error());
             return 1;
         }
 
         auto instantiation_result = abstract_machine.instantiate(*module_parse_result, link_result.release_value());
         if (instantiation_result.is_error()) {
             warnln("Module instantiation failed: {}", instantiation_result.error().error);
             return 1;
         }
         auto module_instance = instantiation_result.release_value();
 
         auto start_debugger_repl = [&] {
             Wasm::Configuration config { abstract_machine.store() };
             Wasm::Expression expression { {} };
             config.set_frame(Wasm::Frame {
                 *module_instance,
                 {},
                 expression,
                 0,
             });
             Wasm::Instruction instruction { Wasm::Instructions::nop };
             Wasm::InstructionPointer ip { 0 };
             g_continue = false;
             pre_interpret_hook(config, ip, instruction);
         };
 
         auto display_function_details = [&](auto const& address) {
             Wasm::FunctionInstance* fn = abstract_machine.store().get(address);
             g_stdout->write_until_depleted(ByteString::formatted("- Function addr {}, ptr={}\n", address.value(), fn))
                 .release_value_but_fixme_should_propagate_errors();
             if (!fn)
                 return;
             g_stdout->write_until_depleted(
                 ByteString::formatted("    wasm function? {}\n", fn->has<Wasm::WasmFunction>()))
                 .release_value_but_fixme_should_propagate_errors();
             fn->visit(
                 [&](Wasm::WasmFunction const& wasm_fn) {
                     Wasm::Printer verbose_printer { *g_stdout, 3 };
                     g_stdout->write_until_depleted("    type:\n"sv.bytes()).release_value_but_fixme_should_propagate_errors();
                     verbose_printer.print(wasm_fn.type());
                     g_stdout->write_until_depleted("    code:\n"sv.bytes()).release_value_but_fixme_should_propagate_errors();
                     verbose_printer.print(wasm_fn.code());
                 },
                 [](Wasm::HostFunction const&) {});
         };
 
         // If requested, display all functions in the instantiated module
         if (should_print) {
             for (auto& function_address : module_instance->functions()) {
                 display_function_details(function_address);
             }
         }
 
         if (is_shell_mode) {
             start_debugger_repl();
             return 0;
         }
 
         // Execute specific exported function if provided
         if (!function_to_execute.is_empty()) {
             Optional<Wasm::FunctionAddress> run_address;
             Vector<Wasm::Value> call_values;
             for (auto& entry : module_instance->exports()) {
                 if (entry.name() == function_to_execute) {
                     if (auto addr = entry.value().get_pointer<Wasm::FunctionAddress>())
                         run_address = *addr;
                 }
             }
             if (!run_address.has_value()) {
                 warnln("No such exported function: {}", function_to_execute);
                 return 1;
             }
 
             auto instance = abstract_machine.store().get(*run_address);
             VERIFY(instance);
 
             if (instance->has<Wasm::HostFunction>()) {
                 warnln("Exported function is a host function; cannot run that yet");
                 return 1;
             }
 
             auto& wasm_function = instance->get<Wasm::WasmFunction>();
             for (auto& param : wasm_function.type().parameters()) {
                 if (values_to_push.is_empty()) {
                     call_values.append(Wasm::Value(param));
                 } else if (param == values_to_push.last().type) {
                     call_values.append(values_to_push.take_last().value);
                 } else {
                     warnln("Type mismatch in argument: expected {}, got {}",
                            Wasm::ValueType::kind_name(param.kind()),
                            Wasm::ValueType::kind_name(values_to_push.last().type.kind()));
                     return 1;
                 }
             }
 
             if (should_print) {
                 outln("Executing function '{}':", function_to_execute);
                 display_function_details(*run_address);
                 outln();
             }
 
             auto invoke_result = abstract_machine.invoke(g_interpreter, run_address.value(), move(call_values))
                 .assert_wasm_result();
 
             if (is_debug_mode)
                 start_debugger_repl();
 
             if (invoke_result.is_trap()) {
                 if (invoke_result.trap().reason.starts_with("exit:"sv))
                     return -invoke_result.trap().reason.substring_view(5).to_number<i32>().value_or(-1);
                 warnln("Execution trapped: {}", invoke_result.trap().reason);
             } else {
                 if (!invoke_result.values().is_empty())
                     warnln("Returned:");
                 auto result_types = wasm_function.type().results();
                 size_t idx = 0;
                 for (auto& value : invoke_result.values()) {
                     g_stdout->write_until_depleted("  -> "sv.bytes()).release_value_but_fixme_should_propagate_errors();
                     g_printer->print(value, result_types[idx]);
                     ++idx;
                 }
             }
         }
     }
     return 0;
 }
