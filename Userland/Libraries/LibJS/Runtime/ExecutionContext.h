/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <AK/WeakPtr.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Forward.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceRange.h>

namespace JS {

using ScriptOrModule = Variant<Empty, NonnullGCPtr<Script>, NonnullGCPtr<Module>>;

// 9.4 Execution Contexts, https://tc39.es/ecma262/#sec-execution-contexts
struct ExecutionContext {
    static NonnullOwnPtr<ExecutionContext> create();
    [[nodiscard]] NonnullOwnPtr<ExecutionContext> copy() const;

    ~ExecutionContext();

    void visit_edges(Cell::Visitor&);

private:
    friend class ExecutionContextAllocator;

    ExecutionContext();

public:
    void operator delete(void* ptr);

    GCPtr<FunctionObject> function;                // [[Function]]
    GCPtr<Realm> realm;                            // [[Realm]]
    ScriptOrModule script_or_module;               // [[ScriptOrModule]]
    GCPtr<Environment> lexical_environment;        // [[LexicalEnvironment]]
    GCPtr<Environment> variable_environment;       // [[VariableEnvironment]]
    GCPtr<PrivateEnvironment> private_environment; // [[PrivateEnvironment]]

    // Non-standard: This points at something that owns this ExecutionContext, in case it needs to be protected from GC.
    GCPtr<Cell> context_owner;

    Optional<size_t> program_counter;
    GCPtr<PrimitiveString> function_name;
    Value this_value;

    GCPtr<Bytecode::Executable> executable;

    // https://html.spec.whatwg.org/multipage/webappapis.html#skip-when-determining-incumbent-counter
    // FIXME: Move this out of LibJS (e.g. by using the CustomData concept), as it's used exclusively by LibWeb.
    size_t skip_when_determining_incumbent_counter { 0 };

    Value argument(size_t index) const
    {
        if (index >= arguments.size()) [[unlikely]]
            return js_undefined();
        return arguments[index];
    }

    Value& local(size_t index)
    {
        return registers_and_constants_and_locals[index];
    }

    u32 passed_argument_count { 0 };
    bool is_strict_mode { false };

    Vector<Value> arguments;
    Vector<Value> registers_and_constants_and_locals;
    Vector<Bytecode::UnwindInfo> unwind_contexts;
    Vector<Optional<size_t>> previously_scheduled_jumps;
    Vector<GCPtr<Environment>> saved_lexical_environments;
};

struct StackTraceElement {
    ExecutionContext* execution_context;
    Optional<UnrealizedSourceRange> source_range;
};

}
