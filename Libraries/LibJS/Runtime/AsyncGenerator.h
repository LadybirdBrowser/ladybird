/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// 27.6.2 Properties of AsyncGenerator Instances, https://tc39.es/ecma262/#sec-properties-of-asyncgenerator-intances
class AsyncGenerator final : public Object {
    JS_OBJECT(AsyncGenerator, Object);
    JS_DECLARE_ALLOCATOR(AsyncGenerator);

public:
    enum class State {
        SuspendedStart,
        SuspendedYield,
        Executing,
        AwaitingReturn,
        Completed,
    };

    static ThrowCompletionOr<NonnullGCPtr<AsyncGenerator>> create(Realm&, Value, ECMAScriptFunctionObject*, NonnullOwnPtr<ExecutionContext>);

    virtual ~AsyncGenerator() override;

    void async_generator_enqueue(Completion, NonnullGCPtr<PromiseCapability>);
    ThrowCompletionOr<void> resume(VM&, Completion completion);
    void await_return();
    void complete_step(Completion, bool done, Realm* realm = nullptr);
    void drain_queue();

    State async_generator_state() const { return m_async_generator_state; }
    void set_async_generator_state(Badge<AsyncGeneratorPrototype>, State value);

    Optional<String> const& generator_brand() const { return m_generator_brand; }

private:
    AsyncGenerator(Realm&, Object& prototype, NonnullOwnPtr<ExecutionContext>);

    virtual void visit_edges(Cell::Visitor&) override;

    void execute(VM&, Completion completion);
    ThrowCompletionOr<void> await(Value);

    // At the time of constructing an AsyncGenerator, we still need to point to an
    // execution context on the stack, but later need to 'adopt' it.
    State m_async_generator_state { State::SuspendedStart };   // [[AsyncGeneratorState]]
    NonnullOwnPtr<ExecutionContext> m_async_generator_context; // [[AsyncGeneratorContext]]
    Vector<AsyncGeneratorRequest> m_async_generator_queue;     // [[AsyncGeneratorQueue]]
    Optional<String> m_generator_brand;                        // [[GeneratorBrand]]

    GCPtr<ECMAScriptFunctionObject> m_generating_function;
    Value m_previous_value;
    GCPtr<Promise> m_current_promise;
};

}
