/*
 * Copyright (c) 2023, Michiel Visser <opensource@webmichiel.nl>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/OpenSSL.h>

namespace {
// Used by ASN1 macros
static String s_error_string;
}

namespace Crypto::Curves {

struct SECPxxxr1Point {
    UnsignedBigInteger x;
    UnsignedBigInteger y;
    size_t size;

    static ErrorOr<ByteBuffer> scalar_to_bytes(UnsignedBigInteger const& a, size_t size)
    {
        auto a_bytes = TRY(ByteBuffer::create_uninitialized(a.byte_length()));
        auto a_size = a.export_data(a_bytes.span());

        if (a_size >= size) {
            for (size_t i = 0; i < a_size - size; i++) {
                if (a_bytes[i] != 0) {
                    return Error::from_string_literal("Scalar is too large for the given size");
                }
            }

            return a_bytes.slice(a_size - size, size);
        }

        auto a_extended_bytes = TRY(ByteBuffer::create_zeroed(size));
        a_extended_bytes.overwrite(size - a_size, a_bytes.data(), a_size);
        return a_extended_bytes;
    }

    static ErrorOr<SECPxxxr1Point> from_uncompressed(ReadonlyBytes data)
    {
        if (data.size() < 1 || data[0] != 0x04)
            return Error::from_string_literal("Invalid length or not an uncompressed SECPxxxr1 point");

        auto half_size = (data.size() - 1) / 2;
        return SECPxxxr1Point {
            UnsignedBigInteger::import_data(data.slice(1, half_size)),
            UnsignedBigInteger::import_data(data.slice(1 + half_size, half_size)),
            half_size,
        };
    }

    ErrorOr<ByteBuffer> x_bytes() const
    {
        return scalar_to_bytes(x, size);
    }

    ErrorOr<ByteBuffer> y_bytes() const
    {
        return scalar_to_bytes(y, size);
    }

    ErrorOr<ByteBuffer> to_uncompressed() const
    {
        auto x = TRY(x_bytes());
        auto y = TRY(y_bytes());

        auto bytes = TRY(ByteBuffer::create_uninitialized(1 + (size * 2)));
        bytes[0] = 0x04; // uncompressed
        bytes.overwrite(1, x.data(), size);
        bytes.overwrite(1 + size, y.data(), size);
        return bytes;
    }
};

struct SECPxxxr1Signature {
    UnsignedBigInteger r;
    UnsignedBigInteger s;
    size_t size;

    static ErrorOr<SECPxxxr1Signature> from_asn(Span<int const> curve_oid, ReadonlyBytes signature, Vector<StringView> current_scope)
    {
        ASN1::Decoder decoder(signature);
        ENTER_TYPED_SCOPE(Sequence, "SECPxxxr1Signature");
        READ_OBJECT(Integer, UnsignedBigInteger, r_big_int);
        READ_OBJECT(Integer, UnsignedBigInteger, s_big_int);

        size_t scalar_size;
        if (curve_oid == ASN1::secp256r1_oid) {
            scalar_size = ceil_div(256, 8);
        } else if (curve_oid == ASN1::secp384r1_oid) {
            scalar_size = ceil_div(384, 8);
        } else if (curve_oid == ASN1::secp521r1_oid) {
            scalar_size = ceil_div(521, 8);
        } else {
            return Error::from_string_literal("Unknown SECPxxxr1 curve");
        }

        if (r_big_int.byte_length() < scalar_size || s_big_int.byte_length() < scalar_size)
            return Error::from_string_literal("Invalid SECPxxxr1 signature");

        return SECPxxxr1Signature { r_big_int, s_big_int, scalar_size };
    }

    ErrorOr<ByteBuffer> r_bytes() const
    {
        return SECPxxxr1Point::scalar_to_bytes(r, size);
    }

    ErrorOr<ByteBuffer> s_bytes() const
    {
        return SECPxxxr1Point::scalar_to_bytes(s, size);
    }

    ErrorOr<ByteBuffer> to_asn()
    {
        ASN1::Encoder encoder;
        TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write(r));
            TRY(encoder.write(s));
            return {};
        }));

        return encoder.finish();
    }
};

class SECPxxxr1 {
public:
    ErrorOr<UnsignedBigInteger> generate_private_key();
    ErrorOr<SECPxxxr1Point> generate_public_key(UnsignedBigInteger scalar);
    ErrorOr<SECPxxxr1Point> compute_coordinate(UnsignedBigInteger scalar, SECPxxxr1Point point);
    ErrorOr<bool> verify(ReadonlyBytes hash, SECPxxxr1Point pubkey, SECPxxxr1Signature signature);
    ErrorOr<SECPxxxr1Signature> sign(ReadonlyBytes hash, UnsignedBigInteger private_key);

protected:
    SECPxxxr1(char const* curve_name, size_t scalar_size)
        : m_curve_name(curve_name)
        , m_scalar_size(scalar_size)
    {
    }

private:
    char const* m_curve_name;
    size_t m_scalar_size;
};

class SECP256r1 : public SECPxxxr1 {
public:
    SECP256r1()
        : SECPxxxr1("P-256", 32)
    {
    }
};

class SECP384r1 : public SECPxxxr1 {
public:
    SECP384r1()
        : SECPxxxr1("P-384", 48)
    {
    }
};

class SECP521r1 : public SECPxxxr1 {
public:
    SECP521r1()
        : SECPxxxr1("P-521", 66)
    {
    }
};

}
