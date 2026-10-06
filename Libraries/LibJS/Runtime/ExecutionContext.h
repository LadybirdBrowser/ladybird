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

// [[ScriptOrModule]]: null, a Script Record or a Module Record. The C++ runtime's is a Variant<Empty, GC::Ref<Script>,
// GC::Ref<Module>>, whose index follows its storage. The Rust runtime puts the tag first, so this type keeps that
// layout and offers the part of Variant's interface that LibJS's users call.
class ScriptOrModule {
public:
    ScriptOrModule() { }
    ScriptOrModule(Empty) { }

    ScriptOrModule(GC::Ref<Script> script)
        : m_tag(Tag::Script)
        , m_script(script)
    {
    }

    ScriptOrModule(GC::Ref<Module> module)
        : m_tag(Tag::Module)
        , m_module(module)
    {
    }

    template<typename T>
    bool has() const
    {
        if constexpr (IsSame<T, Empty>)
            return m_tag == Tag::Empty;
        else if constexpr (IsSame<T, GC::Ref<Script>>)
            return m_tag == Tag::Script;
        else if constexpr (IsSame<T, GC::Ref<Module>>)
            return m_tag == Tag::Module;
        else
            static_assert(DependentFalse<T>, "A ScriptOrModule holds Empty, GC::Ref<Script> or GC::Ref<Module>");
    }

    template<typename T>
    T& get()
    {
        VERIFY(has<T>());
        return alternative<T>();
    }

    template<typename T>
    T const& get() const
    {
        VERIFY(has<T>());
        return const_cast<ScriptOrModule&>(*this).alternative<T>();
    }

    template<typename T>
    T* get_pointer()
    {
        return has<T>() ? &alternative<T>() : nullptr;
    }

    template<typename T>
    T const* get_pointer() const
    {
        return has<T>() ? &const_cast<ScriptOrModule&>(*this).alternative<T>() : nullptr;
    }

    template<typename... Functions>
    decltype(auto) visit(Functions&&... functions)
    {
        return visit_alternative(*this, OverloadedVisitor<RemoveCVReference<Functions>...> { forward<Functions>(functions)... });
    }

    template<typename... Functions>
    decltype(auto) visit(Functions&&... functions) const
    {
        return visit_alternative(*this, OverloadedVisitor<RemoveCVReference<Functions>...> { forward<Functions>(functions)... });
    }

    bool operator==(ScriptOrModule const& other) const
    {
        return m_tag == other.m_tag && m_cell_or_null == other.m_cell_or_null;
    }

private:
    enum class Tag : u8 {
        Empty = JS_LAYOUT_SCRIPT_OR_MODULE_TAG_EMPTY,
        Script = JS_LAYOUT_SCRIPT_OR_MODULE_TAG_SCRIPT,
        Module = JS_LAYOUT_SCRIPT_OR_MODULE_TAG_MODULE,
    };

    template<typename... Functions>
    struct AK_COMPACT_EMPTY_BASES OverloadedVisitor : Functions... {
        using Functions::operator()...;
    };

    template<typename T>
    T& alternative()
    {
        if constexpr (IsSame<T, Empty>)
            return m_empty;
        else if constexpr (IsSame<T, GC::Ref<Script>>)
            return m_script;
        else
            return m_module;
    }

    template<typename Self, typename Visitor>
    static decltype(auto) visit_alternative(Self& self, Visitor&& visitor)
    {
        auto& mutable_self = const_cast<ScriptOrModule&>(self);
        switch (self.m_tag) {
        case Tag::Script:
            return visitor(static_cast<CopyConst<Self, GC::Ref<Script>>&>(mutable_self.m_script));
        case Tag::Module:
            return visitor(static_cast<CopyConst<Self, GC::Ref<Module>>&>(mutable_self.m_module));
        case Tag::Empty:
            break;
        }
        return visitor(static_cast<CopyConst<Self, Empty>&>(mutable_self.m_empty));
    }

    Tag m_tag { Tag::Empty };
    // The payload is the cell for a script or a module, and null for an empty one.
    union {
        void* m_cell_or_null { nullptr };
        GC::Ref<Script> m_script;
        GC::Ref<Module> m_module;
    };
    // Empty has no bits, so it lives outside the payload, which the runtime reads.
    NO_UNIQUE_ADDRESS Empty m_empty;

    static constexpr void assert_layout_matches_the_runtime()
    {
        static_assert(__builtin_offsetof(ScriptOrModule, m_tag) == JS_LAYOUT_SCRIPT_OR_MODULE_TAG_OFFSET);
        static_assert(__builtin_offsetof(ScriptOrModule, m_cell_or_null) == JS_LAYOUT_SCRIPT_OR_MODULE_PAYLOAD_OFFSET);
    }
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
