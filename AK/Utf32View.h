/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/Format.h>
#include <AK/Types.h>

namespace AK {

class Utf32View;

class Utf32CodePointIterator {
    friend class Utf32View;

public:
    Utf32CodePointIterator() = default;
    ~Utf32CodePointIterator() = default;

    bool operator==(Utf32CodePointIterator const& other) const
    {
        return m_ptr == other.m_ptr && m_length == other.m_length;
    }
    Utf32CodePointIterator& operator++()
    {
        VERIFY(m_length > 0);
        m_ptr++;
        m_length--;
        return *this;
    }
    ssize_t operator-(Utf32CodePointIterator const& other) const
    {
        return m_ptr - other.m_ptr;
    }
    u32 operator*() const
    {
        VERIFY(m_length > 0);
        return *m_ptr;
    }

    // NOTE: This returns {} if the peek is at or past EOF.
    Optional<u32> peek(size_t offset = 0) const;

    constexpr int code_point_length_in_bytes() const { return sizeof(u32); }
    bool done() const { return !m_length; }

private:
    Utf32CodePointIterator(u32 const* ptr, size_t length)
        : m_ptr(ptr)
        , m_length((ssize_t)length)
    {
    }
    u32 const* m_ptr { nullptr };
    ssize_t m_length { -1 };
};

class Utf32View {
public:
    using Iterator = Utf32CodePointIterator;

    Utf32View() = default;
    Utf32View(u32 const* code_points, size_t length)
        : m_code_points(code_points)
        , m_length(length)
    {
        VERIFY(code_points || length == 0);
    }

    Utf32View(ReadonlySpan<u32> code_points)
        : Utf32View(code_points.data(), code_points.size())
    {
    }

    Utf32CodePointIterator begin() const
    {
        return { begin_ptr(), m_length };
    }

    Utf32CodePointIterator end() const
    {
        return { end_ptr(), 0 };
    }

    u32 at(size_t index) const
    {
        VERIFY(index < m_length);
        return m_code_points[index];
    }

    u32 operator[](size_t index) const { return at(index); }

    u32 const* code_points() const { return m_code_points; }
    bool is_empty() const { return m_length == 0; }
    bool is_null() const { return !m_code_points; }
    size_t length() const { return m_length; }

    size_t iterator_offset(Utf32CodePointIterator const& it) const
    {
        VERIFY(it.m_ptr >= m_code_points);
        VERIFY(it.m_ptr < m_code_points + m_length);
        return ((ptrdiff_t)it.m_ptr - (ptrdiff_t)m_code_points) / sizeof(u32);
    }

    Utf32View substring_view(size_t offset, size_t length) const
    {
        VERIFY(offset <= m_length);
        VERIFY(!Checked<size_t>::addition_would_overflow(offset, length));
        VERIFY((offset + length) <= m_length);
        return Utf32View(m_code_points + offset, length);
    }

    Utf32View substring_view(size_t offset) const
    {
        return substring_view(offset, length() - offset);
    }

    bool operator==(Utf32View const& other) const;

private:
    u32 const* begin_ptr() const
    {
        return m_code_points;
    }
    u32 const* end_ptr() const
    {
        return m_code_points + m_length;
    }

    u32 const* m_code_points { nullptr };
    size_t m_length { 0 };
};

template<>
struct Formatter<Utf32View> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Utf32View const&);
};

}

#if USING_AK_GLOBALLY
using AK::Utf32View;
#endif
