/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceRange.h>

namespace JS {

using ScriptOrModule = Variant<Empty, GC::Ref<Script>, GC::Ref<Module>>;

struct CachedSourceRange : public RefCounted<CachedSourceRange> {
    CachedSourceRange(size_t program_counter, Variant<UnrealizedSourceRange, SourceRange> source_range)
        : program_counter(program_counter)
        , source_range(move(source_range))
    {
    }
    size_t program_counter { 0 };
    Variant<UnrealizedSourceRange, SourceRange> source_range;
};

// 9.4 Execution Contexts, https://tc39.es/ecma262/#sec-execution-contexts
struct JS_API ExecutionContext {
    static NonnullOwnPtr<ExecutionContext> create(u32 registers_and_constants_and_locals_count, u32 arguments_count);
    [[nodiscard]] NonnullOwnPtr<ExecutionContext> copy() const;

    ~ExecutionContext();

    void visit_edges(Cell::Visitor&);

private:
    friend class ExecutionContextAllocator;

public:
    ExecutionContext(u32 registers_and_constants_and_locals_count, u32 arguments_count);

    void operator delete(void* ptr);

    GC::Ptr<FunctionObject> function;                // [[Function]]
    GC::Ptr<Realm> realm;                            // [[Realm]]
    ScriptOrModule script_or_module;                 // [[ScriptOrModule]]
    GC::Ptr<Environment> lexical_environment;        // [[LexicalEnvironment]]
    GC::Ptr<Environment> variable_environment;       // [[VariableEnvironment]]
    GC::Ptr<PrivateEnvironment> private_environment; // [[PrivateEnvironment]]

    Optional<size_t> scheduled_jump;
    GC::Ptr<Object> global_object;
    GC::Ptr<DeclarativeEnvironment> global_declarative_environment;
    Span<Value> registers_and_constants_and_locals_arguments;
    ReadonlySpan<Utf16FlyString> identifier_table;

    // Non-standard: This points at something that owns this ExecutionContext, in case it needs to be protected from GC.
    GC::Ptr<Cell> context_owner;

    u32 program_counter { 0 };

    // https://html.spec.whatwg.org/multipage/webappapis.html#skip-when-determining-incumbent-counter
    // FIXME: Move this out of LibJS (e.g. by using the CustomData concept), as it's used exclusively by LibWeb.
    u32 skip_when_determining_incumbent_counter { 0 };

    mutable RefPtr<CachedSourceRange> cached_source_range;

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
        if (index >= arguments.size()) [[unlikely]]
            return js_undefined();
        return arguments[index];
    }

    Value& local(size_t index)
    {
        return registers_and_constants_and_locals_and_arguments()[index];
    }

    Span<Value> arguments;

    Vector<Bytecode::UnwindInfo> unwind_contexts;
    Vector<Optional<size_t>> previously_scheduled_jumps;
    Vector<GC::Ptr<Environment>> saved_lexical_environments;

    u32 passed_argument_count { 0 };

private:
    friend class Bytecode::Interpreter;

    Value* registers_and_constants_and_locals_and_arguments()
    {
        return reinterpret_cast<Value*>(reinterpret_cast<uintptr_t>(this) + sizeof(ExecutionContext));
    }

    u32 registers_and_constants_and_locals_and_arguments_count { 0 };
};

#define ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(execution_context,  \
    registers_and_constants_and_locals_count,                                                \
    arguments_count)                                                                         \
    auto execution_context_size = sizeof(JS::ExecutionContext)                               \
        + (((registers_and_constants_and_locals_count) + (arguments_count))                  \
            * sizeof(JS::Value));                                                            \
                                                                                             \
    void* execution_context_memory = alloca(execution_context_size);                         \
                                                                                             \
    execution_context = new (execution_context_memory)                                       \
        JS::ExecutionContext((registers_and_constants_and_locals_count), (arguments_count)); \
                                                                                             \
    ScopeGuard run_execution_context_destructor([execution_context] {                        \
        execution_context->~ExecutionContext();                                              \
    })

#define ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(execution_context, registers_and_constants_and_locals_count, \
    arguments_count)                                                                                            \
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(execution_context,                         \
        registers_and_constants_and_locals_count, arguments_count);                                             \
    do {                                                                                                        \
        for (size_t i = 0; i < execution_context->arguments.size(); i++) {                                      \
            execution_context->arguments[i] = JS::js_undefined();                                               \
        }                                                                                                       \
    } while (0)

struct StackTraceElement {
    ExecutionContext* execution_context;
    RefPtr<CachedSourceRange> source_range;
};

}
