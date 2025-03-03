/*
 * Copyright (c) 2021, Brandon Scott <xeon.productions@gmail.com>
 * Copyright (c) 2020, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <WebContent/Forward.h>
#include <WebContent/WebContentConsoleClient.h>

namespace WebContent {

class InspectorConsoleClient final : public WebContentConsoleClient {
    GC_CELL(InspectorConsoleClient, WebContentConsoleClient);
    GC_DECLARE_ALLOCATOR(InspectorConsoleClient);

public:
    static GC::Ref<InspectorConsoleClient> create(JS::Realm&, JS::Console&, PageClient&);
    virtual ~InspectorConsoleClient() override;

private:
    InspectorConsoleClient(JS::Realm&, JS::Console&, PageClient&, ConsoleGlobalEnvironmentExtensions&);

    virtual void handle_result(JS::Value) override;
    virtual void report_exception(JS::Error const&, bool) override;

    void begin_group(String const& label, bool start_expanded);
    virtual void end_group() override;
    virtual void clear() override;

    void print_html(String const& line);

    virtual void send_messages(i32 start_index) override;
    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel log_level, PrinterArguments) override;

    virtual void add_css_style_to_current_message(StringView style) override
    {
        m_current_message_style.append(style);
        m_current_message_style.append(';');
    }

    struct ConsoleOutput {
        enum class Type {
            HTML,
            Clear,
            BeginGroup,
            BeginGroupCollapsed,
            EndGroup,
        };
        Type type;
        String data;
    };

    Vector<ConsoleOutput> m_message_log;
    StringBuilder m_current_message_style;
};

}
