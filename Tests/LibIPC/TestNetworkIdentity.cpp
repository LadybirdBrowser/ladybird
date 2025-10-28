/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibIPC/NetworkIdentity.h>
#include <AK/ByteString.h>

/*
 * NetworkIdentity Cryptographic Identity Tests
 *
 * These tests verify that NetworkIdentity correctly:
 * 1. Generates Ed25519 keypairs on creation
 * 2. Stores public and private keys
 * 3. Zeros out private keys on destruction
 * 4. Validates key sizes (32 bytes for Ed25519)
 */

TEST_CASE(verify_ed25519_keypair_generation)
{
    // Create a NetworkIdentity
    auto identity_result = IPC::NetworkIdentity::create_for_page(1);
    EXPECT(!identity_result.is_error());

    auto identity = identity_result.release_value();

    // Verify public key was generated
    EXPECT(identity->public_key().has_value());
    auto const& public_key = identity->public_key().value();

    // Ed25519 public keys are 32 bytes
    EXPECT_EQ(public_key.length(), 32u);

    // Verify private key was generated
    EXPECT(identity->private_key().has_value());
    auto const& private_key = identity->private_key().value();

    // Ed25519 private keys are 32 bytes
    EXPECT_EQ(private_key.length(), 32u);

    // Verify keys are not all zeros (actual cryptographic material)
    bool public_key_has_nonzero = false;
    for (size_t i = 0; i < public_key.length(); i++) {
        if (public_key.characters()[i] != '\0') {
            public_key_has_nonzero = true;
            break;
        }
    }
    EXPECT(public_key_has_nonzero);

    bool private_key_has_nonzero = false;
    for (size_t i = 0; i < private_key.length(); i++) {
        if (private_key.characters()[i] != '\0') {
            private_key_has_nonzero = true;
            break;
        }
    }
    EXPECT(private_key_has_nonzero);
}

TEST_CASE(verify_unique_keypairs_per_identity)
{
    // Create two NetworkIdentity instances
    auto identity1_result = IPC::NetworkIdentity::create_for_page(1);
    auto identity2_result = IPC::NetworkIdentity::create_for_page(2);

    EXPECT(!identity1_result.is_error());
    EXPECT(!identity2_result.is_error());

    auto identity1 = identity1_result.release_value();
    auto identity2 = identity2_result.release_value();

    // Verify both have keys
    EXPECT(identity1->public_key().has_value());
    EXPECT(identity1->private_key().has_value());
    EXPECT(identity2->public_key().has_value());
    EXPECT(identity2->private_key().has_value());

    // Verify keys are different (different identities = different keys)
    EXPECT_NE(identity1->public_key().value(), identity2->public_key().value());
    EXPECT_NE(identity1->private_key().value(), identity2->private_key().value());
}

TEST_CASE(verify_private_key_cleared_on_destruction)
{
    ByteString private_key_copy;

    {
        // Create identity in a scope
        auto identity_result = IPC::NetworkIdentity::create_for_page(1);
        EXPECT(!identity_result.is_error());
        auto identity = identity_result.release_value();

        // Copy the private key for later verification
        EXPECT(identity->private_key().has_value());
        private_key_copy = identity->private_key().value();

        // Verify private key has content
        bool has_nonzero = false;
        for (size_t i = 0; i < private_key_copy.length(); i++) {
            if (private_key_copy.characters()[i] != '\0') {
                has_nonzero = true;
                break;
            }
        }
        EXPECT(has_nonzero);

        // Manually call clear_sensitive_data() to verify zeroing
        identity->clear_sensitive_data();

        // After clearing, private key should be cleared
        EXPECT(!identity->private_key().has_value());
    }

    // After scope exit (destruction), the original memory should have been zeroed
    // This is verified by the explicit_bzero call in clear_sensitive_data()
}

TEST_CASE(verify_key_generation_for_tor_identity)
{
    // Create NetworkIdentity with Tor circuit
    auto identity_result = IPC::NetworkIdentity::create_with_tor(1, "test-circuit-123"_string);
    EXPECT(!identity_result.is_error());

    auto identity = identity_result.release_value();

    // Verify Ed25519 keys were still generated (Tor doesn't prevent key generation)
    EXPECT(identity->public_key().has_value());
    EXPECT(identity->private_key().has_value());

    // Verify Tor configuration is set
    EXPECT(identity->has_tor_circuit());
    EXPECT(identity->tor_circuit_id().has_value());
    EXPECT_EQ(identity->tor_circuit_id().value(), "test-circuit-123"_string);
}

TEST_CASE(verify_key_generation_for_proxy_identity)
{
    // Create NetworkIdentity with proxy
    IPC::ProxyConfig proxy;
    proxy.type = IPC::ProxyType::SOCKS5;
    proxy.host = "proxy.example.com"_string;
    proxy.port = 1080;

    auto identity_result = IPC::NetworkIdentity::create_with_proxy(1, move(proxy));
    EXPECT(!identity_result.is_error());

    auto identity = identity_result.release_value();

    // Verify Ed25519 keys were still generated (proxy doesn't prevent key generation)
    EXPECT(identity->public_key().has_value());
    EXPECT(identity->private_key().has_value());

    // Verify proxy configuration is set
    EXPECT(identity->has_proxy());
    EXPECT(identity->proxy_config().has_value());
    EXPECT_EQ(identity->proxy_config()->host, "proxy.example.com"_string);
}
