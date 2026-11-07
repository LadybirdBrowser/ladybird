/*
 * Copyright (c) 2020-2026, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/Checked.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceRange.h>

namespace JS {

// [[ScriptOrModule]]: null, a Script Record or a Module Record, in the one word the Rust runtime keeps it in: zero for
// null, otherwise the pointer to the record with its lowest bit set for a module. It offers the part of
// Variant<Empty, GC::Ref<Script>, GC::Ref<Module>>'s interface that LibJS's users call.
class ScriptOrModule {
public:
    ScriptOrModule() { }
    ScriptOrModule(Empty) { }

    ScriptOrModule(GC::Ref<Script> script)
        : m_bits(bit_cast<FlatPtr>(script.ptr()))
    {
    }

    ScriptOrModule(GC::Ref<Module> module)
        : m_bits(bit_cast<FlatPtr>(module.ptr()) | JS_LAYOUT_SCRIPT_OR_MODULE_MODULE_BIT)
    {
    }

    template<typename T>
    bool has() const
    {
        if constexpr (IsSame<T, Empty>)
            return m_bits == 0;
        else if constexpr (IsSame<T, GC::Ref<Script>>)
            return m_bits != 0 && !(m_bits & JS_LAYOUT_SCRIPT_OR_MODULE_MODULE_BIT);
        else if constexpr (IsSame<T, GC::Ref<Module>>)
            return m_bits & JS_LAYOUT_SCRIPT_OR_MODULE_MODULE_BIT;
        else
            static_assert(DependentFalse<T>, "A ScriptOrModule holds Empty, GC::Ref<Script> or GC::Ref<Module>");
    }

    template<typename... Functions>
    decltype(auto) visit(Functions&&... functions) const
    {
        OverloadedVisitor<RemoveCVReference<Functions>...> visitor { forward<Functions>(functions)... };
        if (has<GC::Ref<Script>>()) {
            GC::Ref<Script> script = *bit_cast<Script*>(m_bits);
            return visitor(script);
        }
        if (has<GC::Ref<Module>>()) {
            GC::Ref<Module> module = *bit_cast<Module*>(m_bits & ~static_cast<FlatPtr>(JS_LAYOUT_SCRIPT_OR_MODULE_MODULE_BIT));
            return visitor(module);
        }
        Empty empty;
        return visitor(empty);
    }

    void visit_edges(GC::Cell::Visitor& visitor) const
    {
        visit(
            [](Empty) { },
            [&](auto const& script_or_module) { visitor.visit(script_or_module); });
    }

    bool operator==(ScriptOrModule const&) const = default;

private:
    template<typename... Functions>
    struct AK_COMPACT_EMPTY_BASES OverloadedVisitor : Functions... {
        using Functions::operator()...;
    };

    FlatPtr m_bits { 0 };
};

static_assert(sizeof(ScriptOrModule) == JS_LAYOUT_SCRIPT_OR_MODULE_SIZE);
static_assert(IsTriviallyCopyable<ScriptOrModule>);

// 9.4 Execution Contexts, https://tc39.es/ecma262/#sec-execution-contexts
// The Rust runtime's execution context, field for field. Its value slots follow it directly: the registers and locals,
// the constants, then the arguments. Only the runtime constructs and destroys one, through create(), copy() and the
// VM's interpreter stack.
struct JS_API ExecutionContext {
    static NonnullOwnPtr<ExecutionContext> create(u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count);
    [[nodiscard]] NonnullOwnPtr<ExecutionContext> copy() const;

    ExecutionContext() = delete;
    ExecutionContext(ExecutionContext const&) = delete;
    ExecutionContext& operator=(ExecutionContext const&) = delete;
    ~ExecutionContext() = default;

    void operator delete(void* execution_context);

    GC_VISITS_THROUGH_FOREIGN_IMPLEMENTATION void visit_edges(GC::Cell::Visitor&);

    GC::Ptr<FunctionObject> function;                // [[Function]]
    GC::Ptr<Realm> realm;                            // [[Realm]]
    ScriptOrModule script_or_module;                 // [[ScriptOrModule]]
    GC::Ptr<Environment> lexical_environment;        // [[LexicalEnvironment]]
    GC::Ptr<Environment> variable_environment;       // [[VariableEnvironment]]
    GC::Ptr<PrivateEnvironment> private_environment; // [[PrivateEnvironment]]

    u64 frame_id { 0 };
    u32 program_counter { 0 };

    // https://html.spec.whatwg.org/multipage/webappapis.html#skip-when-determining-incumbent-counter
    u32 skip_when_determining_incumbent_counter { 0 };

    static constexpr u32 no_yield_continuation = NumericLimits<u32>::max();
    u32 yield_continuation { no_yield_continuation };

    bool yield_is_await { false };
    bool yield_value_is_iterator_result { false };
    bool caller_is_construct { false };
    bool frame_initialized { false };

    Optional<Value> this_value;

    // The bytecode the context runs, a cell of the runtime that only the runtime looks into.
    GC::Ptr<GC::Cell> executable;

    ExecutionContext* caller_frame { nullptr };
    u32 passed_argument_count { 0 };
    u32 caller_return_pc { 0 };
    u32 caller_dst_raw { 0 };
    u32 registers_and_constants_and_locals_and_arguments_count { 0 };
    u32 argument_count { 0 };

    // Non-standard: Set on an inline frame whose caller waits for it in native code (a call made by JIT code) rather
    // than in the interpreter.
    bool returns_to_native_caller { false };

    // Non-standard: Set while JIT code runs the frame.
    bool runs_jit_code { false };

    Value* registers_and_constants_and_locals_and_arguments()
    {
        return reinterpret_cast<Value*>(reinterpret_cast<uintptr_t>(this) + sizeof(ExecutionContext));
    }

    Value const* registers_and_constants_and_locals_and_arguments() const
    {
        return reinterpret_cast<Value const*>(reinterpret_cast<uintptr_t>(this) + sizeof(ExecutionContext));
    }

    Span<Value> registers_and_constants_and_locals_and_arguments_span()
    {
        return { registers_and_constants_and_locals_and_arguments(), registers_and_constants_and_locals_and_arguments_count };
    }

    Value* arguments_data()
    {
        return registers_and_constants_and_locals_and_arguments() + (registers_and_constants_and_locals_and_arguments_count - argument_count);
    }

    Value const* arguments_data() const
    {
        return registers_and_constants_and_locals_and_arguments() + (registers_and_constants_and_locals_and_arguments_count - argument_count);
    }

    Value argument(size_t index) const
    {
        if (index >= argument_count) [[unlikely]]
            return js_undefined();
        return arguments_data()[index];
    }

    Span<Value> arguments_span() { return { arguments_data(), argument_count }; }
    ReadonlySpan<Value> arguments_span() const { return { arguments_data(), argument_count }; }

    // Non-standard: The source code and function name of the bytecode this context runs, for debuggers. A context
    // that runs no bytecode, such as a native function's, has no source code and an empty function name. The source
    // code is the runtime's own, whose address identifies it.
    SourceCode const* source_code() const;
    Utf16FlyString function_name() const;
};

static_assert(IsTriviallyDestructible<ExecutionContext>);
static_assert(sizeof(Optional<Value>) == sizeof(Value));

#define JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(field, layout_name)                                                  \
    static_assert(__builtin_offsetof(ExecutionContext, field) == JS_LAYOUT_EXECUTION_CONTEXT_##layout_name##_OFFSET); \
    static_assert(sizeof(ExecutionContext::field) == JS_LAYOUT_EXECUTION_CONTEXT_##layout_name##_SIZE);
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(function, FUNCTION)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(realm, REALM)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(script_or_module, SCRIPT_OR_MODULE)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(lexical_environment, LEXICAL_ENVIRONMENT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(variable_environment, VARIABLE_ENVIRONMENT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(private_environment, PRIVATE_ENVIRONMENT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(frame_id, FRAME_ID)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(program_counter, PROGRAM_COUNTER)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(skip_when_determining_incumbent_counter, SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(yield_continuation, YIELD_CONTINUATION)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(yield_is_await, YIELD_IS_AWAIT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(yield_value_is_iterator_result, YIELD_VALUE_IS_ITERATOR_RESULT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(caller_is_construct, CALLER_IS_CONSTRUCT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(frame_initialized, FRAME_INITIALIZED)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(this_value, THIS_VALUE)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(executable, EXECUTABLE)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(caller_frame, CALLER_FRAME)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(passed_argument_count, PASSED_ARGUMENT_COUNT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(caller_return_pc, CALLER_RETURN_PC)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(caller_dst_raw, CALLER_DST_RAW)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(registers_and_constants_and_locals_and_arguments_count, REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(argument_count, ARGUMENT_COUNT)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(returns_to_native_caller, RETURNS_TO_NATIVE_CALLER)
JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT(runs_jit_code, RUNS_JIT_CODE)
#undef JS_ASSERT_EXECUTION_CONTEXT_FIELD_LAYOUT
static_assert(sizeof(ExecutionContext) == JS_LAYOUT_EXECUTION_CONTEXT_SIZE);
static_assert(alignof(ExecutionContext) == JS_LAYOUT_EXECUTION_CONTEXT_ALIGN);

struct StackTraceElement {
    ExecutionContext* execution_context { nullptr };
    Optional<SourceRange> source_range;
};

}

template<>
inline constexpr bool AllocatedWithCustomAllocator<JS::ExecutionContext> = true;
