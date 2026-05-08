/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCrypto/KeyProvider.h>
#include <LibCrypto/SecureByteBuffer.h>

namespace Crypto {

// Storage-independent encrypted blob helper.
//
// Encrypts/decrypts blobs using AES-256-GCM with per-blob random nonces.
// Requires a KeyProvider for master key derivation. The derived key is
// held in memory only while unlocked and securely wiped on lock().
//
// Blob wire format: [12-byte nonce][ciphertext][16-byte tag]
//
// Callers are responsible for persistence. SecureStore only transforms
// plaintext <-> encrypted blobs.
class SecureStore {
    AK_MAKE_NONCOPYABLE(SecureStore);

public:
    static constexpr size_t nonce_size = 12;
    static constexpr size_t tag_size = 16;
    static constexpr size_t key_size = 32;
    static constexpr size_t salt_size = 16;
    static constexpr size_t overhead = nonce_size + tag_size;

    explicit SecureStore(NonnullOwnPtr<KeyProvider>);
    ~SecureStore();

    SecureStore(SecureStore&&);
    SecureStore& operator=(SecureStore&&);

    // Derive the master key from the provider using the given salt.
    // The salt should be generated once per store and persisted alongside
    // the encrypted data.
    ErrorOr<void> unlock(ReadonlyBytes salt);

    // Securely wipe the master key from memory. The provider retains
    // its secret (e.g. password) so unlock() can re-derive without
    // re-prompting the user. To wipe all secrets, destroy the store.
    void lock();

    [[nodiscard]] bool is_unlocked() const;

    // Encrypt plaintext into a self-contained blob (nonce || ct || tag).
    // Optional AAD binds metadata (e.g. entry ID) to the ciphertext.
    ErrorOr<ByteBuffer> encrypt(ReadonlyBytes plaintext,
        ReadonlyBytes aad = { }) const;

    // Decrypt a blob produced by encrypt(). AAD must match what was
    // used during encryption or decryption will fail.
    ErrorOr<SecureByteBuffer> decrypt(ReadonlyBytes blob,
        ReadonlyBytes aad = { }) const;

    // Generate a random salt suitable for unlock().
    static ErrorOr<ByteBuffer> generate_salt();

private:
    NonnullOwnPtr<KeyProvider> m_provider;
    Optional<SecureByteBuffer> m_key;
};

}
