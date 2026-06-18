/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <AK/Stream.h>
#include <AK/Types.h>

namespace AK {

template<typename ValueType>
class [[gnu::packed]] LEB128 {
public:
    constexpr LEB128() = default;

    constexpr LEB128(ValueType value)
        : m_value(value)
    {
    }

    constexpr operator ValueType() const { return m_value; }

    static ErrorOr<LEB128<ValueType>> read_from_stream(Stream& stream)
    requires(Unsigned<ValueType>)
    {
        // First byte is unrolled for speed
        u8 byte;
        do {
            auto result = stream.read_some({ &byte, 1 });
            if (result.is_error() && result.error().is_errno() && (result.error().code() == EAGAIN || result.error().code() == EINTR))
                continue;
            if (result.is_error())
                return result.release_error();
            if (result.value().size() != 1)
                return Error::from_string_literal("EOF reached before expected end");
            break;
        } while (true);

        if ((byte & 0x80) == 0)
            return LEB128<ValueType> { byte };

        ValueType result = byte & 0x7F;
        size_t num_bytes = 1;
        while (true) {
            auto byte = TRY(stream.read_value<u8>());
            ValueType masked = byte & 0x7F;
            result |= masked << (num_bytes * 7);
            if (num_bytes * 7 >= sizeof(ValueType) * 8 - 7 && (byte >> (sizeof(ValueType) * 8 - (num_bytes * 7))) != 0) {
                if ((byte & 0x80) != 0)
                    return Error::from_string_literal("Read value contains more bits than fit the chosen ValueType");
                return Error::from_string_literal("Read byte is too large to fit the chosen ValueType");
            }
            ++num_bytes;
            if ((byte & 0x80) == 0)
                return LEB128<ValueType> { result };
        }
    }

    static ErrorOr<LEB128<ValueType>> read_from_stream(Stream& stream)
    requires(Signed<ValueType>)
    {
        constexpr auto BITS = sizeof(ValueType) * 8;

        ValueType result = 0;
        u32 shift = 0;
        u8 byte = 0;

        do {
            if (stream.is_eof())
                return Error::from_string_literal("Stream reached end-of-file while reading LEB128 value");
            byte = TRY(stream.read_value<u8>());
            result |= (ValueType)(byte & 0x7F) << shift;

            if (shift >= BITS - 7) {
                bool has_continuation = (byte & 0x80);
                ValueType sign_and_unused = (i8)(byte << 1) >> (BITS - shift);
                if (has_continuation)
                    return Error::from_string_literal("Read value contains more bits than fit the chosen ValueType");
                if (sign_and_unused != 0 && sign_and_unused != -1)
                    return Error::from_string_literal("Read byte is too large to fit the chosen ValueType");
                return LEB128<ValueType> { result };
            }

            shift += 7;
        } while (byte & 0x80);

        // Sign extend
        if (shift < BITS && (byte & 0x40))
            result |= ((ValueType)~0 << shift);

        return LEB128<ValueType> { result };
    }

    ErrorOr<void> write_to_stream(Stream& stream) const
    {
        ErrorOr<void> result;
        for_each_leb128_byte(m_value, [&](u8 byte) {
            if (!result.is_error())
                result = stream.write_value(byte);
        });
        return result;
    }

    template<typename Buffer>
    static void append_to(Buffer& output, ValueType value)
    {
        for_each_leb128_byte(value, [&output](u8 byte) { output.append(byte); });
    }

private:
    template<typename Callback>
    static void for_each_leb128_byte(ValueType value, Callback&& callback)
    requires(Unsigned<ValueType>)
    {
        do {
            u8 byte = value & 0x7F;
            value >>= 7;
            if (value != 0)
                byte |= 0x80; // More bytes follow.
            callback(byte);
        } while (value != 0);
    }

    template<typename Callback>
    static void for_each_leb128_byte(ValueType value, Callback&& callback)
    requires(Signed<ValueType>)
    {
        while (true) {
            u8 byte = value & 0x7F;
            value >>= 7; // Arithmetic shift; preserves the sign.
            bool const sign_bit_set = (byte & 0x40) != 0;
            if ((value == 0 && !sign_bit_set) || (value == -1 && sign_bit_set)) {
                callback(byte);
                return;
            }
            callback(static_cast<u8>(byte | 0x80));
        }
    }

    ValueType m_value { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::LEB128;
#endif
