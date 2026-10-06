/*
 * Copyright (c) 2020, Emanuele Torre <torreemanuele6@gmail.com>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/RootVector.h>
#include <LibJS/ConsoleLogLevel.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class ConsoleClient;

// https://console.spec.whatwg.org
// The console of a realm's console object, which keeps the counters, timers and group stack of the console methods, and
// hands what they log to its client.
class JS_API Console : public EngineCell {
public:
    using LogLevel = ConsoleLogLevel;

    struct Group {
        Utf16String label;
    };

    struct TraceFrame {
        Utf16String function_name;
        Optional<Utf16String> source_file;
        Optional<size_t> line;
        Optional<size_t> column;
    };

    struct Trace {
        Utf16String label;
        Vector<TraceFrame> stack;
    };

    // The console keeps its client alive.
    void set_client(ConsoleClient&);

    Realm& realm() const;

    void output_debug_message(LogLevel log_level, StringView output) const;
    void output_debug_message(LogLevel log_level, Utf16View output) const;
    void report_exception(Utf16View name, Utf16View message, JS::ErrorData const&, bool) const;
};

// The cell of the Rust runtime that a console holds as its client. It forwards what the console logs to the virtual
// functions of the ConsoleClient it was created for, which it keeps alive.
class EngineConsoleClient final : public EngineCell {
};

class JS_API ConsoleClient : public Cell {
    GC_CELL(ConsoleClient, Cell);
    GC_DECLARE_ALLOCATOR(ConsoleClient);

public:
    using PrinterArguments = Variant<Console::Group, Console::Trace, GC::RootVector<Value>>;

    virtual ThrowCompletionOr<Value> printer(Console::LogLevel log_level, PrinterArguments) = 0;

    virtual void add_css_style_to_current_message(Utf16View) { }
    virtual void report_exception(Utf16View, Utf16View, JS::ErrorData const&, bool) { }

    virtual void clear() = 0;
    virtual void end_group() = 0;

    ThrowCompletionOr<Utf16String> generically_format_values(GC::RootVector<Value> const&);

protected:
    explicit ConsoleClient(Console&);
    virtual ~ConsoleClient() override;
    virtual void visit_edges(Visitor& visitor) override;

    GC::Ref<Console> m_console;

private:
    friend class Console;

    GC::Ref<EngineConsoleClient> m_engine_console_client;
};

}
