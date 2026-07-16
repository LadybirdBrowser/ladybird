/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/Export.h>

namespace Wasm {

// A fixed reservation keeps the backing stable for compiled code and is recycled between calls.
class WASM_API ValueStack {
    AK_MAKE_NONCOPYABLE(ValueStack);
    AK_MAKE_NONMOVABLE(ValueStack);

public:
    static constexpr size_t reservation_size = 64 * MiB;
    static constexpr size_t max_values = reservation_size / sizeof(Value);

    ValueStack();
    ~ValueStack();

    ALWAYS_INLINE size_t size() const { return static_cast<size_t>(m_top - m_base); }
    ALWAYS_INLINE bool is_empty() const { return m_top == m_base; }

    ALWAYS_INLINE void append(Value value)
    {
        VERIFY(m_top != m_limit);
        *m_top++ = value;
    }
    ALWAYS_INLINE void unchecked_append(Value value) { *m_top++ = value; }

    ALWAYS_INLINE Value take_last()
    {
        VERIFY(m_top != m_base);
        return *--m_top;
    }
    ALWAYS_INLINE Value unsafe_take_last() { return *--m_top; }
    ALWAYS_INLINE Value& last() { return *(m_top - 1); }
    ALWAYS_INLINE Value& unsafe_last() { return *(m_top - 1); }

    ALWAYS_INLINE Value* data() { return m_base; }
    ALWAYS_INLINE Value const* data() const { return m_base; }
    ALWAYS_INLINE Value* begin() { return m_base; }
    ALWAYS_INLINE Value* end() { return m_top; }
    ALWAYS_INLINE Value const* begin() const { return m_base; }
    ALWAYS_INLINE Value const* end() const { return m_top; }
    ALWAYS_INLINE Span<Value> span() { return { m_base, size() }; }
    ALWAYS_INLINE ReadonlySpan<Value> span() const { return { m_base, size() }; }

    ALWAYS_INLINE void shrink(size_t new_size, bool = false) // mirroring keep_capacity, ignored
    {
        VERIFY(new_size <= size());
        m_top = m_base + new_size;
    }

    ALWAYS_INLINE void restore_size(size_t size)
    {
        VERIFY(size <= max_values);
        m_top = m_base + size;
    }

    void remove(size_t index, size_t count)
    {
        VERIFY(index + count <= size());
        __builtin_memmove(m_base + index, m_base + index + count, (size() - index - count) * sizeof(Value));
        m_top -= count;
    }

    ALWAYS_INLINE void ensure_capacity(size_t total) { VERIFY(total <= max_values); }

    // The conservative GC scans a little past the top;
    // a value handed out by unsafe_take_last() can still be live in a caller's register when a collection runs.
    size_t conservative_scan_size() const { return min(size() + 8, max_values); }

    static constexpr size_t base_offset() { return __builtin_offsetof(ValueStack, m_base); }
    static constexpr size_t top_offset() { return __builtin_offsetof(ValueStack, m_top); }

private:
    Value* m_base { nullptr };
    Value* m_top { nullptr };
    Value* m_limit { nullptr };
};

}
