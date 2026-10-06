/*
 * Copyright (c) 2020, Emanuele Torre <torreemanuele6@gmail.com>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC_DEFINE_ALLOCATOR(ConsoleClient);

#define JS_ASSERT_CONSOLE_LOG_LEVEL(name, abi_name) \
    static_assert(to_underlying(Console::LogLevel::name) == JS_CONSOLE_LOG_LEVEL_##abi_name);
JS_ASSERT_CONSOLE_LOG_LEVEL(Assert, ASSERT)
JS_ASSERT_CONSOLE_LOG_LEVEL(Count, COUNT)
JS_ASSERT_CONSOLE_LOG_LEVEL(CountReset, COUNT_RESET)
JS_ASSERT_CONSOLE_LOG_LEVEL(Debug, DEBUG)
JS_ASSERT_CONSOLE_LOG_LEVEL(Dir, DIR)
JS_ASSERT_CONSOLE_LOG_LEVEL(DirXML, DIR_XML)
JS_ASSERT_CONSOLE_LOG_LEVEL(Error, ERROR)
JS_ASSERT_CONSOLE_LOG_LEVEL(Group, GROUP)
JS_ASSERT_CONSOLE_LOG_LEVEL(GroupCollapsed, GROUP_COLLAPSED)
JS_ASSERT_CONSOLE_LOG_LEVEL(Info, INFO)
JS_ASSERT_CONSOLE_LOG_LEVEL(Log, LOG)
JS_ASSERT_CONSOLE_LOG_LEVEL(TimeEnd, TIME_END)
JS_ASSERT_CONSOLE_LOG_LEVEL(TimeLog, TIME_LOG)
JS_ASSERT_CONSOLE_LOG_LEVEL(Table, TABLE)
JS_ASSERT_CONSOLE_LOG_LEVEL(Trace, TRACE)
JS_ASSERT_CONSOLE_LOG_LEVEL(Warn, WARN)
#undef JS_ASSERT_CONSOLE_LOG_LEVEL

static JSConsole* console_to_abi(Console const& console)
{
    return cell_to_abi<JSConsole>(console);
}

static JSConsoleClient* engine_console_client_to_abi(EngineConsoleClient& client)
{
    return cell_to_abi<JSConsoleClient>(client);
}

// A JS::ErrorData is the runtime's error data itself, which lives in the cell that has it.
static JSErrorData const* error_data_to_abi(ErrorData const& error_data)
{
    return reinterpret_cast<JSErrorData const*>(&error_data);
}

static ErrorData const& error_data_from_abi(JSErrorData const* error_data)
{
    VERIFY(error_data);
    return *reinterpret_cast<ErrorData const*>(error_data);
}

static ReadonlySpan<Value> values_from_abi(JSValue const* values, size_t count)
{
    if (count == 0)
        return {};
    return { reinterpret_cast<Value const*>(values), count };
}

static Console::Trace trace_from_abi(JSConsolePrinterArguments const& arguments)
{
    Console::Trace trace { .label = Utf16String::from_utf16(utf16_view_from_abi(arguments.label)), .stack = {} };
    trace.stack.ensure_capacity(arguments.trace_frame_count);
    for (size_t index = 0; index < arguments.trace_frame_count; ++index) {
        auto const& frame = arguments.trace_frames[index];
        Console::TraceFrame trace_frame {
            .function_name = Utf16String::from_utf16(utf16_view_from_abi(frame.function_name)),
            .source_file = {},
            .line = {},
            .column = {},
        };
        if (frame.has_source_file)
            trace_frame.source_file = Utf16String::from_utf16(utf16_view_from_abi(frame.source_file));
        if (frame.has_line)
            trace_frame.line = frame.line;
        if (frame.has_column)
            trace_frame.column = frame.column;
        trace.stack.unchecked_append(move(trace_frame));
    }
    return trace;
}

static ConsoleClient::PrinterArguments printer_arguments_from_abi(JSConsolePrinterArguments const& arguments)
{
    switch (arguments.kind) {
    case JS_CONSOLE_PRINTER_ARGUMENTS_GROUP:
        return Console::Group { .label = Utf16String::from_utf16(utf16_view_from_abi(arguments.label)) };
    case JS_CONSOLE_PRINTER_ARGUMENTS_TRACE:
        return trace_from_abi(arguments);
    case JS_CONSOLE_PRINTER_ARGUMENTS_VALUES:
        return GC::RootVector<Value> { values_from_abi(arguments.values, arguments.value_count) };
    default:
        VERIFY_NOT_REACHED();
    }
}

namespace {

// The runtime calls the virtual functions of an embedder's console client through these, each with the client as its
// context.
struct ConsoleClientMethodThunks {
    static ConsoleClient& client_of(void* context)
    {
        return static_cast<ConsoleClient&>(*static_cast<GC::Cell*>(context));
    }

    static JSCompletion printer(void* context, JSVM*, u8 log_level, JSConsolePrinterArguments const* arguments)
    {
        VERIFY(arguments);
        return completion_to_abi(client_of(context).printer(static_cast<Console::LogLevel>(log_level), printer_arguments_from_abi(*arguments)));
    }

    static void add_css_style_to_current_message(void* context, JSUtf16View style)
    {
        client_of(context).add_css_style_to_current_message(utf16_view_from_abi(style));
    }

    static void report_exception(void* context, JSUtf16View name, JSUtf16View message, JSErrorData const* error_data, bool in_promise)
    {
        client_of(context).report_exception(utf16_view_from_abi(name), utf16_view_from_abi(message), error_data_from_abi(error_data), in_promise);
    }

    static void clear(void* context)
    {
        client_of(context).clear();
    }

    static void end_group(void* context)
    {
        client_of(context).end_group();
    }

    static constexpr JSConsoleClientMethods methods {
        .printer = printer,
        .add_css_style_to_current_message = add_css_style_to_current_message,
        .report_exception = report_exception,
        .clear = clear,
        .end_group = end_group,
    };
};

}

void Console::set_client(ConsoleClient& client)
{
    js_console_set_client(console_to_abi(*this), engine_console_client_to_abi(*client.m_engine_console_client));
}

Realm& Console::realm() const
{
    return cell_ref_from_abi<Realm>(js_console_realm(console_to_abi(*this)));
}

void Console::output_debug_message(LogLevel log_level, StringView output) const
{
    switch (log_level) {
    case Console::LogLevel::Debug:
        dbgln("\033[32;1m(js debug)\033[0m {}", output);
        break;
    case Console::LogLevel::Error:
        dbgln("\033[32;1m(js error)\033[0m {}", output);
        break;
    case Console::LogLevel::Info:
        dbgln("\033[32;1m(js info)\033[0m {}", output);
        break;
    case Console::LogLevel::Log:
        dbgln("\033[32;1m(js log)\033[0m {}", output);
        break;
    case Console::LogLevel::Warn:
        dbgln("\033[32;1m(js warn)\033[0m {}", output);
        break;
    default:
        dbgln("\033[32;1m(js)\033[0m {}", output);
        break;
    }
}

void Console::output_debug_message(LogLevel log_level, Utf16View output) const
{
    switch (log_level) {
    case Console::LogLevel::Debug:
        dbgln("\033[32;1m(js debug)\033[0m {}", output);
        break;
    case Console::LogLevel::Error:
        dbgln("\033[32;1m(js error)\033[0m {}", output);
        break;
    case Console::LogLevel::Info:
        dbgln("\033[32;1m(js info)\033[0m {}", output);
        break;
    case Console::LogLevel::Log:
        dbgln("\033[32;1m(js log)\033[0m {}", output);
        break;
    case Console::LogLevel::Warn:
        dbgln("\033[32;1m(js warn)\033[0m {}", output);
        break;
    default:
        dbgln("\033[32;1m(js)\033[0m {}", output);
        break;
    }
}

void Console::report_exception(Utf16View name, Utf16View message, ErrorData const& error_data, bool in_promise) const
{
    js_console_report_exception(console_to_abi(*this), utf16_view_to_abi(name), utf16_view_to_abi(message), error_data_to_abi(error_data), in_promise);
}

static GC::Ref<EngineConsoleClient> create_engine_console_client(Console& console, ConsoleClient& client)
{
    auto* engine_console_client = js_console_client_create(vm_to_abi(console.vm()), console_to_abi(console), &ConsoleClientMethodThunks::methods, static_cast<GC::Cell*>(&client));
    return cell_ref_from_abi<EngineConsoleClient>(engine_console_client);
}

// LibGC defers garbage collection while a cell is being constructed, so the runtime's client can take this client as
// its context before this client is complete.
ConsoleClient::ConsoleClient(Console& console)
    : m_console(console)
    , m_engine_console_client(create_engine_console_client(console, *this))
{
}

ConsoleClient::~ConsoleClient() = default;

void ConsoleClient::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_console);
    visitor.visit(m_engine_console_client);
}

ThrowCompletionOr<Utf16String> ConsoleClient::generically_format_values(GC::RootVector<Value> const& values)
{
    static_assert(sizeof(Value) == sizeof(JSValue));
    JSOwnedUtf16String formatted {};
    auto completion = js_console_client_generically_format_values(vm_to_abi(vm()), engine_console_client_to_abi(*m_engine_console_client), reinterpret_cast<JSValue const*>(values.data()), values.size(), &formatted);
    TRY(completion_from_abi<void>(completion));
    return owned_utf16_string_from_abi(formatted);
}

}
