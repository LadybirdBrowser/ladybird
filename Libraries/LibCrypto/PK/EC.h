/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/PK/PK.h>

namespace Crypto::PK {

template<typename Integer = UnsignedBigInteger>
class ECPublicKey {
public:
    ECPublicKey(Integer x, Integer y, size_t scalar_size)
        : m_x(move(x))
        , m_y(move(y))
        , m_scalar_size(scalar_size)
    {
    }

    ECPublicKey(Curves::SECPxxxr1Point point)
        : m_x(move(point.x))
        , m_y(move(point.y))
        , m_scalar_size(point.size)
    {
    }

    ECPublicKey()
        : m_x(0)
        , m_y(0)
        , m_scalar_size(0)
    {
    }

    size_t scalar_size() const { return m_scalar_size; }

    ErrorOr<ByteBuffer> x_bytes() const
    {
        return Curves::SECPxxxr1Point::scalar_to_bytes(m_x, m_scalar_size);
    }

    ErrorOr<ByteBuffer> y_bytes() const
    {
        return Curves::SECPxxxr1Point::scalar_to_bytes(m_y, m_scalar_size);
    }

    Curves::SECPxxxr1Point to_secpxxxr1_point() const
    {
        return Curves::SECPxxxr1Point { m_x, m_y, m_scalar_size };
    }

    ErrorOr<ByteBuffer> to_uncompressed() const
    {
        return to_secpxxxr1_point().to_uncompressed();
    }

private:
    Integer m_x;
    Integer m_y;
    size_t m_scalar_size;
};

// https://www.rfc-editor.org/rfc/rfc5915#section-3
template<typename Integer = UnsignedBigInteger>
class ECPrivateKey {
public:
    ECPrivateKey(Integer d, size_t scalar_size, Optional<Vector<int>> parameters, Optional<ECPublicKey<Integer>> public_key)
        : m_d(move(d))
        , m_scalar_size(scalar_size)
        , m_parameters(parameters)
        , m_public_key(public_key)
    {
    }

    ECPrivateKey() = default;

    Integer const& d() const { return m_d; }
    ErrorOr<ByteBuffer> d_bytes() const
    {
        return Curves::SECPxxxr1Point::scalar_to_bytes(m_d, m_scalar_size);
    }

    Optional<Vector<int> const&> parameters() const { return m_parameters; }
    Optional<ECPublicKey<Integer> const&> public_key() const { return m_public_key; }

    ErrorOr<ByteBuffer> export_as_der() const;

private:
    Integer m_d;
    size_t m_scalar_size;

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

    static ErrorOr<KeyPairType> parse_ec_key(ReadonlyBytes der, bool is_private, Vector<StringView> current_scope);
};

}
