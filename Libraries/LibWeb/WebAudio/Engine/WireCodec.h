/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Endian.h>
#include <AK/Error.h>
#include <AK/NumericLimits.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace Web::WebAudio::Render {

class WireEncoder {
public:
    ErrorOr<void> append_u8(u8 value)
    {
        return m_buffer.try_append(value);
    }

    ErrorOr<void> append_u16(u16 value)
    {
        u16 le = AK::convert_between_host_and_little_endian(value);
        return m_buffer.try_append(&le, sizeof(le));
    }

    ErrorOr<void> append_u32(u32 value)
    {
        u32 le = AK::convert_between_host_and_little_endian(value);
        return m_buffer.try_append(&le, sizeof(le));
    }

    ErrorOr<void> append_u64(u64 value)
    {
        u64 le = AK::convert_between_host_and_little_endian(value);
        return m_buffer.try_append(&le, sizeof(le));
    }

    ErrorOr<void> append_f32(f32 value)
    {
        static_assert(sizeof(f32) == sizeof(u32));
        u32 bits;
        __builtin_memcpy(&bits, &value, sizeof(bits));
        return append_u32(bits);
    }

    ErrorOr<void> append_f64(f64 value)
    {
        static_assert(sizeof(f64) == sizeof(u64));
        u64 bits;
        __builtin_memcpy(&bits, &value, sizeof(bits));
        return append_u64(bits);
    }

    ErrorOr<void> append_string(StringView utf8)
    {
        VERIFY(utf8.bytes().size() <= 0xFFFF'FFFFu);
        TRY(append_u32(static_cast<u32>(utf8.bytes().size())));
        return m_buffer.try_append(utf8.bytes());
    }

    size_t size() const { return m_buffer.size(); }

    void overwrite_u32_at(size_t offset, u32 value)
    {
        u32 le = AK::convert_between_host_and_little_endian(value);
        m_buffer.overwrite(offset, &le, sizeof(le));
    }

    ByteBuffer take() { return move(m_buffer); }

private:
    ByteBuffer m_buffer;
};

class WireDecoder {
public:
    explicit WireDecoder(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    bool at_end() const { return m_offset >= m_bytes.size(); }
    size_t remaining() const { return m_bytes.size() - m_offset; }

    ErrorOr<u8> read_u8()
    {
        if (remaining() < 1)
            return Error::from_string_literal("Wire: truncated u8");
        return m_bytes[m_offset++];
    }

    ErrorOr<u16> read_u16()
    {
        if (remaining() < 2)
            return Error::from_string_literal("Wire: truncated u16");
        u16 le;
        __builtin_memcpy(&le, m_bytes.offset(m_offset), sizeof(le));
        m_offset += sizeof(le);
        return AK::convert_between_host_and_little_endian(le);
    }

    ErrorOr<u32> read_u32()
    {
        if (remaining() < 4)
            return Error::from_string_literal("Wire: truncated u32");
        u32 le;
        __builtin_memcpy(&le, m_bytes.offset(m_offset), sizeof(le));
        m_offset += sizeof(le);
        return AK::convert_between_host_and_little_endian(le);
    }

    ErrorOr<u64> read_u64()
    {
        if (remaining() < 8)
            return Error::from_string_literal("Wire: truncated u64");
        u64 le;
        __builtin_memcpy(&le, m_bytes.offset(m_offset), sizeof(le));
        m_offset += sizeof(le);
        return AK::convert_between_host_and_little_endian(le);
    }

    ErrorOr<f32> read_f32()
    {
        auto bits = TRY(read_u32());
        f32 value;
        __builtin_memcpy(&value, &bits, sizeof(value));
        return value;
    }

    ErrorOr<f64> read_f64()
    {
        auto bits = TRY(read_u64());
        f64 value;
        __builtin_memcpy(&value, &bits, sizeof(value));
        return value;
    }

    ErrorOr<ByteString> read_string()
    {
        auto size = TRY(read_u32());
        if (remaining() < size)
            return Error::from_string_literal("Wire: truncated string");
        auto const* data = reinterpret_cast<char const*>(m_bytes.offset(m_offset));
        auto view = StringView { data, size };
        m_offset += size;
        return view.to_byte_string();
    }

    ErrorOr<ReadonlyBytes> read_bytes(size_t size)
    {
        if (remaining() < size)
            return Error::from_string_literal("Wire: truncated bytes");
        auto out = m_bytes.slice(m_offset, size);
        m_offset += size;
        return out;
    }

    ErrorOr<void> skip(size_t size)
    {
        if (remaining() < size)
            return Error::from_string_literal("Wire: truncated skip");
        m_offset += size;
        return {};
    }

private:
    ReadonlyBytes m_bytes;
    size_t m_offset { 0 };
};

inline ErrorOr<void> append_optional_u64(WireEncoder& encoder, Optional<u64> value)
{
    if (!value.has_value()) {
        TRY(encoder.append_u8(0));
        return {};
    }
    TRY(encoder.append_u8(1));
    return encoder.append_u64(value.value());
}

inline ErrorOr<Optional<u64>> read_optional_u64(WireDecoder& decoder)
{
    auto present = TRY(decoder.read_u8());
    if (!present)
        return Optional<u64> {};
    return Optional<u64> { TRY(decoder.read_u64()) };
}

inline ErrorOr<void> append_optional_f64(WireEncoder& encoder, Optional<f64> value)
{
    if (!value.has_value()) {
        TRY(encoder.append_u8(0));
        return {};
    }
    TRY(encoder.append_u8(1));
    return encoder.append_f64(value.value());
}

inline ErrorOr<Optional<f64>> read_optional_f64(WireDecoder& decoder)
{
    auto present = TRY(decoder.read_u8());
    if (!present)
        return Optional<f64> {};
    return Optional<f64> { TRY(decoder.read_f64()) };
}

inline size_t clamp_u64_to_size(u64 value)
{
    if (value > AK::NumericLimits<size_t>::max())
        return AK::NumericLimits<size_t>::max();
    return static_cast<size_t>(value);
}

inline Optional<size_t> clamp_optional_u64_to_size(Optional<u64> value)
{
    if (!value.has_value())
        return {};
    return clamp_u64_to_size(value.value());
}

inline ErrorOr<void> append_optional_size_as_u64(WireEncoder& encoder, Optional<size_t> value)
{
    if (!value.has_value())
        return append_optional_u64(encoder, Optional<u64> {});
    return append_optional_u64(encoder, Optional<u64> { static_cast<u64>(value.value()) });
}

inline ErrorOr<Optional<size_t>> read_optional_size_from_u64(WireDecoder& decoder)
{
    auto v = TRY(read_optional_u64(decoder));
    return clamp_optional_u64_to_size(v);
}

}
