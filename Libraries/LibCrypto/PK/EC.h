/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/PK.h>

namespace Crypto::PK {

template<typename Integer = UnsignedBigInteger>
class ECPublicKey {
public:
    ECPublicKey(Integer x, Integer y)
        : m_x(move(x))
        , m_y(move(y))
    {
    }

    ECPublicKey()
        : m_x(0)
        , m_y(0)
    {
    }

    Integer const& x() const { return m_x; }
    Integer const& y() const { return m_y; }

    ErrorOr<ByteBuffer> to_uncompressed() const
    {
        auto bytes = TRY(ByteBuffer::create_uninitialized(1 + m_x.byte_length() + m_y.byte_length()));
        bytes[0] = 0x04; // uncompressed
        auto x_size = m_x.export_data(bytes.span().slice(1));
        auto y_size = m_y.export_data(bytes.span().slice(1 + x_size));
        return bytes.slice(0, 1 + x_size + y_size);
    }

private:
    Integer m_x;
    Integer m_y;
};

// https://www.rfc-editor.org/rfc/rfc5915#section-3
template<typename Integer = UnsignedBigInteger>
class ECPrivateKey {
public:
    ECPrivateKey(Integer d, Optional<Vector<int>> parameters, Optional<ECPublicKey<Integer>> public_key)
        : m_d(move(d))
        , m_parameters(parameters)
        , m_public_key(public_key)
    {
    }

    ECPrivateKey() = default;

    Integer const& d() const { return m_d; }
    Optional<Vector<int> const&> parameters() const { return m_parameters; }
    Optional<ECPublicKey<Integer> const&> public_key() const { return m_public_key; }

    ErrorOr<ByteBuffer> export_as_der() const;

private:
    Integer m_d;
    Optional<Vector<int>> m_parameters;
    Optional<ECPublicKey<Integer>> m_public_key;
};

template<typename PubKey, typename PrivKey>
struct ECKeyPair {
    PubKey public_key;
    PrivKey private_key;
};

using IntegerType = UnsignedBigInteger;
class EC : public PKSystem<ECPrivateKey<IntegerType>, ECPublicKey<IntegerType>> {
public:
    using KeyPairType = ECKeyPair<PublicKeyType, PrivateKeyType>;

    static ErrorOr<KeyPairType> parse_ec_key(ReadonlyBytes der);
};

}
