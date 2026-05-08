/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/SecureStore.h>

namespace Crypto {

SecureStore::SecureStore(NonnullOwnPtr<KeyProvider> provider)
    : m_provider(move(provider))
{
}

SecureStore::~SecureStore()
{
    lock();
}

SecureStore::SecureStore(SecureStore&& other)
    : m_provider(move(other.m_provider))
    , m_key(move(other.m_key))
{
}

SecureStore& SecureStore::operator=(SecureStore&& other)
{
    if (this != &other) {
        lock();
        m_provider = move(other.m_provider);
        m_key = move(other.m_key);
    }
    return *this;
}

ErrorOr<void> SecureStore::unlock(ReadonlyBytes salt)
{
    lock();
    auto key = TRY(m_provider->derive_key(salt));
    if (key.size() != key_size)
        return Error::from_string_literal("KeyProvider returned wrong key size (expected 32 bytes)");
    m_key = move(key);
    return { };
}

void SecureStore::lock()
{
    m_key.clear();
}

bool SecureStore::is_unlocked() const
{
    return m_key.has_value();
}

ErrorOr<ByteBuffer> SecureStore::encrypt(ReadonlyBytes plaintext, ReadonlyBytes aad) const
{
    if (!is_unlocked())
        return Error::from_string_literal("SecureStore is locked");

    u8 nonce_bytes[nonce_size];
    fill_with_random({ nonce_bytes, nonce_size });

    Cipher::AESGCMCipher cipher(m_key->bytes());
    auto [ciphertext, tag] = TRY(cipher.encrypt(
        plaintext,
        ReadonlyBytes { nonce_bytes, nonce_size },
        aad,
        tag_size));

    auto blob = TRY(ByteBuffer::create_uninitialized(
        nonce_size + ciphertext.size() + tag.size()));
    blob.overwrite(0, nonce_bytes, nonce_size);
    blob.overwrite(nonce_size, ciphertext.data(), ciphertext.size());
    blob.overwrite(nonce_size + ciphertext.size(), tag.data(), tag.size());

    return blob;
}

ErrorOr<SecureByteBuffer> SecureStore::decrypt(ReadonlyBytes blob, ReadonlyBytes aad) const
{
    if (!is_unlocked())
        return Error::from_string_literal("SecureStore is locked");

    if (blob.size() < overhead)
        return Error::from_string_literal("Encrypted blob is too small");

    auto nonce = blob.slice(0, nonce_size);
    auto ciphertext = blob.slice(nonce_size, blob.size() - overhead);
    auto tag = blob.slice(blob.size() - tag_size, tag_size);

    Cipher::AESGCMCipher cipher(m_key->bytes());
    auto plaintext = TRY(cipher.decrypt(ciphertext, nonce, aad, tag));
    ScopeGuard const zero_plaintext = [&] { secure_zero(plaintext.data(), plaintext.size()); };

    return SecureByteBuffer::copy(plaintext.bytes());
}

ErrorOr<ByteBuffer> SecureStore::generate_salt()
{
    auto salt = TRY(ByteBuffer::create_uninitialized(salt_size));
    fill_with_random(salt.bytes());
    return salt;
}

}
