/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Hex.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/PK/KeyInfo.h>
#include <LibCrypto/PK/PK.h>
#include <LibCrypto/PK/RSA.h>
#include <LibTest/TestCase.h>
#include <cstring>

// An RSA component in hex, as the big-endian bytes that RSAPrivateKey::component() gives.
static ByteBuffer integer(StringView hex)
{
    return MUST(decode_hex(hex));
}

static ByteBuffer operator""_b(char const* string, size_t length)
{
    return ByteBuffer::copy(string, length).release_value();
}

TEST_CASE(test_RSA_rejects_invalid_public_key)
{
    Array<u8, 1> message { 42 };
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, message);

    Array<u8, 128> modulus;
    modulus.fill(0xff);
    Array<u8, 128> encoded_message;
    encoded_message.fill(0xff);
    encoded_message[0] = 0;
    encoded_message[1] = 1;
    constexpr Array<u8, 19> digest_info_prefix {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    auto separator = encoded_message.size() - digest_info_prefix.size() - digest.bytes().size() - 1;
    encoded_message[separator] = 0;
    __builtin_memcpy(encoded_message.data() + separator + 1, digest_info_prefix.data(), digest_info_prefix.size());
    __builtin_memcpy(encoded_message.data() + separator + 1 + digest_info_prefix.size(), digest.bytes().data(), digest.bytes().size());

    u8 const exponent_one[] { 1 };
    auto invalid_key = TRY_OR_FAIL(Crypto::PK::RSAPublicKey::from_components(modulus, exponent_one));
    EXPECT(!TRY_OR_FAIL(Crypto::PK::rsa_pkcs1_verify(invalid_key, Crypto::Hash::HashKind::SHA256, message, encoded_message)));
}

// RSA PKCS #1 1.5
// RSA | ASN1 PKCS1 DER encoded Key import
TEST_CASE(test_RSA_ASN1_PKCS1_DER_parse)
{
    auto der = TRY_OR_FAIL(decode_base64(
        "MIIBOgIBAAJBAJsrIYHxs1YL9tpfodaWs1lJoMdF4kgFisUFSj6nvBhJUlmBh607AlgTaX0EDGPYycXYGZ2n6rqmms5lpDXBpUcC"
        "AwEAAQJAUNpPkmtEHDENxsoQBUXvXDYeXdePSiIBJhpUjoNOYoR5R9z5oX2cpcyykQ58FC2vKKg+x8N6xczG7qO95tw5UQIhAN35"
        "4CP/FA+uTeJ6KJ+izCBCl58CjNCzO0s5HTc56el5AiEAsvPKXo5/9gS/S4UzDRP6abq7GreixTfjR8LXidk3FL8CIQCTjYI861Y+"
        "hjMnlORkGSdvWlTHUj6gjEOh4TlWeJzQoQIgAxMZOQKtxCZUuxFwzRq4xLRGnrDlBQpuxz7bwSyQO7UCIHrYMnDohgNbwtA5ZpW3"
        "H1cKKQQvueWm6sxW9P5sUrZ3"sv));

    // NB: Private keys are imported inside a PrivateKeyInfo.
    auto private_key_info = TRY_OR_FAIL(Crypto::PK::wrap_in_private_key_info(der, Crypto::ASN1::rsa_encryption_oid, nullptr));
    size_t consumed = 0;
    auto private_key = TRY_OR_FAIL(Crypto::PK::parse_private_key_info(private_key_info, consumed)).rsa;
    if (TRY_OR_FAIL(private_key.component(Crypto::PK::RSAPrivateKey::Component::PublicExponent)) != integer("010001"sv)) {
        FAIL("Invalid public exponent");
    }
    if (TRY_OR_FAIL(private_key.component(Crypto::PK::RSAPrivateKey::Component::PrivateExponent)) != integer("50da4f926b441c310dc6ca100545ef5c361e5dd78f4a2201261a548e834e62847947dcf9a17d9ca5ccb2910e7c142daf28a83ec7c37ac5ccc6eea3bde6dc3951"sv)) {
        FAIL("Invalid private exponent");
    }
}

// RSA | ASN1 PKCS8 DER encoded Key import
TEST_CASE(test_RSA_ASN1_PKCS8_DER_parse)
{
    auto der = TRY_OR_FAIL(decode_base64(
        "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC7ZBYaG9+CcJP7WVFJRI/uw3hljc7WpzeYs8MN82/g9CG1gnEF"
        "3P3ZSBdWVr8gnbh05EsSGHKghIceCB7DNrM5Ab0ru04CuODdPx56xCj+4MmzTc/aq79ntmOt131NGHgq9yVwfJqnSpylOoVw7j/W"
        "g4ciwPDQaeLmD1BsE/W9UsF1km7DWasBpW5br82DpudKgJq2Ixf52+rYTCkMgyWcetx4MfXll4y5ZVtJXCnHJfkCS64EaCqXmClP"
        "4ovOuHH4khJ3rW9j4yuLe5ck3PSXOrtOTR43HZkCXzseCkbW7qKSmk/9ZreImOzOgu8vvw7ewLAQR9qYVS6XPXY8IilDAgMBAAEC"
        "ggEBAIV3ld5mt90Z/exqA2Fh+fofMyNxyz5Lv2d9sZHAL5FTkKbND18TtaIKnMSb6Gl8rKJk76slyo7Vlb8oHXEBBsm1mV0KfVen"
        "AlHS4QyjpmdTB5Yz97VR2nQuDfUFpHNC2GQRv5LMzQIWPFfaxKxYpRNOfvOb5Gks4bTmd2tjFAYRMCbHgPw1liKA9dYKk4NB0301"
        "EY05e4Zz8RjqYHkkmOPD7DnjFbHqcFUjVKK5E3vDWjxNXUbiSudCCN7WLEOyeHZNd+l6kSAVxZuCAp0G3Da5ndXgIStcy4hYi/fL"
        "3XQQbNpxjfhsjlD3tdHNr3NNYDAqxcxpsyO1NCpCIW3ZVrECgYEA7l6gTZ3e9AiSNlMd2O2vNnbQ6UZfsEfu2y7HmpCuNJkFkAnM"
        "/1h72Krejnn31rRuR6uCFn4YgQUN9Eq0E1PJCtTay2ucZw5rqtkewT9QzXvVD9eiGM+MF89UzSCC+dOW0/odkD+xP2evnPvGPbXz"
        "tnuERC1pi0YWLj1YcsfsEX0CgYEAyUA2UtYjnvCcteIy+rURT0aoZ9tDMrG+Es42EURVv1sduVdUst5R+bXx1aDzpCkcdni3Tyxe"
        "osvTGAZngI3O8ghh1GV7NPZRnkiPXjMnhL0Zf+X9gCA6TFANfPuWhMSGijYsCd46diKGDReGYUnmcN9XopeG1h6i3JiOuVPAIb8C"
        "gYBmIcUtfGb6yHFdNV+kgrJ/84ivaqe1MBz3bKO5ZiQ+BRKNFKXxAkiOHSgeg8PdCpH1w1aJrJ1zKmdANIHThiKtsWXNot3wig03"
        "tq+mvSox4Mz5bLrXRpYP3ZXIDhYQVMhbKt9f3upi8FoeOQJHjp5Nob6aN5rxQaZfSYmMJHzRQQKBgQCOALwUGTtLNBYvlKtKEadk"
        "G8RKfAFfbOFkXZLy/hfPDRjdJY0DJTIMk+BPT+F6rPODeMxHllQ0ZMPPiP1RTT5/s4BsISsdhMy0dhiLbGbvF4s9nugPly3rmPTb"
        "gp6DkjQoo+7RC7iOkO+rnzTXwxBSBpXMiUTAIx/hrdfPVxQT+wKBgCh7N3OLIOH6EWcW1fifUoENh8rkt/kzm89G1JLwBhuBIBPX"
        "UEZt2dS/xSUempqVqFGONpP87gvqxkMTtgCA73KXn/cxHWM2kmXyHA3kQlOYw6WHjpldQAxLE+TRHXO2JUtZ09Mu4rVXX7lmwbTm"
        "l3vmuDEF3/Bo1C1HTg0xRV/l"sv));

    size_t consumed = 0;
    auto private_key = TRY_OR_FAIL(Crypto::PK::parse_private_key_info(der, consumed)).rsa;
    if (TRY_OR_FAIL(private_key.component(Crypto::PK::RSAPrivateKey::Component::PublicExponent)) != integer("010001"sv)) {
        FAIL("Invalid public exponent");
    }

    if (TRY_OR_FAIL(private_key.component(Crypto::PK::RSAPrivateKey::Component::PrivateExponent)) != integer("857795de66b7dd19fdec6a036161f9fa1f332371cb3e4bbf677db191c02f915390a6cd0f5f13b5a20a9cc49be8697caca264efab25ca8ed595bf281d710106c9b5995d0a7d57a70251d2e10ca3a66753079633f7b551da742e0df505a47342d86411bf92cccd02163c57dac4ac58a5134e7ef39be4692ce1b4e6776b631406113026c780fc35962280f5d60a938341d37d35118d397b8673f118ea60792498e3c3ec39e315b1ea70552354a2b9137bc35a3c4d5d46e24ae74208ded62c43b278764d77e97a912015c59b82029d06dc36b99dd5e0212b5ccb88588bf7cbdd74106cda718df86c8e50f7b5d1cdaf734d60302ac5cc69b323b5342a42216dd956b1"sv)) {
        FAIL("Invalid private exponent");
    }
}

TEST_CASE(test_RSA_keygen_enc)
{
    auto der = TRY_OR_FAIL(decode_base64(
        "MIIBVQIBADANBgkqhkiG9w0BAQEFAASCAT8wggE7AgEAAkEA5HMXMnY+RhEcYXsaOyB/YkcrO1nxIeyDCMqwg5MDrSXO8vPXSEb9"
        "AZUNMF1jKiFWPoHxZ+foRxrLv4d9sV/ETwIDAQABAkBpC37UJkjWQRHyxP83xuasExuO6/mT5sQN692kcppTJ9wHNWoD9ZcREk4G"
        "Giklu4qx48/fYt8Cv6z6JuQ0ZQExAiEA9XRZVUnCJ2xOcCFCbyIF+d3F9Kht5rR77F9KsRlgUbkCIQDuQ7YzLpQ8V8BJwKbDeXw1"
        "vQvcPEnyKnTOoALpF6bqRwIhAIDSm8Ajgf7m3RQEoLVrCe/l8WtCqsuWliOsr6rbQq4hAiEAx8R16wvOtZlNW4jvSU1+WwAaBZl2"
        "1lfKf8OhLRXrmNkCIG9IRdcSiNR/Ut8QfD3N9Bb1HsUm+Bvzc8yGzl89pYST"sv));
    size_t consumed = 0;
    auto priv_key_info = MUST(Crypto::PK::parse_private_key_info(der, consumed));
    auto public_key = TRY_OR_FAIL(priv_key_info.rsa.public_key());
    auto wrapped_priv_der = MUST(Crypto::PK::wrap_in_private_key_info(priv_key_info.raw_key, Crypto::ASN1::rsa_encryption_oid, nullptr));

    EXPECT_EQ(der.bytes(), wrapped_priv_der.bytes());

    auto enc = TRY_OR_FAIL(Crypto::PK::rsa_oaep_encrypt(public_key, Crypto::Hash::HashKind::SHA1, {}, "WellHelloFriends"sv.bytes()));
    auto dec = TRY_OR_FAIL(Crypto::PK::rsa_oaep_decrypt(priv_key_info.rsa, Crypto::Hash::HashKind::SHA1, {}, enc));

    EXPECT_EQ(StringView { dec }, "WellHelloFriends"sv);
}

TEST_CASE(test_RSA_OAEP_rejects_short_ciphertext)
{
    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    auto hash_kind = Crypto::Hash::HashKind::SHA256;
    u8 message = 42;

    for (size_t i = 0; i < 4096; ++i) {
        auto encrypted = TRY_OR_FAIL(Crypto::PK::rsa_oaep_encrypt(keypair.public_key, hash_kind, {}, { &message, 1 }));
        if (encrypted[0] != 0)
            continue;

        auto decrypted = TRY_OR_FAIL(Crypto::PK::rsa_oaep_decrypt(keypair.private_key, hash_kind, {}, encrypted));
        EXPECT_EQ(decrypted.size(), 1u);
        EXPECT_EQ(decrypted[0], message);
        EXPECT(Crypto::PK::rsa_oaep_decrypt(keypair.private_key, hash_kind, {}, encrypted.bytes().slice(1)).is_error());
        return;
    }

    FAIL("Could not generate an RSA ciphertext with a leading zero");
}

TEST_CASE(test_RSA_PSS_rejects_short_signature)
{
    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    auto hash_kind = Crypto::Hash::HashKind::SHA256;
    ByteBuffer message { "message"_b };

    for (size_t i = 0; i < 4096; ++i) {
        auto signature = TRY_OR_FAIL(Crypto::PK::rsa_pss_sign(keypair.private_key, hash_kind, 32, message));
        if (signature[0] != 0)
            continue;

        EXPECT(TRY_OR_FAIL(Crypto::PK::rsa_pss_verify(keypair.public_key, hash_kind, 32, message, signature)));
        EXPECT(!TRY_OR_FAIL(Crypto::PK::rsa_pss_verify(keypair.public_key, hash_kind, 32, message, signature.bytes().slice(1))));
        return;
    }

    FAIL("Could not generate an RSA signature with a leading zero");
}

TEST_CASE(test_RSA_rejects_exponent_one)
{
    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    u8 const exponent_one[] { 1 };
    auto modulus = TRY_OR_FAIL(keypair.public_key.modulus());
    auto public_key = TRY_OR_FAIL(Crypto::PK::RSAPublicKey::from_components(modulus, exponent_one));

    // With e = 1, the EMSA-PKCS1-v1_5 encoding of the message is its own signature.
    ByteBuffer message { "message"_b };
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, message);
    static constexpr Array<u8, 19> sha256_digest_info { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };

    auto encoded = TRY_OR_FAIL(ByteBuffer::create_zeroed(public_key.length()));
    auto padding_end = encoded.size() - sha256_digest_info.size() - digest.bytes().size() - 1;
    encoded[1] = 0x01;
    for (size_t i = 2; i < padding_end; ++i)
        encoded[i] = 0xff;
    encoded.overwrite(padding_end + 1, sha256_digest_info.data(), sha256_digest_info.size());
    encoded.overwrite(padding_end + 1 + sha256_digest_info.size(), digest.bytes().data(), digest.bytes().size());

    EXPECT(!TRY_OR_FAIL(Crypto::PK::rsa_pkcs1_verify(public_key, Crypto::Hash::HashKind::SHA256, message, encoded)));
}
