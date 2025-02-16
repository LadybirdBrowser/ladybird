/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCrypto/OpenSSL.h>

namespace Crypto::Curves {

class EdwardsCurve {
public:
    ErrorOr<ByteBuffer> generate_private_key();
    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes private_key);

protected:
    EdwardsCurve(char const* curve_name)
        : m_curve_name(curve_name)
    {
    }

    char const* m_curve_name;
};

class SignatureEdwardsCurve : public EdwardsCurve {
public:
    ErrorOr<ByteBuffer> sign(ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context = {});
    ErrorOr<bool> verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context = {});

protected:
    explicit SignatureEdwardsCurve(char const* curve_name)
        : EdwardsCurve(curve_name)
    {
    }
};

class ExchangeEdwardsCurve : public EdwardsCurve {
public:
    ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes private_key, ReadonlyBytes public_key);

protected:
    explicit ExchangeEdwardsCurve(char const* curve_name)
        : EdwardsCurve(curve_name)
    {
    }
};

class Ed448 : public SignatureEdwardsCurve {
public:
    Ed448()
        : SignatureEdwardsCurve("ED448")
    {
    }
};

class X448 : public ExchangeEdwardsCurve {
public:
    X448()
        : ExchangeEdwardsCurve("X448")
    {
    }
};

class Ed25519 : public SignatureEdwardsCurve {
public:
    Ed25519()
        : SignatureEdwardsCurve("ED25519")
    {
    }
};

class X25519 : public ExchangeEdwardsCurve {
public:
    X25519()
        : ExchangeEdwardsCurve("X25519")
    {
    }
};

}
