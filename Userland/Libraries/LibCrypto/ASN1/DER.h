/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitmapView.h>
#include <AK/Result.h>
#include <AK/Types.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto::ASN1 {

class BitStringView {
public:
    BitStringView(ReadonlyBytes data, size_t unused_bits)
        : m_data(data)
        , m_unused_bits(unused_bits)
    {
    }

    ErrorOr<ReadonlyBytes> raw_bytes() const
    {
        if (m_unused_bits != 0)
            return Error::from_string_literal("ASN1::Decoder: BitStringView contains unexpected partial bytes");
        return m_data;
    }

    bool get(size_t index) const
    {
        if (index >= 8 * m_data.size() - m_unused_bits)
            return false;
        return 0 != (m_data[index / 8] & (1u << (7 - (index % 8))));
    }

    size_t unused_bits() const { return m_unused_bits; }
    size_t byte_length() const { return m_data.size(); }

    ReadonlyBytes underlying_bytes() const { return m_data; }

    // FIXME: Improve me! I am naive!
    bool operator==(BitStringView const& other) const
    {
        for (size_t bit_index = 0; bit_index < 8 * m_data.size() - m_unused_bits; ++bit_index) {
            if (get(bit_index) != other.get(bit_index))
                return false;
        }
        return true;
    }

private:
    ReadonlyBytes m_data;
    size_t m_unused_bits;
};

class Decoder {
public:
    Decoder(ReadonlyBytes data)
    {
        m_stack.append(data);
    }

    // Read a tag without consuming it (and its data).
    ErrorOr<Tag> peek();

    bool eof() const;

    template<typename ValueType>
    struct TaggedValue {
        Tag tag;
        ValueType value;
    };

    ErrorOr<void> rewrite_tag(Kind kind)
    {
        if (m_stack.is_empty())
            return Error::from_string_literal("Nothing on stack to rewrite");

        if (eof())
            return Error::from_string_literal("Stream is empty");

        if (m_current_tag.has_value()) {
            m_current_tag->kind = kind;
            return {};
        }

        auto tag = TRY(read_tag());
        m_current_tag = tag;
        m_current_tag->kind = kind;
        return {};
    }

    ErrorOr<void> drop()
    {
        if (m_stack.is_empty())
            return Error::from_string_literal("ASN1::Decoder: Trying to drop using an empty stack");

        if (eof())
            return Error::from_string_literal("ASN1::Decoder: Trying to drop using a decoder that is EOF");

        auto previous_position = m_stack;

        auto tag_or_error = peek();
        if (tag_or_error.is_error()) {
            m_stack = move(previous_position);
            return tag_or_error.release_error();
        }

        auto length_or_error = read_length();
        if (length_or_error.is_error()) {
            m_stack = move(previous_position);
            return length_or_error.release_error();
        }

        auto length = length_or_error.value();

        auto bytes_result = read_bytes(length);
        if (bytes_result.is_error()) {
            m_stack = move(previous_position);
            return bytes_result.release_error();
        }

        m_current_tag.clear();
        return {};
    }

    template<typename ValueType>
    ErrorOr<ValueType> read(Optional<Class> class_override = {}, Optional<Kind> kind_override = {})
    {
        if (m_stack.is_empty())
            return Error::from_string_literal("ASN1::Decoder: Trying to read using an empty stack");

        if (eof())
            return Error::from_string_literal("ASN1::Decoder: Trying to read using a decoder that is EOF");

        auto previous_position = m_stack;

        auto tag_or_error = peek();
        if (tag_or_error.is_error()) {
            m_stack = move(previous_position);
            return tag_or_error.release_error();
        }

        auto length_or_error = read_length();
        if (length_or_error.is_error()) {
            m_stack = move(previous_position);
            return length_or_error.release_error();
        }

        auto tag = tag_or_error.value();
        auto length = length_or_error.value();

        auto value_or_error = read_value<ValueType>(class_override.value_or(tag.class_), kind_override.value_or(tag.kind), length);
        if (value_or_error.is_error()) {
            m_stack = move(previous_position);
            return value_or_error.release_error();
        }

        m_current_tag.clear();

        return value_or_error.release_value();
    }

    ErrorOr<void> enter();
    ErrorOr<void> leave();

    ErrorOr<ReadonlyBytes> peek_entry_bytes();

private:
    template<typename ValueType, typename DecodedType>
    ErrorOr<ValueType> with_type_check(DecodedType&& value)
    {
        if constexpr (requires { ValueType { value }; })
            return ValueType { value };

        return Error::from_string_literal("ASN1::Decoder: Trying to decode a value from an incompatible type");
    }

    template<typename ValueType, typename DecodedType>
    ErrorOr<ValueType> with_type_check(ErrorOr<DecodedType>&& value_or_error)
    {
        if (value_or_error.is_error())
            return value_or_error.release_error();

        if constexpr (IsSame<ValueType, bool> && !IsSame<DecodedType, bool>) {
            return Error::from_string_literal("ASN1::Decoder: Trying to decode a boolean from a non-boolean type");
        } else {
            auto&& value = value_or_error.value();
            if constexpr (requires { ValueType { value }; })
                return ValueType { value };
        }

        return Error::from_string_literal("ASN1::Decoder: Trying to decode a value from an incompatible type");
    }

    template<typename ValueType>
    ErrorOr<ValueType> read_value(Class klass, Kind kind, size_t length)
    {
        auto data = TRY(read_bytes(length));

        if constexpr (IsSame<ValueType, ReadonlyBytes>) {
            return data;
        } else {
            if (klass != Class::Universal)
                return with_type_check<ValueType>(data);

            if (kind == Kind::Boolean)
                return with_type_check<ValueType>(decode_boolean(data));

            if (kind == Kind::Integer)
                return with_type_check<ValueType>(decode_arbitrary_sized_integer(data));

            if (kind == Kind::OctetString)
                return with_type_check<ValueType>(decode_octet_string(data));

            if (kind == Kind::Null)
                return with_type_check<ValueType>(decode_null(data));

            if (kind == Kind::ObjectIdentifier)
                return with_type_check<ValueType>(decode_object_identifier(data));

            if (kind == Kind::PrintableString || kind == Kind::IA5String || kind == Kind::UTCTime)
                return with_type_check<ValueType>(decode_printable_string(data));

            if (kind == Kind::Utf8String)
                return with_type_check<ValueType>(StringView { data.data(), data.size() });

            if (kind == Kind::BitString)
                return with_type_check<ValueType>(decode_bit_string(data));

            return with_type_check<ValueType>(data);
        }
    }

    ErrorOr<Tag> read_tag();
    ErrorOr<size_t> read_length();
    ErrorOr<u8> read_byte();
    ErrorOr<ReadonlyBytes> read_bytes(size_t length);

    static ErrorOr<bool> decode_boolean(ReadonlyBytes);
    static ErrorOr<UnsignedBigInteger> decode_arbitrary_sized_integer(ReadonlyBytes);
    static ErrorOr<StringView> decode_octet_string(ReadonlyBytes);
    static ErrorOr<nullptr_t> decode_null(ReadonlyBytes);
    static ErrorOr<Vector<int>> decode_object_identifier(ReadonlyBytes);
    static ErrorOr<StringView> decode_printable_string(ReadonlyBytes);
    static ErrorOr<BitStringView> decode_bit_string(ReadonlyBytes);

    Vector<ReadonlyBytes> m_stack;
    Optional<Tag> m_current_tag;
};

ErrorOr<void> pretty_print(Decoder&, Stream&, int indent = 0);

class Encoder {
public:
    Encoder()
    {
        m_buffer_stack.empend();
    }

    ReadonlyBytes active_bytes() const { return m_buffer_stack.last().bytes(); }
    ByteBuffer finish()
    {
        VERIFY(m_buffer_stack.size() == 1);
        return m_buffer_stack.take_last();
    }

    template<typename ValueType>
    ErrorOr<void> write(ValueType const& value, Optional<Class> class_override = {}, Optional<Kind> kind_override = {})
    {
        if constexpr (IsSame<ValueType, bool>) {
            return write_boolean(value, class_override, kind_override);
        } else if constexpr (IsSame<ValueType, UnsignedBigInteger> || (IsIntegral<ValueType> && IsUnsigned<ValueType>)) {
            return write_arbitrary_sized_integer(value, class_override, kind_override);
        } else if constexpr (IsOneOf<ValueType, StringView, String, ByteString>) {
            return write_printable_string(value, class_override, kind_override);
        } else if constexpr (IsOneOf<ValueType, ReadonlyBytes, ByteBuffer>) {
            return write_octet_string(value, class_override, kind_override);
        } else if constexpr (IsSame<ValueType, nullptr_t>) {
            return write_null(class_override, kind_override);
        } else if constexpr (IsOneOf<ValueType, Vector<int>, Span<int const>, Span<int>>) {
            return write_object_identifier(value, class_override, kind_override);
        } else if constexpr (IsSame<ValueType, BitStringView>) {
            return write_bit_string(value, class_override, kind_override);
        } else {
            dbgln("Unsupported type: {}", __PRETTY_FUNCTION__);
            return Error::from_string_literal("ASN1::Encoder: Trying to encode a value of an unsupported type");
        }
    }

    template<typename Fn>
    ErrorOr<void> write_constructed(Class class_, Kind kind, Fn&& fn)
    {
        return write_constructed(bit_cast<u8>(class_), bit_cast<u8>(kind), forward<Fn>(fn));
    }

    template<typename Fn>
    ErrorOr<void> write_constructed(u8 class_, u8 kind, Fn&& fn)
    {
        m_buffer_stack.empend();
        using ResultType = decltype(fn());
        if constexpr (IsSpecializationOf<ResultType, ErrorOr>) {
            TRY(fn());
        } else {
            fn();
        }
        auto buffer = m_buffer_stack.take_last();

        TRY(write_tag(bit_cast<Class>(class_), Type::Constructed, bit_cast<Kind>(kind)));
        TRY(write_length(buffer.size()));
        TRY(write_bytes(buffer.bytes()));

        return {};
    }

private:
    ErrorOr<void> write_tag(Class, Type, Kind);
    ErrorOr<void> write_length(size_t);
    ErrorOr<void> write_bytes(ReadonlyBytes);
    ErrorOr<void> write_byte(u8);

    ErrorOr<void> write_boolean(bool, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_arbitrary_sized_integer(UnsignedBigInteger const&, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_printable_string(StringView, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_octet_string(ReadonlyBytes, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_null(Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_object_identifier(Span<int const>, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_bit_string(BitStringView, Optional<Class>, Optional<Kind>);

    Vector<ByteBuffer> m_buffer_stack;
};

}
