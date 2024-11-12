/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>

namespace JS {

class AsyncFunctionDriverWrapper final : public Promise {
    JS_OBJECT(AsyncFunctionDriverWrapper, Promise);
    JS_DECLARE_ALLOCATOR(AsyncFunctionDriverWrapper);

public:
    enum class IsInitialExecution {
        No,
        Yes,
    };

    [[nodiscard]] static NonnullGCPtr<Promise> create(Realm&, GeneratorObject*);

    virtual ~AsyncFunctionDriverWrapper() override = default;
    void visit_edges(Cell::Visitor&) override;

    void continue_async_execution(VM&, Value, bool is_successful, IsInitialExecution is_initial_execution = IsInitialExecution::No);

private:
    AsyncFunctionDriverWrapper(Realm&, NonnullGCPtr<GeneratorObject>, NonnullGCPtr<Promise> top_level_promise);
    ThrowCompletionOr<void> await(Value);

    NonnullGCPtr<GeneratorObject> m_generator_object;
    NonnullGCPtr<Promise> m_top_level_promise;
    GCPtr<Promise> m_current_promise { nullptr };
    OwnPtr<ExecutionContext> m_suspended_execution_context;
};

}
