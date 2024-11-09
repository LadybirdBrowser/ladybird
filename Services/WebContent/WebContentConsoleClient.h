/*
 * Copyright (c) 2021, Brandon Scott <xeon.productions@gmail.com>
 * Copyright (c) 2020, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibJS/Console.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <WebContent/ConsoleGlobalEnvironmentExtensions.h>
#include <WebContent/Forward.h>

namespace WebContent {

class WebContentConsoleClient final : public JS::ConsoleClient {
    JS_CELL(WebContentConsoleClient, JS::ConsoleClient);
    JS_DECLARE_ALLOCATOR(WebContentConsoleClient);

public:
    virtual ~WebContentConsoleClient() override;

    void handle_input(ByteString const& js_source);
    void send_messages(i32 start_index);
    void report_exception(JS::Error const&, bool) override;

private:
    WebContentConsoleClient(JS::Console&, JS::Realm&, PageClient&);

    virtual void visit_edges(JS::Cell::Visitor&) override;
    virtual void clear() override;
    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel log_level, PrinterArguments) override;

    virtual void add_css_style_to_current_message(StringView style) override
    {
        m_current_message_style.append(style);
        m_current_message_style.append(';');
    }

    JS::NonnullGCPtr<PageClient> m_client;
    JS::GCPtr<ConsoleGlobalEnvironmentExtensions> m_console_global_environment_extensions;

    void clear_output();
    void print_html(ByteString const& line);
    void begin_group(ByteString const& label, bool start_expanded);
    virtual void end_group() override;

    struct ConsoleOutput {
        enum class Type {
            HTML,
            Clear,
            BeginGroup,
            BeginGroupCollapsed,
            EndGroup,
        };
        Type type;
        ByteString data;
    };
    Vector<ConsoleOutput> m_message_log;

    StringBuilder m_current_message_style;
};

}
