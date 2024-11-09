/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class GeneratorObject : public Object {
    JS_OBJECT(GeneratorObject, Object);
    JS_DECLARE_ALLOCATOR(GeneratorObject);

public:
    static ThrowCompletionOr<NonnullGCPtr<GeneratorObject>> create(Realm&, Value, ECMAScriptFunctionObject*, NonnullOwnPtr<ExecutionContext>);
    virtual ~GeneratorObject() override = default;
    void visit_edges(Cell::Visitor&) override;

    ThrowCompletionOr<Value> resume(VM&, Value value, Optional<StringView> const& generator_brand);
    ThrowCompletionOr<Value> resume_abrupt(VM&, JS::Completion abrupt_completion, Optional<StringView> const& generator_brand);

    enum class GeneratorState {
        SuspendedStart,
        SuspendedYield,
        Executing,
        Completed,
    };
    GeneratorState generator_state() const { return m_generator_state; }
    void set_generator_state(GeneratorState generator_state) { m_generator_state = generator_state; }

protected:
    GeneratorObject(Realm&, Object& prototype, NonnullOwnPtr<ExecutionContext>, Optional<StringView> generator_brand = {});

    ThrowCompletionOr<GeneratorState> validate(VM&, Optional<StringView> const& generator_brand);
    virtual ThrowCompletionOr<Value> execute(VM&, JS::Completion const& completion);

private:
    NonnullOwnPtr<ExecutionContext> m_execution_context;
    GCPtr<ECMAScriptFunctionObject> m_generating_function;
    Value m_previous_value;
    GeneratorState m_generator_state { GeneratorState::SuspendedStart };
    Optional<StringView> m_generator_brand;
};

}
