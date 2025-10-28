/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020-2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/StandardPaths.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Console.h>
#include <LibJS/Contrib/Test262/GlobalObject.h>
#include <LibJS/Parser.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/SourceTextModule.h>
#include <LibMain/Main.h>
#include <LibTextCodec/Decoder.h>
#include <signal.h>

#if !defined(AK_OS_WINDOWS)
#    include <LibLine/Editor.h>
#endif

// FIXME: https://github.com/LadybirdBrowser/ladybird/issues/2412
//    We should be able to destroy the VM on process exit.
NeverDestroyed<RefPtr<JS::VM>> g_vm_storage;
JS::VM* g_vm;
Vector<String> g_repl_statements;
GC::Root<JS::Value> g_last_value = GC::make_root(JS::js_undefined());

class ReplObject final : public JS::GlobalObject {
    JS_OBJECT(ReplObject, JS::GlobalObject);

public:
    ReplObject(JS::Realm& realm)
        : GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~ReplObject() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(exit_interpreter);
    JS_DECLARE_NATIVE_FUNCTION(repl_help);
    JS_DECLARE_NATIVE_FUNCTION(save_to_file);
    JS_DECLARE_NATIVE_FUNCTION(load_ini);
    JS_DECLARE_NATIVE_FUNCTION(load_json);
    JS_DECLARE_NATIVE_FUNCTION(last_value_getter);
    JS_DECLARE_NATIVE_FUNCTION(print);
};

class ScriptObject final : public JS::GlobalObject {
    JS_OBJECT(ScriptObject, JS::GlobalObject);

public:
    ScriptObject(JS::Realm& realm)
        : JS::GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~ScriptObject() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(load_ini);
    JS_DECLARE_NATIVE_FUNCTION(load_json);
    JS_DECLARE_NATIVE_FUNCTION(print);
};

static bool s_dump_ast = false;
static bool s_as_module = false;
static bool s_print_last_result = false;
static bool s_strip_ansi = false;
static bool s_raw_strings = false;
static bool s_disable_source_location_hints = false;
#if !defined(AK_OS_WINDOWS)
static RefPtr<Line::Editor> s_editor;
#endif
static String s_history_path = String {};
[[maybe_unused]] static int s_repl_line_level = 0;
static bool s_keep_running_repl = true;
static int s_exit_code = 0;

static ErrorOr<void> print_inline(JS::Value value, Stream& stream)
{
    JS::PrintContext print_context { .vm = *g_vm, .stream = stream, .strip_ansi = s_strip_ansi, .raw_strings = s_raw_strings };
    return JS::print(value, print_context);
}

enum class PrintTarget {
    StandardError,
    StandardOutput,
};

static ErrorOr<NonnullOwnPtr<Stream>> flushed_print_stream(PrintTarget target)
{
    if (target == PrintTarget::StandardOutput) {
        (void)fflush(stdout);
        return Core::File::standard_output();
    }

    (void)fflush(stderr);
    return Core::File::standard_error();
}

enum class PrintEnd {
    Newline,
    None
};

static ErrorOr<void> print(JS::Value value, PrintTarget target = PrintTarget::StandardOutput, PrintEnd end = PrintEnd::Newline)
{
    auto stream = TRY(flushed_print_stream(target));

    TRY(print_inline(value, *stream));

    if (end == PrintEnd::Newline)
        TRY(stream->write_until_depleted("\n"sv));

    return {};
}

static ErrorOr<void> print_all_arguments(JS::VM const& vm, PrintTarget target = PrintTarget::StandardOutput, PrintEnd end = PrintEnd::Newline)
{
    auto stream = TRY(flushed_print_stream(target));

    for (size_t i = 0; i < vm.argument_count(); i++) {
        TRY(print_inline(vm.argument(i), *stream));

        if (i < vm.argument_count() - 1) {
            TRY(stream->write_until_depleted(" "sv));
        }
    }

    if (end == PrintEnd::Newline)
        TRY(stream->write_until_depleted("\n"sv));

    return {};
}

static size_t s_ctrl_c_hit_count = 0;
[[maybe_unused]] static ErrorOr<String> prompt_for_level(int level)
{
    static StringBuilder prompt_builder;
    prompt_builder.clear();
    if (s_ctrl_c_hit_count > 0)
        prompt_builder.append("(Use Ctrl+C again to exit)\n"sv);
    prompt_builder.append("> "sv);

    for (auto i = 0; i < level; ++i)
        prompt_builder.append("    "sv);

    return prompt_builder.to_string();
}

static ErrorOr<void> write_to_file(String const& path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write, 0666));
    for (size_t i = 0; i < g_repl_statements.size(); i++) {
        auto line = g_repl_statements[i].bytes();
        if (line.size() > 0 && i != g_repl_statements.size() - 1) {
            TRY(file->write_until_depleted(line));
        }
        if (i != g_repl_statements.size() - 1) {
            TRY(file->write_value('\n'));
        }
    }
    file->close();
    return {};
}

static ErrorOr<bool> parse_and_run(JS::Realm& realm, StringView source, StringView source_name)
{
    auto& vm = realm.vm();

    JS::ThrowCompletionOr<JS::Value> result { JS::js_undefined() };

    auto run_script_or_module = [&](auto& script_or_module) {
        if (s_dump_ast)
            script_or_module->parse_node().dump(0);

        result = vm.bytecode_interpreter().run(*script_or_module);
    };

    if (!s_as_module) {
        auto script_or_error = JS::Script::parse(source, realm, source_name);
        if (script_or_error.is_error()) {
            auto utf16_source = Utf16String::from_utf8(source);

            auto error = script_or_error.error()[0];
            auto hint = error.source_location_hint(utf16_source);
            if (!hint.is_empty())
                outln("{}", hint);

            auto error_string = error.to_string();
            outln("{}", error_string);
            result = vm.throw_completion<JS::SyntaxError>(move(error_string));
        } else {
            run_script_or_module(script_or_error.value());
        }
    } else {
        auto module_or_error = JS::SourceTextModule::parse(source, realm, source_name);
        if (module_or_error.is_error()) {
            auto utf16_source = Utf16String::from_utf8(source);

            auto error = module_or_error.error()[0];
            auto hint = error.source_location_hint(utf16_source);
            if (!hint.is_empty())
                outln("{}", hint);

            auto error_string = error.to_string();
            outln("{}", error_string);
            result = vm.throw_completion<JS::SyntaxError>(move(error_string));
        } else {
            run_script_or_module(module_or_error.value());
        }
    }

    auto handle_exception = [&](JS::Value thrown_value) -> ErrorOr<void> {
        warnln("Uncaught exception: ");
        TRY(print(thrown_value, PrintTarget::StandardError));

        if (!thrown_value.is_object() || !is<JS::Error>(thrown_value.as_object()))
            return {};
        warnln("{}", static_cast<JS::Error const&>(thrown_value.as_object()).stack_string(JS::CompactTraceback::Yes));
        return {};
    };

    if (!result.is_error())
        g_last_value = GC::make_root(result.value());

    if (result.is_error()) {
        TRY(handle_exception(result.release_error().value()));
        return false;
    }

    if (s_print_last_result) {
        TRY(print(result.value()));
    }

    return true;
}

static JS::ThrowCompletionOr<JS::Value> load_ini_impl(JS::VM& vm)
{
    auto& realm = *vm.current_realm();

    auto filename = TRY(vm.argument(0).to_byte_string(vm));
    auto file_or_error = Core::File::open(filename, Core::File::OpenMode::Read);
    if (file_or_error.is_error())
        return vm.throw_completion<JS::Error>(TRY_OR_THROW_OOM(vm, String::formatted("Failed to open '{}': {}", filename, file_or_error.error())));

    auto config_file = MUST(Core::ConfigFile::open(filename, file_or_error.release_value()));
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto const& group : config_file->groups()) {
        auto group_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
        for (auto const& key : config_file->keys(group)) {
            auto entry = config_file->read_entry(group, key);
            group_object->define_direct_property(Utf16String::from_utf8(key), JS::PrimitiveString::create(vm, move(entry)), JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);
        }
        object->define_direct_property(Utf16String::from_utf8(group), group_object, JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);
    }
    return object;
}

static JS::ThrowCompletionOr<JS::Value> load_json_impl(JS::VM& vm)
{
    auto filename = TRY(vm.argument(0).to_string(vm));
    auto file_or_error = Core::File::open(filename, Core::File::OpenMode::Read);
    if (file_or_error.is_error())
        return vm.throw_completion<JS::Error>(TRY_OR_THROW_OOM(vm, String::formatted("Failed to open '{}': {}", filename, file_or_error.error())));

    auto file_contents_or_error = file_or_error.value()->read_until_eof();
    if (file_contents_or_error.is_error())
        return vm.throw_completion<JS::Error>(TRY_OR_THROW_OOM(vm, String::formatted("Failed to read '{}': {}", filename, file_contents_or_error.error())));

    auto json = JsonValue::from_string(file_contents_or_error.value());
    if (json.is_error())
        return vm.throw_completion<JS::SyntaxError>(JS::ErrorType::JsonMalformed);

    return JS::JSONObject::parse_json_value(vm, json.value());
}

void ReplObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global"_utf16_fly_string, this, JS::Attribute::Enumerable);
    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "exit"_utf16_fly_string, exit_interpreter, 0, attr);
    define_native_function(realm, "help"_utf16_fly_string, repl_help, 0, attr);
    define_native_function(realm, "save"_utf16_fly_string, save_to_file, 1, attr);
    define_native_function(realm, "loadINI"_utf16_fly_string, load_ini, 1, attr);
    define_native_function(realm, "loadJSON"_utf16_fly_string, load_json, 1, attr);
    define_native_function(realm, "print"_utf16_fly_string, print, 1, attr);

    define_native_accessor(
        realm,
        "_"_utf16_fly_string,
        [](JS::VM&) {
            return g_last_value.value();
        },
        [](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto& global_object = vm.get_global_object();
            VERIFY(is<ReplObject>(global_object));
            outln("Disable writing last value to '_'");

            // We must delete first otherwise this setter gets called recursively.
            TRY(global_object.internal_delete(vm.names._));

            auto value = vm.argument(0);
            TRY(global_object.internal_set(vm.names._, value, &global_object));
            return value;
        },
        attr);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::save_to_file)
{
    if (!vm.argument_count())
        return JS::Value(false);
    auto const save_path = TRY(vm.argument(0).to_string(vm));
    if (!write_to_file(save_path).is_error()) {
        return JS::Value(true);
    }
    return JS::Value(false);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::exit_interpreter)
{
    if (vm.argument_count() != 0)
        s_exit_code = TRY(vm.argument(0).to_number(vm)).as_double();

    s_keep_running_repl = false;
    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::repl_help)
{
    warnln("REPL commands:");
    warnln("    exit(code): exit the REPL with specified code. Defaults to 0.");
    warnln("    help(): display this menu");
    warnln("    loadINI(file): load the given file as INI.");
    warnln("    loadJSON(file): load the given file as JSON.");
    warnln("    print(value): pretty-print the given JS value.");
    warnln("    save(file): write REPL input history to the given file. For example: save(\"foo.txt\")");
    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::load_ini)
{
    return load_ini_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::load_json)
{
    return load_json_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::print)
{
    auto result = print_all_arguments(vm);
    if (result.is_error())
        return g_vm->throw_completion<JS::InternalError>(TRY_OR_THROW_OOM(*g_vm, String::formatted("Failed to print value(s): {}", result.error())));

    return JS::js_undefined();
}

void ScriptObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global"_utf16_fly_string, this, JS::Attribute::Enumerable);
    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "loadINI"_utf16_fly_string, load_ini, 1, attr);
    define_native_function(realm, "loadJSON"_utf16_fly_string, load_json, 1, attr);
    define_native_function(realm, "print"_utf16_fly_string, print, 1, attr);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::load_ini)
{
    return load_ini_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::load_json)
{
    return load_json_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::print)
{
    auto result = print_all_arguments(vm);
    if (result.is_error())
        return g_vm->throw_completion<JS::InternalError>(TRY_OR_THROW_OOM(*g_vm, String::formatted("Failed to print value(s): {}", result.error())));

    return JS::js_undefined();
}

class ReplConsoleClient final : public JS::ConsoleClient {
    GC_CELL(ReplConsoleClient, JS::ConsoleClient);

public:
    ReplConsoleClient(JS::Console& console)
        : ConsoleClient(console)
    {
    }

    virtual void clear() override
    {
        out("\033[3J\033[H\033[2J");
        m_group_stack_depth = 0;
        fflush(stdout);
    }

    virtual void end_group() override
    {
        if (m_group_stack_depth > 0)
            m_group_stack_depth--;
    }

    // 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel log_level, PrinterArguments arguments) override
    {
        auto indent = TRY_OR_THROW_OOM(*g_vm, String::repeated(' ', m_group_stack_depth * 2));

        if (log_level == JS::Console::LogLevel::Trace) {
            auto trace = arguments.get<JS::Console::Trace>();
            StringBuilder builder;
            if (!trace.label.is_empty())
                builder.appendff("{}\033[36;1m{}\033[0m\n", indent, trace.label);

            for (auto& function_name : trace.stack)
                builder.appendff("{}-> {}\n", indent, function_name);

            outln("{}", builder.string_view());
            return JS::js_undefined();
        }

        if (log_level == JS::Console::LogLevel::Group || log_level == JS::Console::LogLevel::GroupCollapsed) {
            auto group = arguments.get<JS::Console::Group>();
            outln("{}\033[36;1m{}\033[0m", indent, group.label);
            m_group_stack_depth++;
            return JS::js_undefined();
        }

        auto output = TRY(generically_format_values(arguments.get<GC::RootVector<JS::Value>>()));

        switch (log_level) {
        case JS::Console::LogLevel::Debug:
            outln("{}\033[36;1m{}\033[0m", indent, output);
            break;
        case JS::Console::LogLevel::Error:
        case JS::Console::LogLevel::Assert:
            outln("{}\033[31;1m{}\033[0m", indent, output);
            break;
        case JS::Console::LogLevel::Info:
            outln("{}(i) {}", indent, output);
            break;
        case JS::Console::LogLevel::Log:
            outln("{}{}", indent, output);
            break;
        case JS::Console::LogLevel::Warn:
        case JS::Console::LogLevel::CountReset:
            outln("{}\033[33;1m{}\033[0m", indent, output);
            break;
        default:
            outln("{}{}", indent, output);
            break;
        }
        return JS::js_undefined();
    }

private:
    int m_group_stack_depth { 0 };
};

#if !defined(AK_OS_WINDOWS)
static ErrorOr<String> read_next_piece()
{
    StringBuilder piece;

    auto line_level_delta_for_next_line { 0 };

    do {
        auto line_result = s_editor->get_line(TRY(prompt_for_level(s_repl_line_level)).to_byte_string());

        s_ctrl_c_hit_count = 0;
        line_level_delta_for_next_line = 0;

        if (line_result.is_error()) {
            s_keep_running_repl = false;
            return String {};
        }

        auto& line = line_result.value();
        s_editor->add_to_history(line);

        piece.append(line);
        piece.append('\n');
        auto lexer = JS::Lexer(line);

        enum {
            NotInLabelOrObjectKey,
            InLabelOrObjectKeyIdentifier,
            InLabelOrObjectKey
        } label_state { NotInLabelOrObjectKey };

        for (JS::Token token = lexer.next(); token.type() != JS::TokenType::Eof; token = lexer.next()) {
            switch (token.type()) {
            case JS::TokenType::BracketOpen:
            case JS::TokenType::CurlyOpen:
            case JS::TokenType::ParenOpen:
                label_state = NotInLabelOrObjectKey;
                s_repl_line_level++;
                break;
            case JS::TokenType::BracketClose:
            case JS::TokenType::CurlyClose:
            case JS::TokenType::ParenClose:
                label_state = NotInLabelOrObjectKey;
                s_repl_line_level--;
                break;

            case JS::TokenType::Identifier:
            case JS::TokenType::StringLiteral:
                if (label_state == NotInLabelOrObjectKey)
                    label_state = InLabelOrObjectKeyIdentifier;
                else
                    label_state = NotInLabelOrObjectKey;
                break;
            case JS::TokenType::Colon:
                if (label_state == InLabelOrObjectKeyIdentifier)
                    label_state = InLabelOrObjectKey;
                else
                    label_state = NotInLabelOrObjectKey;
                break;
            default:
                break;
            }
        }

        if (label_state == InLabelOrObjectKey) {
            // If there's a label or object literal key at the end of this line,
            // prompt for more lines but do not change the line level.
            line_level_delta_for_next_line += 1;
        }
    } while (s_repl_line_level + line_level_delta_for_next_line > 0);

    return piece.to_string();
}

static ErrorOr<void> repl(JS::Realm& realm)
{
    while (s_keep_running_repl) {
        auto const piece = TRY(read_next_piece());
        if (Utf8View { piece }.trim(JS::whitespace_characters).is_empty())
            continue;

        g_repl_statements.append(piece);
        TRY(parse_and_run(realm, piece, "REPL"sv));
    }
    return {};
}

static ErrorOr<int> run_repl(bool gc_on_every_allocation, bool syntax_highlight)
{
    s_print_last_result = true;

    auto root_execution_context = JS::create_simple_execution_context<ReplObject>(*g_vm);
    auto& realm = *root_execution_context->realm;

    auto& console_object = *realm.intrinsics().console_object();
    ReplConsoleClient console_client(console_object.console());
    console_object.console().set_client(console_client);
    g_vm->heap().set_should_collect_on_every_allocation(gc_on_every_allocation);

    auto& global_environment = realm.global_environment();

    s_editor = Line::Editor::construct();
    s_editor->load_history(s_history_path.to_byte_string());

    signal(SIGINT, [](int) {
        if (!s_editor->is_editing())
            exit(0);
        s_editor->save_history(s_history_path.to_byte_string());
    });

    s_editor->register_key_input_callback(Line::ctrl('C'), [](Line::Editor& editor) -> bool {
        if (editor.buffer_view().length() == 0 || s_ctrl_c_hit_count > 0) {
            if (++s_ctrl_c_hit_count == 2) {
                s_keep_running_repl = false;
                editor.finish_edit();
                return false;
            }
        }

        return true;
    });

    s_editor->on_display_refresh = [syntax_highlight](Line::Editor& editor) {
        auto stylize = [&](Line::Span span, Line::Style styles) {
            if (syntax_highlight)
                editor.stylize(span, styles);
        };
        editor.strip_styles();

        size_t open_indents = s_repl_line_level;

        auto line = editor.line();
        JS::Lexer lexer(line);
        bool indenters_starting_line = true;
        for (JS::Token token = lexer.next(); token.type() != JS::TokenType::Eof; token = lexer.next()) {
            auto length = token.value().length_in_code_units();
            auto start = token.offset();
            auto end = start + length;
            if (indenters_starting_line) {
                if (token.type() != JS::TokenType::ParenClose && token.type() != JS::TokenType::BracketClose && token.type() != JS::TokenType::CurlyClose) {
                    indenters_starting_line = false;
                } else {
                    --open_indents;
                }
            }

            switch (token.category()) {
            case JS::TokenCategory::Invalid:
                stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Red), Line::Style::Underline });
                break;
            case JS::TokenCategory::Number:
                stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Magenta) });
                break;
            case JS::TokenCategory::String:
                stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Green), Line::Style::Bold });
                break;
            case JS::TokenCategory::Punctuation:
                break;
            case JS::TokenCategory::Operator:
                break;
            case JS::TokenCategory::Keyword:
                switch (token.type()) {
                case JS::TokenType::BoolLiteral:
                case JS::TokenType::NullLiteral:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow), Line::Style::Bold });
                    break;
                default:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Blue), Line::Style::Bold });
                    break;
                }
                break;
            case JS::TokenCategory::ControlKeyword:
                stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan), Line::Style::Italic });
                break;
            case JS::TokenCategory::Identifier:
                stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::White), Line::Style::Bold });
                break;
            default:
                break;
            }
        }

        editor.set_prompt(prompt_for_level(open_indents).release_value_but_fixme_should_propagate_errors().to_byte_string());
    };

    auto complete = [&realm, &global_environment](Line::Editor const& editor) -> Vector<Line::CompletionSuggestion> {
        auto line = editor.line(editor.cursor());

        JS::Lexer lexer { line };
        enum {
            Initial,
            CompleteVariable,
            CompleteNullProperty,
            CompleteProperty,
        } mode { Initial };

        Utf16FlyString variable_name;
        Utf16FlyString property_name;

        // we're only going to complete either
        //    - <N>
        //        where N is part of the name of a variable
        //    - <N>.<P>
        //        where N is the complete name of a variable and
        //        P is part of the name of one of its properties
        auto js_token = lexer.next();
        for (; js_token.type() != JS::TokenType::Eof; js_token = lexer.next()) {
            switch (mode) {
            case CompleteVariable:
                switch (js_token.type()) {
                case JS::TokenType::Period:
                    // ...<name> <dot>
                    mode = CompleteNullProperty;
                    break;
                default:
                    // not a dot, reset back to initial
                    mode = Initial;
                    break;
                }
                break;
            case CompleteNullProperty:
                if (js_token.is_identifier_name()) {
                    // ...<name> <dot> <name>
                    mode = CompleteProperty;
                    property_name = js_token.fly_string_value();
                } else {
                    mode = Initial;
                }
                break;
            case CompleteProperty:
                // something came after the property access, reset to initial
            case Initial:
                if (js_token.type() == JS::TokenType::Identifier) {
                    // ...<name>...
                    mode = CompleteVariable;
                    variable_name = js_token.fly_string_value();
                } else {
                    mode = Initial;
                }
                break;
            }
        }

        bool last_token_has_trivia = !js_token.trivia().is_empty();

        if (mode == CompleteNullProperty) {
            mode = CompleteProperty;
            property_name = Utf16FlyString {};
            last_token_has_trivia = false; // <name> <dot> [tab] is sensible to complete.
        }

        if (mode == Initial || last_token_has_trivia)
            return {}; // we do not know how to complete this

        Vector<Line::CompletionSuggestion> results;

        Function<void(JS::Shape const&, Utf16FlyString const&)> list_all_properties = [&results, &list_all_properties](JS::Shape const& shape, Utf16FlyString const& property_pattern) {
            for (auto const& descriptor : shape.property_table()) {
                if (!descriptor.key.is_string())
                    continue;

                auto key = descriptor.key.as_string().to_utf16_string();

                if (key.starts_with(property_pattern.view())) {
                    Line::CompletionSuggestion completion { key.to_utf8_but_should_be_ported_to_utf16(), Line::CompletionSuggestion::ForSearch };
                    if (!results.contains_slow(completion)) { // hide duplicates
                        results.append(key.to_byte_string());
                        results.last().invariant_offset = property_pattern.length_in_code_units();
                    }
                }
            }
            if (auto const* prototype = shape.prototype()) {
                list_all_properties(prototype->shape(), property_pattern);
            }
        };

        switch (mode) {
        case CompleteProperty: {
            auto reference_or_error = g_vm->resolve_binding(variable_name, JS::Strict::No, &global_environment);
            if (reference_or_error.is_error())
                return {};
            auto value_or_error = reference_or_error.value().get_value(*g_vm);
            if (value_or_error.is_error())
                return {};
            auto variable = value_or_error.value();
            VERIFY(!variable.is_special_empty_value());

            if (!variable.is_object())
                break;

            auto const object = MUST(variable.to_object(*g_vm));
            auto const& shape = object->shape();
            list_all_properties(shape, property_name);
            break;
        }
        case CompleteVariable: {
            auto const& variable = realm.global_object();
            list_all_properties(variable.shape(), variable_name);

            for (auto const& name : global_environment.declarative_record().bindings()) {
                if (name.view().starts_with(variable_name.view())) {
                    results.empend(MUST(name.view().to_byte_string()));
                    results.last().invariant_offset = variable_name.length_in_code_units();
                }
            }

            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }

        return results;
    };
    s_editor->on_tab_complete = move(complete);
    TRY(repl(realm));
    s_editor->save_history(s_history_path.to_byte_string());
    return s_exit_code;
}

#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    bool gc_on_every_allocation = false;
    bool disable_syntax_highlight = false;
    bool disable_debug_printing = false;
    bool use_test262_global = false;
    StringView evaluate_script;
    Vector<StringView> script_paths;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("This is a JavaScript interpreter.");
    args_parser.add_option(s_dump_ast, "Dump the AST", "dump-ast", 'A');
    args_parser.add_option(JS::Bytecode::g_dump_bytecode, "Dump the bytecode", "dump-bytecode", 'd');
    args_parser.add_option(s_as_module, "Treat as module", "as-module", 'm');
    args_parser.add_option(s_print_last_result, "Print last result", "print-last-result", 'l');
    args_parser.add_option(s_strip_ansi, "Disable ANSI colors", "disable-ansi-colors", 'i');
    args_parser.add_option(s_disable_source_location_hints, "Disable source location hints", "disable-source-location-hints", 'h');
    args_parser.add_option(gc_on_every_allocation, "GC on every allocation", "gc-on-every-allocation", 'g');
    args_parser.add_option(s_raw_strings, "Display strings without quotes or escape sequences", "raw-strings", 'r');
    args_parser.add_option(disable_syntax_highlight, "Disable live syntax highlighting", "no-syntax-highlight", 's');
    args_parser.add_option(disable_debug_printing, "Disable debug output", "disable-debug-output", {});
    args_parser.add_option(evaluate_script, "Evaluate argument as a script", "evaluate", 'c', "script");
    args_parser.add_option(use_test262_global, "Use test262 global ($262)", "use-test262-global", {});
    args_parser.add_positional_argument(script_paths, "Path to script files", "scripts", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    [[maybe_unused]] bool syntax_highlight = !disable_syntax_highlight;

    AK::set_debug_enabled(!disable_debug_printing);
    s_history_path = TRY(String::formatted("{}/.js-history", Core::StandardPaths::home_directory()));

    g_vm_storage.get() = JS::VM::create();
    g_vm = g_vm_storage->ptr();
    g_vm->set_dynamic_imports_allowed(true);

    if (!disable_debug_printing) {
        // NOTE: These will print out both warnings when using something like Promise.reject().catch(...) -
        // which is, as far as I can tell, correct - a promise is created, rejected without handler, and a
        // handler then attached to it. The Node.js REPL doesn't warn in this case, so it's something we
        // might want to revisit at a later point and disable warnings for promises created this way.
        g_vm->on_promise_unhandled_rejection = [](auto& promise) {
            warn("WARNING: A promise was rejected without any handlers");
            warn(" (result: ");
            (void)print(promise.result(), PrintTarget::StandardError, PrintEnd::None);
            warnln(")");
        };
        g_vm->on_promise_rejection_handled = [](auto& promise) {
            warn("WARNING: A handler was added to an already rejected promise");
            warn(" (result: ");
            (void)print(promise.result(), PrintTarget::StandardError, PrintEnd::None);
            warnln(")");
        };
    }

    // FIXME: Figure out some way to interrupt the interpreter now that vm.exception() is gone.

    if (evaluate_script.is_empty() && script_paths.is_empty()) {
#if defined(AK_OS_WINDOWS)
        dbgln("REPL functionality is not supported on Windows");
        VERIFY_NOT_REACHED();
#else
        return run_repl(gc_on_every_allocation, syntax_highlight);
#endif
    } else {
        OwnPtr<JS::ExecutionContext> root_execution_context;
        if (use_test262_global)
            root_execution_context = JS::create_simple_execution_context<JS::Test262::GlobalObject>(*g_vm);
        else
            root_execution_context = JS::create_simple_execution_context<ScriptObject>(*g_vm);

        auto& realm = *root_execution_context->realm;
        auto& console_object = *realm.intrinsics().console_object();
        ReplConsoleClient console_client(console_object.console());
        console_object.console().set_client(console_client);
        g_vm->heap().set_should_collect_on_every_allocation(gc_on_every_allocation);

        StringBuilder builder;
        StringView source_name;

        if (evaluate_script.is_empty()) {
            if (script_paths.size() > 1)
                warnln("Warning: Multiple files supplied, this will concatenate the sources and resolve modules as if it was the first file");

            for (auto& path : script_paths) {
                auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
                auto file_contents = TRY(file->read_until_eof());
                auto source = StringView { file_contents };

                if (Utf8View { file_contents }.validate()) {
                    builder.append(source);
                } else {
                    auto decoder = TextCodec::decoder_for("windows-1252"sv);
                    VERIFY(decoder.has_value());

                    auto utf8_source = TRY(TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, source));
                    builder.append(utf8_source);
                }
            }

            source_name = script_paths[0];
        } else {
            builder.append(evaluate_script);
            source_name = "eval"sv;
        }

        // We resolve modules as if it is the first file

        if (!TRY(parse_and_run(realm, builder.string_view(), source_name)))
            return 1;
    }

    return s_exit_code;
}
