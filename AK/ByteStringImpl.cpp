/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteStringImpl.h>
#include <AK/CharacterTypes.h>
#include <AK/StringHash.h>
#include <AK/kmalloc.h>

namespace AK {

// Static storage for the empty ByteStringImpl, avoiding heap allocation
// and runtime initialization. The struct layout matches ByteStringImpl
// with space for the NUL terminator in the inline buffer.
struct EmptyByteStringImpl {
    // Members from AtomicRefCountedBase
    Atomic<unsigned> ref_count { 1 };

    // Members from ByteStringImpl
    size_t length { 0 };
    unsigned hash { 0 };
    bool has_hash { false };
    char inline_buffer[1] { '\0' };

    constexpr EmptyByteStringImpl() = default;
};

// The inline_buffer[1] fits within the trailing padding of ByteStringImpl's layout.
static_assert(sizeof(EmptyByteStringImpl) == sizeof(ByteStringImpl));
static_assert(alignof(EmptyByteStringImpl) == alignof(ByteStringImpl));

static constinit EmptyByteStringImpl s_the_empty_stringimpl;

ByteStringImpl& ByteStringImpl::the_empty_stringimpl()
{
    return reinterpret_cast<ByteStringImpl&>(s_the_empty_stringimpl);
}

ByteStringImpl::ByteStringImpl(ConstructWithInlineBufferTag, size_t length)
    : m_length(length)
{
}

ByteStringImpl::~ByteStringImpl() = default;

NonnullRefPtr<ByteStringImpl const> ByteStringImpl::create_uninitialized(size_t length, char*& buffer)
{
    VERIFY(length);
    void* slot = kmalloc(allocation_size_for_stringimpl(length));
    VERIFY(slot);
    auto new_stringimpl = adopt_ref(*new (slot) ByteStringImpl(ConstructWithInlineBuffer, length));
    buffer = const_cast<char*>(new_stringimpl->characters());
    buffer[length] = '\0';
    return new_stringimpl;
}

NonnullRefPtr<ByteStringImpl const> ByteStringImpl::create(char const* cstring, size_t length, ShouldChomp should_chomp)
{
    if (should_chomp) {
        while (length) {
            char last_ch = cstring[length - 1];
            if (!last_ch || last_ch == '\n' || last_ch == '\r')
                --length;
            else
                break;
        }
    }

    if (!length)
        return the_empty_stringimpl();

    char* buffer;
    auto new_stringimpl = create_uninitialized(length, buffer);
    memcpy(buffer, cstring, length * sizeof(char));

    return new_stringimpl;
}

NonnullRefPtr<ByteStringImpl const> ByteStringImpl::create(char const* cstring, ShouldChomp shouldChomp)
{
    if (!cstring || !*cstring)
        return the_empty_stringimpl();

    return create(cstring, strlen(cstring), shouldChomp);
}

NonnullRefPtr<ByteStringImpl const> ByteStringImpl::create(ReadonlyBytes bytes, ShouldChomp shouldChomp)
{
    return ByteStringImpl::create(reinterpret_cast<char const*>(bytes.data()), bytes.size(), shouldChomp);
}

unsigned ByteStringImpl::case_insensitive_hash() const
{
    return case_insensitive_string_hash(characters(), length());
}

void ByteStringImpl::compute_hash() const
{
    if (!length())
        m_hash = 0;
    else
        m_hash = string_hash(characters(), m_length);
    m_has_hash = true;
}

}
