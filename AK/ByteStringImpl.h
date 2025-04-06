/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {

enum ShouldChomp {
    NoChomp,
    Chomp
};

size_t allocation_size_for_stringimpl(size_t length);

class ByteStringImpl : public RefCounted<ByteStringImpl> {
public:
    static NonnullRefPtr<ByteStringImpl const> create_uninitialized(size_t length, char*& buffer);
    static NonnullRefPtr<ByteStringImpl const> create(char const* cstring, ShouldChomp = NoChomp);
    static NonnullRefPtr<ByteStringImpl const> create(char const* cstring, size_t length, ShouldChomp = NoChomp);
    static NonnullRefPtr<ByteStringImpl const> create(ReadonlyBytes, ShouldChomp = NoChomp);

    void operator delete(void* ptr)
    {
        kfree_sized(ptr, allocation_size_for_stringimpl(static_cast<ByteStringImpl*>(ptr)->m_length));
    }

    static ByteStringImpl& the_empty_stringimpl();

    ~ByteStringImpl();

    size_t length() const { return m_length; }
    // Includes NUL-terminator.
    char const* characters() const { return &m_inline_buffer[0]; }

    ALWAYS_INLINE ReadonlyBytes bytes() const { return { characters(), length() }; }
    ALWAYS_INLINE StringView view() const { return { characters(), length() }; }

    char const& operator[](size_t i) const
    {
        VERIFY(i < m_length);
        return characters()[i];
    }

    bool operator==(ByteStringImpl const& other) const
    {
        if (length() != other.length())
            return false;
        return __builtin_memcmp(characters(), other.characters(), length()) == 0;
    }

    unsigned hash() const
    {
        if (!m_has_hash)
            compute_hash();
        return m_hash;
    }

    unsigned existing_hash() const
    {
        return m_hash;
    }

    unsigned case_insensitive_hash() const;

private:
    enum ConstructTheEmptyStringImplTag {
        ConstructTheEmptyStringImpl
    };
    explicit ByteStringImpl(ConstructTheEmptyStringImplTag)
    {
        m_inline_buffer[0] = '\0';
    }

    enum ConstructWithInlineBufferTag {
        ConstructWithInlineBuffer
    };
    ByteStringImpl(ConstructWithInlineBufferTag, size_t length);

    void compute_hash() const;

    size_t m_length { 0 };
    mutable unsigned m_hash { 0 };
    mutable bool m_has_hash { false };
    char m_inline_buffer[0];
};

inline size_t allocation_size_for_stringimpl(size_t length)
{
    return sizeof(ByteStringImpl) + (sizeof(char) * length) + sizeof(char);
}

template<>
struct Formatter<ByteStringImpl> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, ByteStringImpl const& value)
    {
        return Formatter<StringView>::format(builder, { value.characters(), value.length() });
    }
};

}

#if USING_AK_GLOBALLY
using AK::Chomp;
using AK::NoChomp;
#endif
