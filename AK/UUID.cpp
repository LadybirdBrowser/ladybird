/*
 * Copyright (c) 2020, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Hex.h>
#include <AK/StringBuilder.h>
#include <AK/UUID.h>

namespace AK {

UUID::UUID(Array<u8, 16> uuid_buffer)
{
    uuid_buffer.span().copy_to(m_uuid_buffer);
}

void UUID::convert_string_view_to_little_endian_uuid(StringView uuid_string_view)
{
    VERIFY(uuid_string_view.length() == 36);
    auto first_unit = MUST(decode_hex(uuid_string_view.substring_view(0, 8)));
    auto second_unit = MUST(decode_hex(uuid_string_view.substring_view(9, 4)));
    auto third_unit = MUST(decode_hex(uuid_string_view.substring_view(14, 4)));
    auto fourth_unit = MUST(decode_hex(uuid_string_view.substring_view(19, 4)));
    auto fifth_unit = MUST(decode_hex(uuid_string_view.substring_view(24, 12)));

    VERIFY(first_unit.size() == 4 && second_unit.size() == 2
        && third_unit.size() == 2 && fourth_unit.size() == 2
        && fifth_unit.size() == 6);

    m_uuid_buffer.span().overwrite(0, first_unit.data(), first_unit.size());
    m_uuid_buffer.span().overwrite(4, second_unit.data(), second_unit.size());
    m_uuid_buffer.span().overwrite(6, third_unit.data(), third_unit.size());
    m_uuid_buffer.span().overwrite(8, fourth_unit.data(), fourth_unit.size());
    m_uuid_buffer.span().overwrite(10, fifth_unit.data(), fifth_unit.size());
}

void UUID::convert_string_view_to_mixed_endian_uuid(StringView uuid_string_view)
{
    VERIFY(uuid_string_view.length() == 36);
    auto first_unit = MUST(decode_hex(uuid_string_view.substring_view(0, 8)));
    auto second_unit = MUST(decode_hex(uuid_string_view.substring_view(9, 4)));
    auto third_unit = MUST(decode_hex(uuid_string_view.substring_view(14, 4)));
    auto fourth_unit = MUST(decode_hex(uuid_string_view.substring_view(19, 4)));
    auto fifth_unit = MUST(decode_hex(uuid_string_view.substring_view(24, 12)));

    VERIFY(first_unit.size() == 4 && second_unit.size() == 2
        && third_unit.size() == 2 && fourth_unit.size() == 2
        && fifth_unit.size() == 6);

    // Revert endianness for first 4 bytes
    for (size_t index = 0; index < 4; index++) {
        m_uuid_buffer[3 - index] = first_unit[index];
    }

    // Revert endianness for second 2 bytes and again for 2 bytes
    for (size_t index = 0; index < 2; index++) {
        m_uuid_buffer[3 + 2 - index] = second_unit[index];
        m_uuid_buffer[5 + 2 - index] = third_unit[index];
    }

    m_uuid_buffer.span().overwrite(8, fourth_unit.data(), fourth_unit.size());
    m_uuid_buffer.span().overwrite(10, fifth_unit.data(), fifth_unit.size());
}

UUID::UUID(StringView uuid_string_view, Endianness endianness)
{
    if (endianness == Endianness::Little) {
        convert_string_view_to_little_endian_uuid(uuid_string_view);
        return;
    } else if (endianness == Endianness::Mixed) {
        convert_string_view_to_mixed_endian_uuid(uuid_string_view);
        return;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<String> UUID::to_string() const
{
    auto buffer_span = m_uuid_buffer.span();
    StringBuilder builder(36);
    TRY(builder.try_append(encode_hex(buffer_span.trim(4))));
    TRY(builder.try_append('-'));
    TRY(builder.try_append(encode_hex(buffer_span.slice(4, 2))));
    TRY(builder.try_append('-'));
    TRY(builder.try_append(encode_hex(buffer_span.slice(6, 2))));
    TRY(builder.try_append('-'));
    TRY(builder.try_append(encode_hex(buffer_span.slice(8, 2))));
    TRY(builder.try_append('-'));
    TRY(builder.try_append(encode_hex(buffer_span.slice(10, 6))));
    return builder.to_string();
}

bool UUID::is_zero() const
{
    return all_of(m_uuid_buffer, [](auto const octet) { return octet == 0; });
}

}
