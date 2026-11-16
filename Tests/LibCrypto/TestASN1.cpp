/*
 * Copyright (c) 2022, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_encoder_primitives)
{
    auto roundtrip_value = [](auto value) {
        Crypto::ASN1::Encoder encoder;
        MUST(encoder.write(value));
        auto encoded = encoder.finish();
        Crypto::ASN1::Decoder decoder(encoded);
        auto decoded = MUST(decoder.read<decltype(value)>());
        EXPECT_EQ(decoded, value);
    };

    roundtrip_value(Crypto::UnsignedBigInteger { 0 });
    roundtrip_value(Crypto::UnsignedBigInteger { 1 });
    roundtrip_value(TRY_OR_FAIL(Crypto::UnsignedBigInteger { 2 }.shift_left(128)));
    roundtrip_value(TRY_OR_FAIL(Crypto::UnsignedBigInteger { 2 }.shift_left(256)));

    roundtrip_value(Crypto::ASN1::ObjectIdentifier { 1, 2, 840, 113549, 1, 1, 1 });
    roundtrip_value(Crypto::ASN1::ObjectIdentifier { 1, 2, 840, 113549, 1, 1, 11 });
    roundtrip_value(Crypto::ASN1::ObjectIdentifier { 1, 2, 0xfffffff });

    roundtrip_value(nullptr);

    u8 const bit_string_bytes[] { 0x00, 0x01, 0x02, 0x03 };
    Crypto::ASN1::Encoder encoder;
    MUST(encoder.write(Crypto::ASN1::BitStringView { bit_string_bytes, 0 }));
    auto encoded = encoder.finish();
    Crypto::ASN1::Decoder decoder(encoded);
    auto decoded = MUST(decoder.read<Crypto::ASN1::BitStringView>());
    EXPECT_EQ(MUST(decoded.raw_bytes()), ReadonlyBytes { bit_string_bytes });
}

TEST_CASE(test_encoder_constructed)
{
    Crypto::ASN1::Encoder encoder;
    /*
     * RSAPrivateKey ::= SEQUENCE {
     *   version           Version,  -- Version ::= INTEGER { two-prime(0), multi(1) }
     *   modulus           INTEGER,  -- n
     *   publicExponent    INTEGER,  -- e
     *   privateExponent   INTEGER,  -- d
     *   prime1            INTEGER,  -- p
     *   prime2            INTEGER,  -- q
     *   exponent1         INTEGER,  -- d mod (p-1)
     *   exponent2         INTEGER,  -- d mod (q-1)
     *   coefficient       INTEGER,  -- (inverse of q) mod p
     *   otherPrimeInfos   OtherPrimeInfos OPTIONAL
     * }
     */
    (void)encoder.write_constructed(Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::Sequence, [&] {
        MUST(encoder.write(0u));       // version
        MUST(encoder.write(0x1234u));  // modulus
        MUST(encoder.write(0x10001u)); // publicExponent
        MUST(encoder.write(0x5678u));  // privateExponent
        MUST(encoder.write(0x9abcu));  // prime1
        MUST(encoder.write(0xdef0u));  // prime2
        MUST(encoder.write(0x1234u));  // exponent1
        MUST(encoder.write(0x5678u));  // exponent2
        MUST(encoder.write(0x9abcu));  // coefficient
    });
    auto encoded = encoder.finish();
    Crypto::ASN1::Decoder decoder(encoded);
    MUST(decoder.enter());                                                 // Sequence
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0u);       // version
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x1234u);  // modulus
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x10001u); // publicExponent
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x5678u);  // privateExponent
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x9abcu);  // prime1
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0xdef0u);  // prime2
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x1234u);  // exponent1
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x5678u);  // exponent2
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x9abcu);  // coefficient
    EXPECT(decoder.eof());                                                 // no otherPrimeInfos
    MUST(decoder.leave());                                                 // Sequence
    EXPECT(decoder.eof());                                                 // no other data
}

TEST_CASE(test_decoder_restores_state_after_failed_read)
{
    // An Integer tag declaring a length of 5, but only 3 value bytes are present.
    u8 const data[] { 0x02, 0x05, 0x00, 0x00, 0x00 };
    Crypto::ASN1::Decoder decoder({ data, sizeof(data) });

    EXPECT(decoder.read<Crypto::UnsignedBigInteger>().is_error());

    // The first failed read should leave the decoder state unchanged, so a second drop should have the same result.
    EXPECT(decoder.read<Crypto::UnsignedBigInteger>().is_error());
}

TEST_CASE(test_decoder_restores_state_after_failed_drop)
{
    // A Boolean tag declaring a length of 5, but only 3 value bytes are present.
    u8 const data[] { 0x01, 0x05, 0x00, 0x00, 0x00 };
    Crypto::ASN1::Decoder decoder({ data, sizeof(data) });

    EXPECT(decoder.drop().is_error());

    // The first failed drop should leave the decoder state unchanged, so a second drop should have the same result.
    EXPECT(decoder.drop().is_error());
}

TEST_CASE(test_decode_object_identifier)
{
    // 1.2.840.113549.1.1.1 (rsaEncryption)
    u8 const rsa_encryption[] { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
    Crypto::ASN1::Decoder decoder({ rsa_encryption, sizeof(rsa_encryption) });
    EXPECT_EQ(MUST(decoder.read<Crypto::ASN1::ObjectIdentifier>()), (Crypto::ASN1::ObjectIdentifier { 1, 2, 840, 113549, 1, 1, 1 }));

    // The widest subidentifier the encoder can write back out.
    u8 const widest_subidentifier[] { 0x06, 0x05, 0x2a, 0xff, 0xff, 0xff, 0x7f };
    Crypto::ASN1::Decoder widest_decoder({ widest_subidentifier, sizeof(widest_subidentifier) });
    EXPECT_EQ(MUST(widest_decoder.read<Crypto::ASN1::ObjectIdentifier>()), (Crypto::ASN1::ObjectIdentifier { 1, 2, 0xfffffff }));
}

TEST_CASE(test_decode_object_identifier_rejects_invalid_encodings)
{
    auto expect_decode_failure = [](ReadonlyBytes data) {
        Crypto::ASN1::Decoder decoder(data);
        EXPECT(decoder.read<Crypto::ASN1::ObjectIdentifier>().is_error());
    };

    // No subidentifiers at all.
    u8 const empty[] { 0x06, 0x00 };
    expect_decode_failure({ empty, sizeof(empty) });

    // rsaEncryption followed by a subidentifier that asks for a base-128 digit which never arrives.
    u8 const unterminated[] { 0x06, 0x0a, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x81 };
    expect_decode_failure({ unterminated, sizeof(unterminated) });

    // A subidentifier with a leading zero digit.
    u8 const non_minimal[] { 0x06, 0x03, 0x2a, 0x80, 0x01 };
    expect_decode_failure({ non_minimal, sizeof(non_minimal) });

    // A subidentifier one bit wider than the encoder can represent.
    u8 const too_large[] { 0x06, 0x06, 0x2a, 0x81, 0x80, 0x80, 0x80, 0x00 };
    expect_decode_failure({ too_large, sizeof(too_large) });
}

TEST_CASE(test_decoder_lengths_in_the_fewest_bytes_possible)
{
    auto read_octet_string_of_length = [](Vector<u8> length_bytes, size_t length) {
        Vector<u8> der { 0x04 };
        der.extend(length_bytes);
        for (size_t i = 0; i < length; ++i)
            der.append(0xaa);
        Crypto::ASN1::Decoder decoder { der };
        return decoder.read<StringView>();
    };

    EXPECT_EQ(TRY_OR_FAIL(read_octet_string_of_length({ 0x05 }, 5)).length(), 5u);
    EXPECT_EQ(TRY_OR_FAIL(read_octet_string_of_length({ 0x81, 0x80 }, 128)).length(), 128u);
    EXPECT_EQ(TRY_OR_FAIL(read_octet_string_of_length({ 0x82, 0x01, 0x00 }, 256)).length(), 256u);

    EXPECT(read_octet_string_of_length({ 0x81, 0x05 }, 5).is_error());
    EXPECT(read_octet_string_of_length({ 0x82, 0x00, 0x80 }, 128).is_error());
    EXPECT(read_octet_string_of_length({ 0x83, 0x00, 0x01, 0x00 }, 256).is_error());
}
