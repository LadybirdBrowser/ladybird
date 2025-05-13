/*
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/FlyString.h>
#include <AK/StringBase.h>
#include <AK/StringData.h>

namespace AK::Detail {

void StringBase::replace_with_string_builder(StringBuilder& builder)
{
    if (builder.length() <= MAX_SHORT_STRING_BYTE_COUNT) {
        return replace_with_new_short_string(builder.length(), [&](Bytes buffer) {
            builder.string_view().bytes().copy_to(buffer);
        });
    }

    destroy_string();

    m_impl = { .data = &StringData::create_from_string_builder(builder).leak_ref() };
}

ErrorOr<Bytes> StringBase::replace_with_uninitialized_buffer(size_t byte_count)
{
    if (byte_count <= MAX_SHORT_STRING_BYTE_COUNT)
        return replace_with_uninitialized_short_string(byte_count);

    u8* buffer = nullptr;
    destroy_string();
    m_impl = { .data = &TRY(StringData::create_uninitialized(byte_count, buffer)).leak_ref() };
    return Bytes { buffer, byte_count };
}

ErrorOr<StringBase> StringBase::substring_from_byte_offset_with_shared_superstring(size_t start, size_t length) const
{
    VERIFY(start + length <= byte_count());

    if (length == 0)
        return StringBase {};
    if (length <= MAX_SHORT_STRING_BYTE_COUNT) {
        StringBase result;
        bytes().slice(start, length).copy_to(result.replace_with_uninitialized_short_string(length));
        return result;
    }
    return StringBase { TRY(Detail::StringData::create_substring(*m_impl.data, start, length)) };
}

}
