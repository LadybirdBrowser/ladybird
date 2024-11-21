/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IntegralMath.h>
#include <AK/Math.h>
#include <AK/Stream.h>
#include <AK/Try.h>
#include <AK/Utf8View.h>
#include <LibCrypto/ASN1/DER.h>

namespace Crypto::ASN1 {

ErrorOr<Tag> Decoder::read_tag()
{
    auto byte = TRY(read_byte());
    u8 class_ = byte & 0xc0;
    u8 type = byte & 0x20;
    u8 kind = byte & 0x1f;

    if (kind == 0x1f) {
        kind = 0;
        while (byte & 0x80) {
            byte = TRY(read_byte());
            kind = (kind << 7) | (byte & 0x7f);
        }
    }

    return Tag { (Kind)kind, (Class)class_, (Type)type };
}

ErrorOr<size_t> Decoder::read_length()
{
    auto byte = TRY(read_byte());
    size_t length = byte;

    if (byte & 0x80) {
        auto count = byte & 0x7f;
        if (count == 0x7f)
            return Error::from_string_literal("ASN1::Decoder: Length has an invalid count value");

        auto data = TRY(read_bytes(count));
        length = 0;

        if (data.size() > sizeof(size_t))
            return Error::from_string_literal("ASN1::Decoder: Length is larger than the target type");

        for (auto&& byte : data)
            length = (length << 8) | (size_t)byte;
    }

    return length;
}

ErrorOr<u8> Decoder::read_byte()
{
    if (m_stack.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Reading byte from an empty stack");

    auto& entry = m_stack.last();
    if (entry.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Reading byte from an empty entry");

    auto byte = entry[0];
    entry = entry.slice(1);

    return byte;
}

ErrorOr<ReadonlyBytes> Decoder::peek_entry_bytes()
{
    if (m_stack.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Reading bytes from an empty stack");

    auto entry = m_stack.last();

    return entry;
}

ErrorOr<ReadonlyBytes> Decoder::read_bytes(size_t length)
{
    if (m_stack.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Reading bytes from an empty stack");

    auto& entry = m_stack.last();
    if (entry.size() < length)
        return Error::from_string_literal("ASN1::Decoder: Reading bytes from an empty entry");

    auto bytes = entry.slice(0, length);
    entry = entry.slice(length);

    return bytes;
}

ErrorOr<bool> Decoder::decode_boolean(ReadonlyBytes data)
{
    if (data.size() != 1)
        return Error::from_string_literal("ASN1::Decoder: Decoding boolean from a non boolean-sized span");

    return data[0] != 0;
}

ErrorOr<UnsignedBigInteger> Decoder::decode_arbitrary_sized_integer(ReadonlyBytes data)
{
    if (data.size() < 1)
        return Error::from_string_literal("ASN1::Decoder: Decoding arbitrary sized integer from an empty span");

    if (data.size() > 1
        && ((data[0] == 0xff && data[1] & 0x80)
            || (data[0] == 0x00 && !(data[1] & 0x80)))) {
        return Error::from_string_literal("ASN1::Decoder: Arbitrary sized integer has an invalid format");
    }

    bool is_negative = data[0] & 0x80;
    if (is_negative)
        return Error::from_string_literal("ASN1::Decoder: Decoding a negative unsigned arbitrary sized integer");

    return UnsignedBigInteger::import_data(data.data(), data.size());
}

ErrorOr<StringView> Decoder::decode_octet_string(ReadonlyBytes bytes)
{
    return StringView { bytes.data(), bytes.size() };
}

ErrorOr<nullptr_t> Decoder::decode_null(ReadonlyBytes data)
{
    if (data.size() != 0)
        return Error::from_string_literal("ASN1::Decoder: Decoding null from a non-empty span");

    return nullptr;
}

ErrorOr<Vector<int>> Decoder::decode_object_identifier(ReadonlyBytes data)
{
    Vector<int> result;
    result.append(0); // Reserved space.

    u32 value = 0;
    for (auto&& byte : data) {
        if (value == 0 && byte == 0x80)
            return Error::from_string_literal("ASN1::Decoder: Invalid first byte in object identifier");

        value = (value << 7) | (byte & 0x7f);
        if (!(byte & 0x80)) {
            result.append(value);
            value = 0;
        }
    }

    if (result.size() == 1 || result[1] >= 1600)
        return Error::from_string_literal("ASN1::Decoder: Invalid encoding in object identifier");

    result[0] = result[1] / 40;
    result[1] = result[1] % 40;

    return result;
}

ErrorOr<StringView> Decoder::decode_printable_string(ReadonlyBytes data)
{
    Utf8View view { data };
    if (!view.validate())
        return Error::from_string_literal("ASN1::Decoder: Invalid UTF-8 in printable string");

    return StringView { data };
}

ErrorOr<BitStringView> Decoder::decode_bit_string(ReadonlyBytes data)
{
    if (data.size() < 1)
        return Error::from_string_literal("ASN1::Decoder: Decoding bit string from empty span");

    auto unused_bits = data[0];
    auto total_size_in_bits = (data.size() - 1) * 8;

    if (unused_bits > total_size_in_bits)
        return Error::from_string_literal("ASN1::Decoder: Number of unused bits is larger than the total size");

    return BitStringView { data.slice(1), unused_bits };
}

ErrorOr<Tag> Decoder::peek()
{
    if (m_stack.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Peeking using an empty stack");

    if (eof())
        return Error::from_string_literal("ASN1::Decoder: Peeking using a decoder that is at EOF");

    if (m_current_tag.has_value())
        return m_current_tag.value();

    m_current_tag = TRY(read_tag());

    return m_current_tag.value();
}

bool Decoder::eof() const
{
    return m_stack.is_empty() || m_stack.last().is_empty();
}

ErrorOr<void> Decoder::enter()
{
    if (m_stack.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Entering using an empty stack");

    auto tag = TRY(peek());
    if (tag.type != Type::Constructed)
        return Error::from_string_literal("ASN1::Decoder: Entering a non-constructed type");

    auto length = TRY(read_length());

    auto data = TRY(read_bytes(length));

    m_current_tag.clear();

    m_stack.append(data);
    return {};
}

ErrorOr<void> Decoder::leave()
{
    if (m_stack.is_empty())
        return Error::from_string_literal("ASN1::Decoder: Leaving using an empty stack");

    if (m_stack.size() == 1)
        return Error::from_string_literal("ASN1::Decoder: Leaving the main context");

    m_stack.take_last();
    m_current_tag.clear();

    return {};
}

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

ErrorOr<void> Encoder::write_boolean(bool value, Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::Boolean);

    TRY(write_tag(class_, type, kind));
    TRY(write_length(1));
    return write_byte(value ? 0xff : 0x00);
}

ErrorOr<void> Encoder::write_arbitrary_sized_integer(UnsignedBigInteger const& value, Optional<Class> class_override, Optional<Kind> kind_override)
{
    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::Integer);
    TRY(write_tag(class_, type, kind));

    auto max_byte_size = max(1ull, value.length() * UnsignedBigInteger::BITS_IN_WORD / 8); // At minimum, we need one byte to encode 0.
    ByteBuffer buffer;
    auto output = TRY(buffer.get_bytes_for_writing(max_byte_size));
    auto size = value.export_data(output);
    // DER does not allow empty integers, encode a zero if the exported size is zero.
    if (size == 0) {
        output[0] = 0;
        size = 1;
    }

    // Chop off the leading zeros
    if constexpr (AK::HostIsLittleEndian) {
        while (size > 1 && output[0] == 0) {
            size--;
            output = output.slice(1);
        }
    } else {
        while (size > 1 && output[size - 1] == 0)
            size--;
    }

    // If the MSB is set, we need to add a leading zero to indicate a positive number.
    if ((output[0] & 0x80) != 0) {
        TRY(write_length(size + 1));
        TRY(write_byte(0));
    } else {
        TRY(write_length(size));
    }
    return write_bytes(output.slice(0, size));
}

ErrorOr<void> Encoder::write_printable_string(StringView string, Optional<Class> class_override, Optional<Kind> kind_override)
{
    Utf8View view { string };
    if (!view.validate())
        return Error::from_string_literal("ASN1::Encoder: Invalid UTF-8 in printable string");

    auto class_ = class_override.value_or(Class::Universal);
    auto type = Type::Primitive;
    auto kind = kind_override.value_or(Kind::PrintableString);

    TRY(write_tag(class_, type, kind));
    TRY(write_length(string.length()));
    return write_bytes(string.bytes());
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

ErrorOr<void> Encoder::write_object_identifier(Span<int const> segments, Optional<Class> class_override, Optional<Kind> kind_override)
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
        if (segment < 0)
            return Error::from_string_literal("ASN1::Encoder: Object identifier segments must be non-negative");

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

ErrorOr<void> pretty_print(Decoder& decoder, Stream& stream, int indent)
{
    while (!decoder.eof()) {
        auto tag = TRY(decoder.peek());

        StringBuilder builder;
        for (int i = 0; i < indent; ++i)
            builder.append(' ');
        builder.appendff("<{}> ", class_name(tag.class_));
        if (tag.type == Type::Constructed) {
            builder.appendff("[{}] {} ({})", type_name(tag.type), to_underlying(tag.kind), kind_name(tag.kind));
            TRY(decoder.enter());

            builder.append('\n');
            TRY(stream.write_until_depleted(builder.string_view().bytes()));

            TRY(pretty_print(decoder, stream, indent + 2));

            TRY(decoder.leave());

            continue;
        } else {
            if (tag.class_ != Class::Universal)
                builder.appendff("[{}] {} {}", type_name(tag.type), to_underlying(tag.kind), kind_name(tag.kind));
            else
                builder.appendff("[{}] {}", type_name(tag.type), kind_name(tag.kind));
            switch (tag.kind) {
            case Kind::Eol: {
                TRY(decoder.read<ReadonlyBytes>());
                break;
            }
            case Kind::Boolean: {
                auto value = TRY(decoder.read<bool>());
                builder.appendff(" {}", value);
                break;
            }
            case Kind::Integer: {
                auto value = TRY(decoder.read<ReadonlyBytes>());
                builder.append(" 0x"sv);
                for (auto ch : value)
                    builder.appendff("{:0>2x}", ch);
                break;
            }
            case Kind::BitString: {
                auto value = TRY(decoder.read<BitmapView>());
                builder.append(" 0b"sv);
                for (size_t i = 0; i < value.size(); ++i)
                    builder.append(value.get(i) ? '1' : '0');
                break;
            }
            case Kind::OctetString: {
                auto value = TRY(decoder.read<StringView>());
                builder.append(" 0x"sv);
                for (auto ch : value)
                    builder.appendff("{:0>2x}", ch);
                break;
            }
            case Kind::Null: {
                TRY(decoder.read<decltype(nullptr)>());
                break;
            }
            case Kind::ObjectIdentifier: {
                auto value = TRY(decoder.read<Vector<int>>());
                for (auto& id : value)
                    builder.appendff(" {}", id);
                break;
            }
            case Kind::UTCTime:
            case Kind::GeneralizedTime:
            case Kind::IA5String:
            case Kind::VisibleString:
            case Kind::BMPString:
            case Kind::PrintableString: {
                auto value = TRY(decoder.read<StringView>());
                builder.append(' ');
                builder.append(value);
                break;
            }
            case Kind::Utf8String: {
                auto value = TRY(decoder.read<Utf8View>());
                builder.append(' ');
                for (auto cp : value)
                    builder.append_code_point(cp);
                break;
            }
            case Kind::Sequence:
            case Kind::Set:
                return Error::from_string_literal("ASN1::Decoder: Unexpected Primitive");
            default: {
                dbgln("PrettyPrint error: Unhandled kind {}", to_underlying(tag.kind));
            }
            }
        }

        builder.append('\n');
        TRY(stream.write_until_depleted(builder.string_view().bytes()));
    }

    return {};
}

}
