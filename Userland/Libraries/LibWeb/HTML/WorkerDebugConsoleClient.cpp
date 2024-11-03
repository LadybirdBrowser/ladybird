/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Heap/MarkedVector.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/WorkerDebugConsoleClient.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(WorkerDebugConsoleClient);

WorkerDebugConsoleClient::WorkerDebugConsoleClient(JS::Console& console)
    : ConsoleClient(console)
{
}

void WorkerDebugConsoleClient::clear()
{
    dbgln("\033[3J\033[H\033[2J");
    m_group_stack_depth = 0;
    fflush(stdout);
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
            builder.appendff("{}\033[36;1m{}\033[0m\n", indent, trace.label);

        for (auto& function_name : trace.stack)
            builder.appendff("{}-> {}\n", indent, function_name);

        dbgln("{}", builder.string_view());
        return JS::js_undefined();
    }

    if (log_level == JS::Console::LogLevel::Group || log_level == JS::Console::LogLevel::GroupCollapsed) {
        auto group = arguments.get<JS::Console::Group>();
        dbgln("{}\033[36;1m{}\033[0m", indent, group.label);
        m_group_stack_depth++;
        return JS::js_undefined();
    }

    auto output = TRY(generically_format_values(arguments.get<JS::MarkedVector<JS::Value>>()));
    m_console->output_debug_message(log_level, output);
    return JS::js_undefined();
}

} // namespace Web::HTML
