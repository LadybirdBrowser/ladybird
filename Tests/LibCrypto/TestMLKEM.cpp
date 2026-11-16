/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibTest/TestCase.h>

using Crypto::ASN1::Class;
using Crypto::ASN1::Kind;
using Crypto::PK::MLKEM;
using Crypto::PK::MLKEMSize;

static ByteBuffer seed_of(Crypto::PK::MLKEMPrivateKey const& key)
{
    Array<u8, Crypto::PK::MLKEMPrivateKey::seed_size> buffer;
    return MUST(ByteBuffer::copy(MUST(key.seed(buffer))));
}

static MLKEM::KeyPairType generate_key_pair(u8 first_seed_byte)
{
    auto seed = MUST(ByteBuffer::create_zeroed(64));
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = first_seed_byte + i;
    return MUST(MLKEM::generate_key_pair(MLKEMSize::MLKEM512, seed));
}

static ByteBuffer encode_both(ReadonlyBytes seed, ReadonlyBytes expanded_key)
{
    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write_constructed(Class::Universal, Kind::Sequence, [&] -> ErrorOr<void> {
        TRY(encoder.write<ReadonlyBytes>(seed, Class::Universal, Kind::OctetString));
        TRY(encoder.write<ReadonlyBytes>(expanded_key, Class::Universal, Kind::OctetString));
        return {};
    }));
    return encoder.finish();
}

TEST_CASE(expanded_key)
{
    auto key_pair = generate_key_pair(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());

    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write<ReadonlyBytes>(expanded_key, Class::Universal, Kind::OctetString));
    auto der = encoder.finish();
    auto parsed = TRY_OR_FAIL(MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, der));

    EXPECT(!parsed.private_key.has_seed());
    EXPECT(TRY_OR_FAIL(parsed.private_key.expanded_key()).is_empty());
    EXPECT(parsed.private_key.export_as_der().is_error());
}

TEST_CASE(expanded_key_in_a_constructed_octet_string)
{
    auto key_pair = generate_key_pair(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());

    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write_constructed(Class::Universal, Kind::OctetString, [&] {
        return encoder.write<ReadonlyBytes>(expanded_key, Class::Universal, Kind::OctetString);
    }));

    auto der = encoder.finish();
    EXPECT(MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, der).is_error());
}

TEST_CASE(seed_and_expanded_key)
{
    auto key_pair = generate_key_pair(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());
    auto seed = seed_of(key_pair.private_key);
    auto der = encode_both(seed, expanded_key);
    auto parsed = TRY_OR_FAIL(MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, der));

    EXPECT_EQ(seed_of(parsed.private_key), seed_of(key_pair.private_key));
    EXPECT_EQ(TRY_OR_FAIL(parsed.private_key.expanded_key()), expanded_key);
}

TEST_CASE(seed_and_expanded_key_of_another_seed)
{
    auto key_pair = generate_key_pair(0);
    auto other_key_pair = generate_key_pair(1);
    auto other_expanded_key = TRY_OR_FAIL(other_key_pair.private_key.expanded_key());
    auto seed = seed_of(key_pair.private_key);
    auto der = encode_both(seed, other_expanded_key);

    EXPECT(MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, der).is_error());
}

TEST_CASE(seed_without_its_tag)
{
    auto seed = MUST(ByteBuffer::create_zeroed(64));
    EXPECT(MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, seed).is_error());
}

TEST_CASE(expanded_key_that_is_not_a_valid_key)
{
    // The expandedKey form is checked only for its encoding, since Web Crypto rejects it before validating the key.
    auto der = MUST(ByteBuffer::create_zeroed(4 + 1632));
    u8 header[] = { 0x04, 0x82, 0x06, 0x60 };
    der.overwrite(0, header, sizeof(header));
    auto parsed = TRY_OR_FAIL(MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, der));
    EXPECT(!parsed.private_key.has_seed());
}

TEST_CASE(encodings_that_are_not_der)
{
    auto key_pair = generate_key_pair(0);
    auto expanded_key = TRY_OR_FAIL(key_pair.private_key.expanded_key());
    auto const& seed = seed_of(key_pair.private_key);
    auto parse = [](Vector<u8> const& der) { return MLKEM::parse_mlkem_key(MLKEMSize::MLKEM512, der); };
    auto encode = [&](Vector<u8> header, ReadonlyBytes contents, Vector<u8> trailer = {}) {
        header.append(contents.data(), contents.size());
        header.extend(trailer);
        return header;
    };

    EXPECT(!parse(encode({ 0x80, 0x40 }, seed)).is_error());
    EXPECT(parse(encode({ 0x80, 0x40 }, seed, { 0x00 })).is_error());
    EXPECT(parse(encode({ 0x80, 0x81, 0x40 }, seed)).is_error());
    EXPECT(parse(encode({ 0x81, 0x40 }, seed)).is_error());

    auto both = encode_both(seed, expanded_key);
    EXPECT(!parse(encode({}, both)).is_error());
    auto both_with_extra_element = encode({ 0x30, 0x82, static_cast<u8>((both.size() - 2) >> 8), static_cast<u8>(both.size() - 2) }, both.bytes().slice(4), { 0x05, 0x00 });
    EXPECT(parse(both_with_extra_element).is_error());
}
