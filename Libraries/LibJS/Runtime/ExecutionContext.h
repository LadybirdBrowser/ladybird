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

class CachedSourceRange final : public GC::Cell {
    GC_CELL(CachedSourceRange, GC::Cell);
    GC_DECLARE_ALLOCATOR(CachedSourceRange);

public:
    CachedSourceRange(size_t program_counter, Variant<UnrealizedSourceRange, SourceRange> source_range)
        : program_counter(program_counter)
        , source_range(move(source_range))
    {
    }

    SourceRange const& realize_source_range()
    {
        static SourceRange dummy_source_range { SourceCode::create({}, {}), {}, {} };

        if (auto* unrealized = source_range.get_pointer<UnrealizedSourceRange>()) {
            if (unrealized->source_code) {
                source_range = unrealized->realize();
            } else {
                source_range = dummy_source_range;
            }
        }
        return source_range.get<SourceRange>();
    }

    size_t program_counter { 0 };
    Variant<UnrealizedSourceRange, SourceRange> source_range;
};

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
    ALWAYS_INLINE ExecutionContext(u32 registers_and_locals_count, u32 constants_count, u32 arguments_count)
    {
        VERIFY(!Checked<u32>::addition_would_overflow(registers_and_locals_count, constants_count, arguments_count));
        registers_and_constants_and_locals_and_arguments_count = registers_and_locals_count + constants_count + arguments_count;
        auto registers_and_locals_and_constants_count = registers_and_locals_count + constants_count;
        auto* values = registers_and_constants_and_locals_and_arguments();
        for (size_t i = 0; i < registers_and_locals_count; ++i)
            values[i] = js_special_empty_value();
        arguments = { values + registers_and_locals_and_constants_count, arguments_count };
    }

    void operator delete(void* ptr);

    GC::Ptr<FunctionObject> function;                // [[Function]]
    GC::Ptr<Realm> realm;                            // [[Realm]]
    ScriptOrModule script_or_module;                 // [[ScriptOrModule]]
    GC::Ptr<Environment> lexical_environment;        // [[LexicalEnvironment]]
    GC::Ptr<Environment> variable_environment;       // [[VariableEnvironment]]
    GC::Ptr<PrivateEnvironment> private_environment; // [[PrivateEnvironment]]

    GC::Ptr<Object> global_object;
    GC::Ptr<DeclarativeEnvironment> global_declarative_environment;
    Utf16FlyString const* identifier_table { nullptr };
    PropertyKey const* property_key_table { nullptr };

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
        if (index >= arguments.size()) [[unlikely]]
            return js_undefined();
        return arguments.data()[index];
    }

    Span<Value> arguments;

    mutable GC::Ptr<CachedSourceRange> cached_source_range;

    // Non-standard: This points at something that owns this ExecutionContext, in case it needs to be protected from GC.
    GC::Ptr<GC::Cell> context_owner;

    u32 passed_argument_count { 0 };

private:
    friend class Bytecode::Interpreter;

    Value* registers_and_constants_and_locals_and_arguments()
    {
        return reinterpret_cast<Value*>(reinterpret_cast<uintptr_t>(this) + sizeof(ExecutionContext));
    }

    u32 registers_and_constants_and_locals_and_arguments_count { 0 };
};

static_assert(IsTriviallyDestructible<ExecutionContext>);

#define ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(execution_context, \
    registers_and_locals_count,                                                             \
    constants_count,                                                                        \
    arguments_count)                                                                        \
    auto execution_context_size = sizeof(JS::ExecutionContext)                              \
        + (((registers_and_locals_count) + (constants_count) + (arguments_count))           \
            * sizeof(JS::Value));                                                           \
                                                                                            \
    void* execution_context_memory = alloca(execution_context_size);                        \
                                                                                            \
    execution_context = new (execution_context_memory)                                      \
        JS::ExecutionContext((registers_and_locals_count), (constants_count), (arguments_count));

#define ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(execution_context, registers_and_locals_count, \
    constants_count, arguments_count)                                                             \
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(execution_context,           \
        registers_and_locals_count, constants_count, arguments_count);                            \
    do {                                                                                          \
        for (size_t i = 0; i < execution_context->arguments.size(); i++) {                        \
            execution_context->arguments[i] = JS::js_undefined();                                 \
        }                                                                                         \
    } while (0)

struct StackTraceElement {
    ExecutionContext* execution_context { nullptr };
    GC::Ptr<CachedSourceRange> source_range;
};

}
