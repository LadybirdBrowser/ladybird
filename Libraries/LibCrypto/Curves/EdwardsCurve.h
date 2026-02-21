/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::Curves {

enum class EdwardsCurveType : u8 {
    Ed25519,
    Ed448,
    X25519,
    X448
};

class EdwardsCurve {
public:
    ErrorOr<ByteBuffer> generate_private_key();
    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes private_key);

protected:
    EdwardsCurve(EdwardsCurveType curve_type)
        : m_curve_type(curve_type)
    {
    }

    static char const* curve_type_to_openssl_name(EdwardsCurveType curve_type);

    EdwardsCurveType m_curve_type;
};

class SignatureEdwardsCurve : public EdwardsCurve {
public:
    ErrorOr<ByteBuffer> sign(ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context = {});
    ErrorOr<bool> verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context = {});

protected:
    explicit SignatureEdwardsCurve(EdwardsCurveType curve_type)
        : EdwardsCurve(curve_type)
    {
    }
};

class ExchangeEdwardsCurve : public EdwardsCurve {
public:
    ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes private_key, ReadonlyBytes public_key);

protected:
    explicit ExchangeEdwardsCurve(EdwardsCurveType curve_type)
        : EdwardsCurve(curve_type)
    {
    }
};

class Ed448 : public SignatureEdwardsCurve {
public:
    Ed448()
        : SignatureEdwardsCurve(EdwardsCurveType::Ed448)
    {
    }
};

class X448 : public ExchangeEdwardsCurve {
public:
    X448()
        : ExchangeEdwardsCurve(EdwardsCurveType::X448)
    {
    }
};

class Ed25519 : public SignatureEdwardsCurve {
public:
    Ed25519()
        : SignatureEdwardsCurve(EdwardsCurveType::Ed25519)
    {
    }
};

class X25519 : public ExchangeEdwardsCurve {
public:
    X25519()
        : ExchangeEdwardsCurve(EdwardsCurveType::X25519)
    {
    }
};

}
