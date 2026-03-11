/*
 * Copyright (c) 2020-2026, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceRange.h>

namespace JS {

using ScriptOrModule = Variant<Empty, GC::Ref<Script>, GC::Ref<Module>>;

// 9.4 Execution Contexts, https://tc39.es/ecma262/#sec-execution-contexts
struct JS_API ExecutionContext {
    static NonnullOwnPtr<ExecutionContext> create(u32 registers_and_locals_count, u32 constants_count, u32 arguments_count);
    [[nodiscard]] NonnullOwnPtr<ExecutionContext> copy() const;

    ~ExecutionContext() = default;

    void visit_edges(Cell::Visitor&);

private:
    friend class ExecutionContextAllocator;

public:
    // NB: The layout is: [registers | locals | constants | arguments]
    //     We only initialize registers and locals to empty, since constants are copied in right after.
    ALWAYS_INLINE ExecutionContext(u32 registers_and_locals_count, u32 constants_count, u32 arguments_count_)
    {
        VERIFY(!Checked<u32>::addition_would_overflow(registers_and_locals_count, constants_count, arguments_count_));
        registers_and_constants_and_locals_and_arguments_count = registers_and_locals_count + constants_count + arguments_count_;
        argument_count = arguments_count_;
        auto* values = registers_and_constants_and_locals_and_arguments();
        for (size_t i = 0; i < registers_and_locals_count; ++i)
            values[i] = js_special_empty_value();
    }

    void operator delete(void* ptr);

    GC::Ptr<FunctionObject> function;                // [[Function]]
    GC::Ptr<Realm> realm;                            // [[Realm]]
    ScriptOrModule script_or_module;                 // [[ScriptOrModule]]
    GC::Ptr<Environment> lexical_environment;        // [[LexicalEnvironment]]
    GC::Ptr<Environment> variable_environment;       // [[VariableEnvironment]]
    GC::Ptr<PrivateEnvironment> private_environment; // [[PrivateEnvironment]]

    u32 program_counter { 0 };

    // https://html.spec.whatwg.org/multipage/webappapis.html#skip-when-determining-incumbent-counter
    // FIXME: Move this out of LibJS (e.g. by using the CustomData concept), as it's used exclusively by LibWeb.
    u32 skip_when_determining_incumbent_counter { 0 };

    Optional<Value> this_value;

    GC::Ptr<Bytecode::Executable> executable;

    Span<Value> registers_and_constants_and_locals_and_arguments_span()
    {
        return { registers_and_constants_and_locals_and_arguments(), registers_and_constants_and_locals_and_arguments_count };
    }

    Value const* registers_and_constants_and_locals_and_arguments() const
    {
        return reinterpret_cast<Value*>(reinterpret_cast<uintptr_t>(this) + sizeof(ExecutionContext));
    }

    Value argument(size_t index) const
    {
        if (index >= argument_count) [[unlikely]]
            return js_undefined();
        return arguments_data()[index];
    }

    Span<Value> arguments_span()
    {
        return { arguments_data(), argument_count };
    }

    ReadonlySpan<Value> arguments_span() const
    {
        return { arguments_data(), argument_count };
    }

    Value* arguments_data()
    {
        return registers_and_constants_and_locals_and_arguments() + (registers_and_constants_and_locals_and_arguments_count - argument_count);
    }

    Value const* arguments_data() const
    {
        return registers_and_constants_and_locals_and_arguments() + (registers_and_constants_and_locals_and_arguments_count - argument_count);
    }

    // Non-standard: Inline frame linkage for the bytecode interpreter.
    // When a JS-to-JS call is inlined in the dispatch loop, these fields
    // allow the Return handler to restore the caller's frame.
    ExecutionContext* caller_frame { nullptr };
    u32 passed_argument_count { 0 };
    u32 caller_return_pc { 0 };
    u32 caller_dst_raw { 0 };
    bool caller_is_construct { false };

private:
    friend class Bytecode::Interpreter;

    Value* registers_and_constants_and_locals_and_arguments()
    {
        return reinterpret_cast<Value*>(reinterpret_cast<uintptr_t>(this) + sizeof(ExecutionContext));
    }

    u32 registers_and_constants_and_locals_and_arguments_count { 0 };

public:
    u32 argument_count { 0 };
};

static_assert(IsTriviallyDestructible<ExecutionContext>);

struct StackTraceElement {
    ExecutionContext* execution_context { nullptr };
    Optional<SourceRange> source_range;
};

}
