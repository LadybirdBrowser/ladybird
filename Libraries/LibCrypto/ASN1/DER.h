/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCrypto/ASN1/ASN1.h>

namespace Crypto::ASN1 {

using ObjectIdentifier = Vector<u32, 12>;

class BitStringView {
public:
    BitStringView(ReadonlyBytes data, size_t unused_bits)
        : m_data(data)
        , m_unused_bits(unused_bits)
    {
    }

    size_t unused_bits() const { return m_unused_bits; }
    size_t byte_length() const { return m_data.size(); }

    ReadonlyBytes underlying_bytes() const { return m_data; }

private:
    ReadonlyBytes m_data;
    size_t m_unused_bits;
};

class Encoder {
public:
    Encoder()
    {
        m_buffer_stack.empend();
    }

    ReadonlyBytes active_bytes() const LIFETIME_BOUND { return m_buffer_stack.last().bytes(); }
    ByteBuffer finish()
    {
        VERIFY(m_buffer_stack.size() == 1);
        return m_buffer_stack.take_last();
    }

    template<typename ValueType>
    ErrorOr<void> write(ValueType const& value, Optional<Class> class_override = {}, Optional<Kind> kind_override = {})
    {
        if constexpr (IsIntegral<ValueType> && IsUnsigned<ValueType> && !IsSame<ValueType, bool>) {
            return write_integer(value, class_override, kind_override);
        } else if constexpr (IsOneOf<ValueType, ReadonlyBytes, ByteBuffer>) {
            return write_octet_string(value, class_override, kind_override);
        } else if constexpr (IsSame<ValueType, nullptr_t>) {
            return write_null(class_override, kind_override);
        } else if constexpr (IsOneOf<ValueType, ObjectIdentifier, Span<u32 const>, Span<u32>>) {
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

    ErrorOr<void> write_integer(u64, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_octet_string(ReadonlyBytes, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_null(Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_object_identifier(Span<u32 const>, Optional<Class>, Optional<Kind>);
    ErrorOr<void> write_bit_string(BitStringView, Optional<Class>, Optional<Kind>);

    Vector<ByteBuffer> m_buffer_stack;
};

}
