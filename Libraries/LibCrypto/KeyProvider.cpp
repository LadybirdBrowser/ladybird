/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/Hash/Argon2.h>
#include <LibCrypto/KeyProvider.h>

namespace Crypto {

ErrorOr<NonnullOwnPtr<PasswordKeyProvider>> PasswordKeyProvider::create(StringView password, Parameters params)
{
    auto password_buf = TRY(SecureByteBuffer::copy(password.bytes()));
    return adopt_own(*new PasswordKeyProvider(move(password_buf), params));
}

PasswordKeyProvider::PasswordKeyProvider(SecureByteBuffer password, Parameters params)
    : m_password(move(password))
    , m_params(params)
{
}

ErrorOr<SecureByteBuffer> PasswordKeyProvider::derive_key(ReadonlyBytes salt)
{
    Hash::Argon2 argon2(Hash::Argon2Type::Argon2id);

    auto key = TRY(argon2.derive_key(
        m_password.bytes(),
        salt,
        m_params.parallelism,
        m_params.memory_kib,
        m_params.iterations,
        0x13, // Argon2 version 1.3 (RFC 9106)
        Optional<ReadonlyBytes> { },
        Optional<ReadonlyBytes> { },
        32)); // 256-bit output

    ScopeGuard const zero_key = [&] { secure_zero(key.data(), key.size()); };
    return SecureByteBuffer::copy(key.bytes());
}

}
