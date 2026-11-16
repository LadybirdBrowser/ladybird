/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibTest/TestCase.h>

static ByteBuffer seed_of(Crypto::PK::MLDSAPrivateKey const& key)
{
    Array<u8, Crypto::PK::MLDSAPrivateKey::seed_size> buffer;
    return MUST(ByteBuffer::copy(MUST(key.seed(buffer))));
}

TEST_CASE(TEST_SIGN_AND_VERIFY)
{
    u8 message_bytes[12] = { 0x48, 0X65, 0X6C, 0X6C, 0X6F, 0X20, 0X57, 0X6F, 0X72, 0X6C, 0X64, 0X21 }; // "Hello World!"

    ReadonlyBytes message(message_bytes, 12);

    auto [public_key, private_key] = TRY_OR_FAIL(Crypto::PK::MLDSA::generate_key_pair(Crypto::PK::MLDSA65));

    auto size = Crypto::PK::MLDSA65;

    auto const signed_message = TRY_OR_FAIL(Crypto::PK::mldsa_sign(size, private_key, {}, message));
    EXPECT(signed_message.size() == 3309);
    EXPECT(TRY_OR_FAIL(Crypto::PK::mldsa_verify(size, public_key, {}, message, signed_message)));

    auto const signed_message_with_context = TRY_OR_FAIL(Crypto::PK::mldsa_sign(size, private_key, message, message));
    EXPECT(signed_message_with_context.size() == 3309);
    EXPECT_NE(signed_message, signed_message_with_context);
    EXPECT(TRY_OR_FAIL(Crypto::PK::mldsa_verify(size, public_key, message, message, signed_message_with_context)));
}

static Crypto::PK::MLDSA::KeyPairType generate_key_pair_from_seed(u8 first_seed_byte)
{
    auto seed = MUST(ByteBuffer::create_zeroed(32));
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = first_seed_byte + i;
    return MUST(Crypto::PK::MLDSA::generate_key_pair(Crypto::PK::MLDSA44, seed));
}

static ByteBuffer encode_both(ReadonlyBytes seed, ReadonlyBytes expanded_key)
{
    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write_constructed(Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::Sequence, [&] -> ErrorOr<void> {
        TRY(encoder.write<ReadonlyBytes>(seed, Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString));
        TRY(encoder.write<ReadonlyBytes>(expanded_key, Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString));
        return {};
    }));
    return encoder.finish();
}

TEST_CASE(expanded_key)
{
    auto key_pair = generate_key_pair_from_seed(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());

    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write<ReadonlyBytes>(expanded_key, Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString));
    auto der = encoder.finish();
    auto parsed = TRY_OR_FAIL(Crypto::PK::MLDSA::parse_mldsa_key(Crypto::PK::MLDSA44, der));

    EXPECT(!parsed.private_key.has_seed());
    EXPECT(TRY_OR_FAIL(parsed.private_key.expanded_key()).is_empty());
    EXPECT(parsed.private_key.export_as_der().is_error());
}

TEST_CASE(expanded_key_in_a_constructed_octet_string)
{
    auto key_pair = generate_key_pair_from_seed(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());

    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write_constructed(Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString, [&] {
        return encoder.write<ReadonlyBytes>(expanded_key, Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString);
    }));
    auto der = encoder.finish();

    EXPECT(Crypto::PK::MLDSA::parse_mldsa_key(Crypto::PK::MLDSA44, der).is_error());
}

TEST_CASE(seed_and_expanded_key)
{
    auto key_pair = generate_key_pair_from_seed(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());
    auto seed = seed_of(key_pair.private_key);
    auto der = encode_both(seed, expanded_key);
    auto parsed = TRY_OR_FAIL(Crypto::PK::MLDSA::parse_mldsa_key(Crypto::PK::MLDSA44, der));

    EXPECT_EQ(seed_of(parsed.private_key), seed_of(key_pair.private_key));
    EXPECT_EQ(TRY_OR_FAIL(parsed.private_key.expanded_key()), expanded_key);
}

TEST_CASE(seed_and_expanded_key_of_another_seed)
{
    auto key_pair = generate_key_pair_from_seed(0);
    auto other_key_pair = generate_key_pair_from_seed(1);
    auto other_expanded_key = TRY_OR_FAIL(other_key_pair.private_key.expanded_key());
    auto seed = seed_of(key_pair.private_key);
    auto der = encode_both(seed, other_expanded_key);

    EXPECT(Crypto::PK::MLDSA::parse_mldsa_key(Crypto::PK::MLDSA44, der).is_error());
}

TEST_CASE(seed_without_its_tag)
{
    auto seed = MUST(ByteBuffer::create_zeroed(32));
    EXPECT(Crypto::PK::MLDSA::parse_mldsa_key(Crypto::PK::MLDSA44, seed).is_error());
}

TEST_CASE(expanded_key_that_is_not_a_valid_key)
{
    // The expandedKey form is checked only for its encoding, since Web Crypto rejects it before validating the key.
    auto der = MUST(ByteBuffer::create_zeroed(4 + 2560));
    u8 header[] = { 0x04, 0x82, 0x0a, 0x00 };
    der.overwrite(0, header, sizeof(header));
    auto parsed = TRY_OR_FAIL(Crypto::PK::MLDSA::parse_mldsa_key(Crypto::PK::MLDSA44, der));
    EXPECT(!parsed.private_key.has_seed());
}
