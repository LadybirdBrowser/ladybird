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

static ByteStringImpl* s_the_empty_stringimpl = nullptr;

ByteStringImpl& ByteStringImpl::the_empty_stringimpl()
{
    if (!s_the_empty_stringimpl) {
        void* slot = kmalloc(sizeof(ByteStringImpl) + sizeof(char));
        s_the_empty_stringimpl = new (slot) ByteStringImpl(ConstructTheEmptyStringImpl);
    }
    return *s_the_empty_stringimpl;
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
