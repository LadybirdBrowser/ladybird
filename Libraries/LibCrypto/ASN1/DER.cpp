/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IntegralMath.h>
#include <AK/Math.h>
#include <AK/Try.h>
#include <LibCrypto/ASN1/DER.h>

namespace Crypto::ASN1 {

ErrorOr<void> Encoder::write_tag(Class class_, Type type, Kind kind)
{
    auto class_byte = to_underlying(class_);
    auto type_byte = to_underlying(type);
    auto kind_byte = to_underlying(kind);

    auto byte = class_byte | type_byte | kind_byte;
    if (kind_byte > 0x1f) {
        auto high = kind_byte >> 7;
        byte = class_byte | type_byte | 0x1f;
        TRY(write_byte(byte));
        byte = (kind_byte & 0x7f) | high;
    }

    return write_byte(byte);
}

ErrorOr<void> Encoder::write_byte(u8 byte)
{
    return write_bytes({ &byte, 1 });
}

ErrorOr<void> Encoder::write_length(size_t value)
{
    if (value < 0x80)
        return write_byte(value);

    double minimum_bits = AK::log2(value);
    size_t size_in_bits = AK::floor(minimum_bits) + 1;
    size_t size = ceil_div(size_in_bits, 8uz);
    TRY(write_byte(0x80 | size));

    for (size_t i = 0; i < size; i++) {
        auto shift = (size - i - 1) * 8;
        auto byte = (value >> shift) & 0xff;
        TRY(write_byte(byte));
    }

    return {};
}

ErrorOr<void> Encoder::write_bytes(ReadonlyBytes bytes)
{
    auto output = TRY(m_buffer_stack.last().get_bytes_for_writing(bytes.size()));
    bytes.copy_to(output);
    return {};
}

ErrorOr<void> Encoder::write_integer(u64 value, Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::Integer);
    TRY(write_tag(class_, type, kind));

    // NB: The contents are the fewest big-endian bytes that hold the value, at least one, after a zero byte if the top
    //     bit is set, which keeps the integer positive.
    u8 bytes[sizeof(u64) + 1] {};
    for (size_t i = 0; i < sizeof(u64); ++i)
        bytes[sizeof(bytes) - 1 - i] = static_cast<u8>(value >> (8 * i));
    size_t start = 1;
    while (start < sizeof(bytes) - 1 && bytes[start] == 0)
        ++start;
    if ((bytes[start] & 0x80) != 0)
        --start;

    ReadonlyBytes contents { bytes + start, sizeof(bytes) - start };
    TRY(write_length(contents.size()));
    return write_bytes(contents);
}

ErrorOr<void> Encoder::write_octet_string(ReadonlyBytes bytes, Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::OctetString);

    TRY(write_tag(class_, type, kind));
    TRY(write_length(bytes.size()));
    return write_bytes(bytes);
}

ErrorOr<void> Encoder::write_null(Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::Null);

    TRY(write_tag(class_, type, kind));
    TRY(write_length(0));
    return {};
}

ErrorOr<void> Encoder::write_object_identifier(Span<u32 const> segments, Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::ObjectIdentifier);

    if (segments.size() < 2)
        return Error::from_string_literal("ASN1::Encoder: Object identifier must have at least two segments");

    TRY(write_tag(class_, type, kind));
    size_t length = 1;
    for (size_t i = 2; i < segments.size(); i++) {
        auto segment = segments[i];
        if (segment < 0x80)
            length += 1;
        else if (segment < 0x4000)
            length += 2;
        else if (segment < 0x200000)
            length += 3;
        else
            length += 4;
    }

    TRY(write_length(length));

    auto first_byte = (segments[0] * 40) + segments[1];
    TRY(write_byte(first_byte));

    for (size_t i = 2; i < segments.size(); i++) {
        auto segment = segments[i];
        if (segment < 0x80) {
            TRY(write_byte(segment));
        } else if (segment < 0x4000) {
            TRY(write_byte((segment >> 7) | 0x80));
            TRY(write_byte(segment & 0x7f));
        } else if (segment < 0x200000) {
            TRY(write_byte((segment >> 14) | 0x80));
            TRY(write_byte(((segment >> 7) & 0x7f) | 0x80));
            TRY(write_byte(segment & 0x7f));
        } else {
            TRY(write_byte((segment >> 21) | 0x80));
            TRY(write_byte(((segment >> 14) & 0x7f) | 0x80));
            TRY(write_byte(((segment >> 7) & 0x7f) | 0x80));
            TRY(write_byte(segment & 0x7f));
        }
    }

    return {};
}

ErrorOr<void> Encoder::write_bit_string(BitStringView view, Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::BitString);

    auto unused_bits = view.unused_bits();
    auto total_size_in_bits = view.byte_length() * 8 - unused_bits;

    TRY(write_tag(class_, type, kind));
    TRY(write_length(ceil_div(total_size_in_bits, 8uz) + 1));
    TRY(write_byte(unused_bits));
    return write_bytes(view.underlying_bytes());
}

}
