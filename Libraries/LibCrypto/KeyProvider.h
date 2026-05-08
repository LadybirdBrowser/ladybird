/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringView.h>
#include <LibCrypto/SecureByteBuffer.h>

namespace Crypto {

// Interface for pluggable master-key derivation strategies.
// Implementations derive a 256-bit encryption key from a secret
// (password, hardware token, etc.) and a per-store salt.
class KeyProvider {
public:
    virtual ~KeyProvider() = default;
    virtual ErrorOr<SecureByteBuffer> derive_key(ReadonlyBytes salt) = 0;
    virtual StringView name() const = 0;
};

struct PasswordKeyProviderParameters {
    u32 memory_kib { 19456 }; // 19 MiB (OWASP 2024 recommendation)
    u32 iterations { 2 };     // OWASP minimum for interactive use
    u32 parallelism { 1 };
};

// Derives a 256-bit key from a password using Argon2id (RFC 9106).
class PasswordKeyProvider final : public KeyProvider {
public:
    using Parameters = PasswordKeyProviderParameters;

    static ErrorOr<NonnullOwnPtr<PasswordKeyProvider>> create(StringView password, Parameters = { });

    ErrorOr<SecureByteBuffer> derive_key(ReadonlyBytes salt) override;
    StringView name() const override { return "password"sv; }

private:
    PasswordKeyProvider(SecureByteBuffer password, Parameters);

    SecureByteBuffer m_password;
    Parameters m_params;
};

}
