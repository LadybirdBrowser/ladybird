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
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class ConsoleClient;

// https://console.spec.whatwg.org
class JS_API Console : public Cell {
    GC_CELL(Console, Cell);
    GC_DECLARE_ALLOCATOR(Console);

public:
    virtual ~Console() override;

    // These are not really levels, but that's the term used in the spec.
    enum class LogLevel {
        Assert,
        Count,
        CountReset,
        Debug,
        Dir,
        DirXML,
        Error,
        Group,
        GroupCollapsed,
        Info,
        Log,
        TimeEnd,
        TimeLog,
        Table,
        Trace,
        Warn,
    };

    struct Group {
        String label;
    };

    struct TraceFrame {
        String function_name;
        Optional<String> source_file;
        Optional<size_t> line;
        Optional<size_t> column;
    };

    struct Trace {
        String label;
        Vector<TraceFrame> stack;
    };

    void set_client(ConsoleClient& client) { m_client = &client; }

    Realm& realm() const { return m_realm; }

    GC::RootVector<Value> vm_arguments();

    HashMap<String, unsigned>& counters() { return m_counters; }
    HashMap<String, unsigned> const& counters() const { return m_counters; }

    ThrowCompletionOr<Value> assert_();
    Value clear();
    ThrowCompletionOr<Value> debug();
    ThrowCompletionOr<Value> error();
    ThrowCompletionOr<Value> info();
    ThrowCompletionOr<Value> log();
    ThrowCompletionOr<Value> table();
    ThrowCompletionOr<Value> trace();
    ThrowCompletionOr<Value> warn();
    ThrowCompletionOr<Value> dir();
    ThrowCompletionOr<Value> dirxml();
    ThrowCompletionOr<Value> count();
    ThrowCompletionOr<Value> count_reset();
    ThrowCompletionOr<Value> group();
    ThrowCompletionOr<Value> group_collapsed();
    ThrowCompletionOr<Value> group_end();
    ThrowCompletionOr<Value> time();
    ThrowCompletionOr<Value> time_log();
    ThrowCompletionOr<Value> time_end();

    void output_debug_message(LogLevel log_level, StringView output) const;
    void report_exception(JS::Error const&, bool) const;

private:
    explicit Console(Realm&);

    virtual void visit_edges(Visitor&) override;

    ThrowCompletionOr<String> value_vector_to_string(GC::RootVector<Value> const&);

    GC::Ref<Realm> m_realm;
    GC::Ptr<ConsoleClient> m_client;

    HashMap<String, unsigned> m_counters;
    HashMap<String, Core::ElapsedTimer> m_timer_table;
    Vector<Group> m_group_stack;
};

class JS_API ConsoleClient : public Cell {
    GC_CELL(ConsoleClient, Cell);
    GC_DECLARE_ALLOCATOR(ConsoleClient);

public:
    using PrinterArguments = Variant<Console::Group, Console::Trace, GC::RootVector<Value>>;

    ThrowCompletionOr<Value> logger(Console::LogLevel log_level, GC::RootVector<Value> const& args);
    ThrowCompletionOr<GC::RootVector<Value>> formatter(GC::RootVector<Value> const& args);
    virtual ThrowCompletionOr<Value> printer(Console::LogLevel log_level, PrinterArguments) = 0;

    virtual void add_css_style_to_current_message(StringView) { }
    virtual void report_exception(JS::Error const&, bool) { }

    virtual void clear() = 0;
    virtual void end_group() = 0;

    ThrowCompletionOr<String> generically_format_values(GC::RootVector<Value> const&);

protected:
    explicit ConsoleClient(Console&);
    virtual ~ConsoleClient() override;
    virtual void visit_edges(Visitor& visitor) override;

    GC::Ref<Console> m_console;
};

}
