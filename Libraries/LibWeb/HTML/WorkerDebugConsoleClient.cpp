/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/WorkerDebugConsoleClient.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerDebugConsoleClient);

WorkerDebugConsoleClient::WorkerDebugConsoleClient(JS::Console& console)
    : ConsoleClient(console)
{
}

// The terminal isn't the page's to clear, so this only ends the groups.
void WorkerDebugConsoleClient::clear()
{
    m_group_stack_depth = 0;
}

void WorkerDebugConsoleClient::end_group()
{
    if (m_group_stack_depth > 0)
        m_group_stack_depth--;
}

// 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
JS::ThrowCompletionOr<JS::Value> WorkerDebugConsoleClient::printer(JS::Console::LogLevel log_level, PrinterArguments arguments)
{
    auto& vm = m_console->realm().vm();

    auto indent = TRY_OR_THROW_OOM(vm, String::repeated(' ', m_group_stack_depth * 2));

    if (log_level == JS::Console::LogLevel::Trace) {
        auto trace = arguments.get<JS::Console::Trace>();
        StringBuilder builder;
        if (!trace.label.is_empty())
            builder.appendff("{}\033[36;1m{}\033[0m\n", indent, JS::Console::escape_for_terminal(trace.label.utf16_view()));

        for (auto& frame : trace.stack)
            builder.appendff("{}-> {}\n", indent, JS::Console::escape_for_terminal(frame.function_name.utf16_view()));

        dbgln("{}", builder.string_view());
        return JS::js_undefined();
    }

    if (log_level == JS::Console::LogLevel::Group || log_level == JS::Console::LogLevel::GroupCollapsed) {
        auto group = arguments.get<JS::Console::Group>();
        dbgln("{}\033[36;1m{}\033[0m", indent, JS::Console::escape_for_terminal(group.label.utf16_view()));
        m_group_stack_depth++;
        return JS::js_undefined();
    }

    auto output = TRY(generically_format_values(arguments.get<GC::RootVector<JS::Value>>()));
    m_console->output_debug_message(log_level, output);
    return JS::js_undefined();
}

} // namespace Web::HTML
