/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibJS/Bytecode/BasicBlock.h>
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
struct ExecutionContext {
    static NonnullOwnPtr<ExecutionContext> create(u32 registers_and_constants_and_locals_count, u32 arguments_count);
    [[nodiscard]] NonnullOwnPtr<ExecutionContext> copy() const;

    ~ExecutionContext();

    void visit_edges(Cell::Visitor&);

private:
    friend class ExecutionContextAllocator;

    ExecutionContext();

public:
    void operator delete(void* ptr);

    GC::Ptr<FunctionObject> function;                // [[Function]]
    GC::Ptr<Realm> realm;                            // [[Realm]]
    ScriptOrModule script_or_module;                 // [[ScriptOrModule]]
    GC::Ptr<Environment> lexical_environment;        // [[LexicalEnvironment]]
    GC::Ptr<Environment> variable_environment;       // [[VariableEnvironment]]
    GC::Ptr<PrivateEnvironment> private_environment; // [[PrivateEnvironment]]

    // Non-standard: This points at something that owns this ExecutionContext, in case it needs to be protected from GC.
    GC::Ptr<Cell> context_owner;

    Optional<size_t> program_counter;

    mutable RefPtr<CachedSourceRange> cached_source_range;

    GC::Ptr<PrimitiveString> function_name;
    Optional<Value> this_value;

    GC::Ptr<Bytecode::Executable> executable;

    // https://html.spec.whatwg.org/multipage/webappapis.html#skip-when-determining-incumbent-counter
    // FIXME: Move this out of LibJS (e.g. by using the CustomData concept), as it's used exclusively by LibWeb.
    size_t skip_when_determining_incumbent_counter { 0 };

    Value argument(size_t index) const
    {
        auto arguments_size = registers_and_constants_and_locals_and_arguments.size() - arguments_offset;
        if (index >= arguments_size) [[unlikely]]
            return js_undefined();
        return registers_and_constants_and_locals_and_arguments[arguments_offset + index];
    }

    Value& local(size_t index)
    {
        return registers_and_constants_and_locals_and_arguments[index];
    }

    u32 arguments_offset { 0 };
    u32 passed_argument_count { 0 };
    bool is_strict_mode { false };

    Span<Value> arguments()
    {
        return registers_and_constants_and_locals_and_arguments.span().slice(arguments_offset);
    }

    ReadonlySpan<Value> arguments() const
    {
        return registers_and_constants_and_locals_and_arguments.span().slice(arguments_offset);
    }

private:
    friend class Bytecode::Interpreter;
    Vector<Value> registers_and_constants_and_locals_and_arguments;

public:
    Vector<Bytecode::UnwindInfo> unwind_contexts;
    Vector<Optional<size_t>> previously_scheduled_jumps;
    Vector<GC::Ptr<Environment>> saved_lexical_environments;
};

struct StackTraceElement {
    ExecutionContext* execution_context;
    RefPtr<CachedSourceRange> source_range;
};

}
