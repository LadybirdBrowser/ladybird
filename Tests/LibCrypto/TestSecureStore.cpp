/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/SecureStore.h>
#include <LibTest/TestCase.h>

// Use minimal Argon2 parameters for test speed. These are cryptographically
// weak but sufficient for verifying correctness.
static Crypto::PasswordKeyProviderParameters const s_fast_params { 32, 1, 1 };

static ErrorOr<Crypto::SecureStore> create_unlocked_store(StringView password = "test-password"sv)
{
    auto provider = TRY(Crypto::PasswordKeyProvider::create(password, s_fast_params));
    auto store = Crypto::SecureStore(move(provider));
    auto salt = TRY(Crypto::SecureStore::generate_salt());
    TRY(store.unlock(salt.bytes()));
    return store;
}

TEST_CASE(test_encrypt_decrypt_round_trip)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    auto plaintext = "Hello, SecureStore!"sv.bytes();
    auto blob = TRY_OR_FAIL(store.encrypt(plaintext));
    auto decrypted = TRY_OR_FAIL(store.decrypt(blob.bytes()));

    EXPECT_EQ(decrypted.bytes(), plaintext);
}

TEST_CASE(test_encrypt_decrypt_with_aad)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    auto plaintext = "secret data"sv.bytes();
    auto aad = "entry-42"sv.bytes();
    auto blob = TRY_OR_FAIL(store.encrypt(plaintext, aad));
    auto decrypted = TRY_OR_FAIL(store.decrypt(blob.bytes(), aad));

    EXPECT_EQ(decrypted.bytes(), plaintext);
}

TEST_CASE(test_decrypt_wrong_aad_fails)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    auto blob = TRY_OR_FAIL(store.encrypt("data"sv.bytes(), "correct-aad"sv.bytes()));
    auto result = store.decrypt(blob.bytes(), "wrong-aad"sv.bytes());

    EXPECT(result.is_error());
}

TEST_CASE(test_decrypt_wrong_key_fails)
{
    auto provider_a = TRY_OR_FAIL(Crypto::PasswordKeyProvider::create("password-A"sv, s_fast_params));
    auto store_a = Crypto::SecureStore(move(provider_a));
    auto salt = TRY_OR_FAIL(Crypto::SecureStore::generate_salt());
    TRY_OR_FAIL(store_a.unlock(salt.bytes()));

    auto blob = TRY_OR_FAIL(store_a.encrypt("secret"sv.bytes()));

    auto provider_b = TRY_OR_FAIL(Crypto::PasswordKeyProvider::create("password-B"sv, s_fast_params));
    auto store_b = Crypto::SecureStore(move(provider_b));
    TRY_OR_FAIL(store_b.unlock(salt.bytes()));

    auto result = store_b.decrypt(blob.bytes());
    EXPECT(result.is_error());
}

TEST_CASE(test_encrypt_while_locked_fails)
{
    auto provider = TRY_OR_FAIL(Crypto::PasswordKeyProvider::create("pw"sv, s_fast_params));
    auto store = Crypto::SecureStore(move(provider));

    auto result = store.encrypt("data"sv.bytes());
    EXPECT(result.is_error());
}

TEST_CASE(test_decrypt_while_locked_fails)
{
    auto provider = TRY_OR_FAIL(Crypto::PasswordKeyProvider::create("pw"sv, s_fast_params));
    auto store = Crypto::SecureStore(move(provider));

    u8 dummy[32] { };
    auto result = store.decrypt(ReadonlyBytes { dummy, sizeof(dummy) });
    EXPECT(result.is_error());
}

TEST_CASE(test_lock_wipes_and_prevents_use)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());
    EXPECT(store.is_unlocked());

    store.lock();
    EXPECT(!store.is_unlocked());

    auto result = store.encrypt("data"sv.bytes());
    EXPECT(result.is_error());
}

TEST_CASE(test_encrypt_decrypt_empty_plaintext)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    auto blob = TRY_OR_FAIL(store.encrypt(ReadonlyBytes { }));
    EXPECT_EQ(blob.size(), Crypto::SecureStore::overhead);

    auto decrypted = TRY_OR_FAIL(store.decrypt(blob.bytes()));
    EXPECT(decrypted.is_empty());
}

TEST_CASE(test_blob_overhead_size)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    u8 data[100];
    auto blob = TRY_OR_FAIL(store.encrypt(ReadonlyBytes { data, sizeof(data) }));
    EXPECT_EQ(blob.size(), sizeof(data) + Crypto::SecureStore::overhead);
}

TEST_CASE(test_truncated_blob_fails)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    u8 short_blob[20] { };
    auto result = store.decrypt(ReadonlyBytes { short_blob, sizeof(short_blob) });
    EXPECT(result.is_error());
}

TEST_CASE(test_corrupted_blob_fails)
{
    auto store = TRY_OR_FAIL(create_unlocked_store());

    auto blob = TRY_OR_FAIL(store.encrypt("important data"sv.bytes()));
    // Flip a bit in the ciphertext area (after the 12-byte nonce).
    blob[Crypto::SecureStore::nonce_size] ^= 0x01;

    auto result = store.decrypt(blob.bytes());
    EXPECT(result.is_error());
}

TEST_CASE(test_generate_salt_length)
{
    auto salt = TRY_OR_FAIL(Crypto::SecureStore::generate_salt());
    EXPECT_EQ(salt.size(), Crypto::SecureStore::salt_size);
}

TEST_CASE(test_password_key_provider_deterministic)
{
    auto salt = TRY_OR_FAIL(Crypto::SecureStore::generate_salt());

    auto provider_1 = TRY_OR_FAIL(Crypto::PasswordKeyProvider::create("same-password"sv, s_fast_params));
    auto key_1 = TRY_OR_FAIL(provider_1->derive_key(salt.bytes()));

    auto provider_2 = TRY_OR_FAIL(Crypto::PasswordKeyProvider::create("same-password"sv, s_fast_params));
    auto key_2 = TRY_OR_FAIL(provider_2->derive_key(salt.bytes()));

    EXPECT_EQ(key_1.bytes(), key_2.bytes());
}
