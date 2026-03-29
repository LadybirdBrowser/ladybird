/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IntegralMath.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibMedia/MediaStream.h>

#include "Streamer.h"

namespace Media::Matroska {

Streamer::Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor)
    : m_stream_cursor(stream_cursor)
{
}

Streamer::~Streamer() = default;

DecoderErrorOr<String> Streamer::read_string()
{
    auto string_length = TRY(read_variable_size_integer());
    auto string_data = TRY(read_raw_octets(string_length));
    auto const* string_data_raw = reinterpret_cast<char const*>(string_data.data());
    auto string_value = String::from_utf8(ReadonlyBytes(string_data.data(), strnlen(string_data_raw, string_length)));
    if (string_value.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "String is not valid UTF-8");
    return string_value.release_value();
}

DecoderErrorOr<u8> Streamer::read_octet()
{
    u8 result;
    Bytes bytes { &result, 1 };
    TRY(m_stream_cursor->read_into(bytes));
    return bytes[0];
}

DecoderErrorOr<i16> Streamer::read_i16()
{
    return (TRY(read_octet()) << 8) | TRY(read_octet());
}

DecoderErrorOr<u32> Streamer::read_element_id()
{
    auto length_byte = TRY(read_octet());
    auto length = count_leading_zeroes_safe(length_byte) + 1;
    if (length > 4)
        return DecoderError::corrupted("Element ID must be 4 bytes or less"sv);
    u32 result = length_byte;
    auto bytes_left = length;
    while (--bytes_left > 0) {
        result <<= 8;
        result |= TRY(read_octet());
    }

    return result;
}

DecoderErrorOr<Optional<size_t>> Streamer::read_element_size()
{
    auto length_byte = TRY(read_octet());
    auto length = count_leading_zeroes_safe(length_byte) + 1;
    if (length > 8)
        return DecoderError::corrupted("Element Data Size 8 bytes or less"sv);

    size_t result = length_byte;
    auto bytes_left = length;
    while (--bytes_left > 0) {
        result <<= 8;
        result |= TRY(read_octet());
    }

    auto mask = 0xFFFFFFFFFFFFFFFF >> (64 - (7 * length));
    result &= mask;

    if (result == mask)
        return OptionalNone();

    return result;
}

DecoderErrorOr<u64> Streamer::read_variable_size_integer()
{
    auto length_byte = TRY(read_octet());
    auto length = count_leading_zeroes_safe(length_byte) + 1;
    if (length > 8)
        return DecoderError::corrupted("VINT is too large"sv);

    auto bytes_left = length;
    auto result = length_byte & (0xFF >> length);
    while (--bytes_left > 0) {
        result <<= 8;
        result |= TRY(read_octet());
    }
    return result;
}

DecoderErrorOr<i64> Streamer::read_variable_size_signed_integer()
{
    auto length_byte = TRY(read_octet());
    auto length = count_leading_zeroes_safe(length_byte) + 1;
    if (length > 8)
        return DecoderError::corrupted("VINT is too large"sv);

    auto bytes_left = length;
    i64 result = length_byte & (0xFF >> length);
    while (--bytes_left > 0) {
        result <<= 8;
        result |= TRY(read_octet());
    }
    result -= AK::exp2<i64>((length * 7) - 1) - 1;
    return result;
}

DecoderErrorOr<ByteBuffer> Streamer::read_raw_octets(size_t num_octets)
{
    auto result = MUST(ByteBuffer::create_uninitialized(num_octets));
    auto bytes = result.bytes();
    TRY(m_stream_cursor->read_into(bytes));
    return result;
}

DecoderErrorOr<u64> Streamer::read_u64()
{
    auto integer_length = TRY(read_variable_size_integer());
    if (integer_length == 0)
        return 0;
    if (integer_length > 8)
        return DecoderError::corrupted("Integer Element is too large"sv);
    u64 result = 0;
    for (size_t i = 0; i < integer_length; i++) {
        result <<= 8;
        result |= TRY(read_octet());
    }
    return result;
}

DecoderErrorOr<i64> Streamer::read_i64()
{
    auto integer_length = TRY(read_variable_size_integer());
    if (integer_length == 0)
        return 0;
    if (integer_length > 8)
        return DecoderError::corrupted("Signed Integer Element is too large"sv);
    i64 result = 0;
    for (size_t i = 0; i < integer_length; i++) {
        result <<= 8;
        result |= TRY(read_octet());
    }
    auto shift = 64 - (static_cast<i64>(integer_length) * 8);
    result <<= shift;
    result >>= shift;
    return result;
}

DecoderErrorOr<double> Streamer::read_float()
{
    auto length = TRY(read_variable_size_integer());
    if (length == 0)
        return 0;
    if (length != 4u && length != 8u)
        return DecoderError::format(DecoderErrorCategory::Invalid, "Float size must be 4 or 8 bytes");

    union {
        u64 value;
        float float_value;
        double double_value;
    } read_data;
    read_data.value = 0;
    for (size_t i = 0; i < length; i++) {
        read_data.value = (read_data.value << 8u) + TRY(read_octet());
    }
    if (length == 4u)
        return read_data.float_value;
    return read_data.double_value;
}

DecoderErrorOr<void> Streamer::read_unknown_element()
{
    auto element_length = TRY(read_variable_size_integer());
    dbgln_if(MATROSKA_TRACE_DEBUG, "Skipping unknown element of size {}.", element_length);
    TRY(m_stream_cursor->seek(element_length, AK::SeekMode::FromCurrentPosition));
    return {};
}

size_t Streamer::position() const
{
    return m_stream_cursor->position();
}

DecoderErrorOr<void> Streamer::seek_to_position(size_t position)
{
    return m_stream_cursor->seek(position, AK::SeekMode::SetPosition);
}

}
