/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibJS/Runtime/ExecutionContext.h>

namespace JS {

class InterpreterStack {
    AK_MAKE_NONCOPYABLE(InterpreterStack);
    AK_MAKE_NONMOVABLE(InterpreterStack);

public:
    static constexpr size_t stack_size = 8 * MiB;

    InterpreterStack();
    ~InterpreterStack();

    [[nodiscard]] ALWAYS_INLINE void* top() const { return m_top; }

    [[nodiscard]] ALWAYS_INLINE ExecutionContext* allocate(u32 registers_and_locals_count, ReadonlySpan<Value> constants, u32 arguments_count)
    {
        auto tail_count = registers_and_locals_count + constants.size() + arguments_count;
        auto size = sizeof(ExecutionContext) + tail_count * sizeof(Value);

        // Align up to alignof(ExecutionContext).
        size = (size + alignof(ExecutionContext) - 1) & ~(alignof(ExecutionContext) - 1);

        auto* new_top = static_cast<u8*>(m_top) + size;
        if (new_top > m_limit) [[unlikely]]
            return nullptr;

        auto* result = new (m_top) ExecutionContext(registers_and_locals_count, constants, arguments_count);
        m_top = new_top;
        return result;
    }

    ALWAYS_INLINE void deallocate(void* mark)
    {
        VERIFY(mark >= m_base && mark <= m_top);
        m_top = mark;
    }

    [[nodiscard]] ALWAYS_INLINE bool is_exhausted() const
    {
        return m_top >= m_limit;
    }

    [[nodiscard]] ALWAYS_INLINE size_t size_remaining() const
    {
        return static_cast<u8*>(m_limit) - static_cast<u8*>(m_top);
    }

private:
    void* m_base { nullptr };
    void* m_top { nullptr };
    void* m_limit { nullptr };
};

}
