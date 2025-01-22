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
    GC_DECLARE_ALLOCATOR(AsyncFunctionDriverWrapper);

public:
    [[nodiscard]] static GC::Ref<Promise> create(Realm&, GeneratorObject*);

    virtual ~AsyncFunctionDriverWrapper() override = default;
    void visit_edges(Cell::Visitor&) override;

    void continue_async_execution(VM&, Value, bool is_successful);

private:
    AsyncFunctionDriverWrapper(Realm&, GC::Ref<GeneratorObject>, GC::Ref<Promise> top_level_promise);
    ThrowCompletionOr<void> await(Value);

    GC::Ref<GeneratorObject> m_generator_object;
    GC::Ref<Promise> m_top_level_promise;
    GC::Ptr<Promise> m_current_promise { nullptr };
    OwnPtr<ExecutionContext> m_suspended_execution_context;
};

}
