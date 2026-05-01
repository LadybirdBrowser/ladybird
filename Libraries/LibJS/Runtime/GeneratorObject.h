/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class GeneratorObject : public Object {
    JS_OBJECT(GeneratorObject, Object);
    GC_DECLARE_ALLOCATOR(GeneratorObject);

public:
    static GC::Ref<GeneratorObject> create(Realm&, Variant<GC::Ref<ECMAScriptFunctionObject>, GC::Ref<NativeJavaScriptBackedFunction>>, NonnullOwnPtr<ExecutionContext>);
    virtual ~GeneratorObject() override = default;
    void visit_edges(Cell::Visitor&) override;

    struct IterationResult {
        IterationResult() = delete;
        explicit IterationResult(Value value, bool done)
            : done(done)
            , value(value)
        {
        }

        bool done { false };
        Value value;
    };

    ThrowCompletionOr<IterationResult> resume(VM&, Value value, Optional<StringView> const& generator_brand);
    ThrowCompletionOr<IterationResult> resume_abrupt(VM&, JS::Completion abrupt_completion, Optional<StringView> const& generator_brand);

    enum class GeneratorState {
        SuspendedStart,
        SuspendedYield,
        Executing,
        Completed,
    };
    GeneratorState generator_state() const { return m_generator_state; }
    void set_generator_state(GeneratorState generator_state) { m_generator_state = generator_state; }

    void set_pending_completion(Completion const& completion)
    {
        m_pending_completion_value = completion.value();
        m_pending_completion_type = completion.type();
    }
    Value pending_completion_value() const { return m_pending_completion_value; }
    Completion::Type pending_completion_type() const { return m_pending_completion_type; }
    void set_pending_completion_type(Completion::Type completion_type) { m_pending_completion_type = completion_type; }
    void clear_pending_completion()
    {
        m_pending_completion_value = js_undefined();
        m_pending_completion_type = Completion::Type::Normal;
    }

protected:
    GeneratorObject(Realm&, Object* prototype, NonnullOwnPtr<ExecutionContext>, Optional<StringView> generator_brand = {});

    ThrowCompletionOr<GeneratorState> validate(VM&, Optional<StringView> const& generator_brand);
    virtual ThrowCompletionOr<IterationResult> execute(VM&, JS::Completion const& completion);

private:
    NonnullOwnPtr<ExecutionContext> m_execution_context;
    GC::Ptr<Bytecode::Executable> m_generating_executable;
    u32 m_yield_continuation { ExecutionContext::no_yield_continuation };
    GeneratorState m_generator_state { GeneratorState::SuspendedStart };
    Optional<StringView> m_generator_brand;
    Value m_pending_completion_value { js_undefined() };
    Completion::Type m_pending_completion_type { Completion::Type::Normal };
};

}
