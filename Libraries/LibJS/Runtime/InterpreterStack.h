/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ExecutionContext.h>

namespace JS {

// The VM's stack of execution contexts that live as long as a block of code, which the runtime's interpreter
// allocates its frames on, too. A context allocated here lives until deallocate() is passed a mark taken before it.
class JS_API InterpreterStack {
    AK_MAKE_NONCOPYABLE(InterpreterStack);
    AK_MAKE_NONMOVABLE(InterpreterStack);

public:
    explicit InterpreterStack(VM& vm)
        : m_vm(vm)
    {
    }

    [[nodiscard]] void* top() const;

    // Null if the stack has no room for the context. Its argument slots are uninitialized.
    [[nodiscard]] ExecutionContext* allocate(u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count);

    void deallocate(void* mark);

private:
    VM& m_vm;
};

}
