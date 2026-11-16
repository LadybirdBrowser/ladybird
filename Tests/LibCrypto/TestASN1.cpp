/*
 * Copyright (c) 2022, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/DER.h>
#include <LibTest/TestCase.h>

template<typename Write>
static ByteBuffer encode(Write write)
{
    Crypto::ASN1::Encoder encoder;
    MUST(write(encoder));
    return encoder.finish();
}

static void expect_encoding(ByteBuffer const& encoded, Vector<u8> const& expected)
{
    EXPECT_EQ(encoded.bytes(), expected.span());
}

TEST_CASE(test_encoder_integers)
{
    expect_encoding(encode([](auto& encoder) { return encoder.write(Crypto::UnsignedBigInteger { 0 }); }), { 0x02, 0x01, 0x00 });
    expect_encoding(encode([](auto& encoder) { return encoder.write(Crypto::UnsignedBigInteger { 1 }); }), { 0x02, 0x01, 0x01 });
    expect_encoding(encode([](auto& encoder) { return encoder.write(0x1234u); }), { 0x02, 0x02, 0x12, 0x34 });

    // A leading zero keeps an integer whose top bit is set positive.
    expect_encoding(encode([](auto& encoder) { return encoder.write(0x80u); }), { 0x02, 0x02, 0x00, 0x80 });
}

TEST_CASE(test_encoder_object_identifiers)
{
    expect_encoding(encode([](auto& encoder) { return encoder.write(Crypto::ASN1::ObjectIdentifier { 1, 2, 840, 113549, 1, 1, 1 }); }),
        { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 });

    // The widest subidentifier the encoder can write.
    expect_encoding(encode([](auto& encoder) { return encoder.write(Crypto::ASN1::ObjectIdentifier { 1, 2, 0xfffffff }); }),
        { 0x06, 0x05, 0x2a, 0xff, 0xff, 0xff, 0x7f });
}

TEST_CASE(test_encoder_null_and_strings)
{
    expect_encoding(encode([](auto& encoder) { return encoder.write(nullptr); }), { 0x05, 0x00 });

    u8 const bytes[] { 0x00, 0x01, 0x02, 0x03 };
    expect_encoding(encode([&](auto& encoder) { return encoder.write(Crypto::ASN1::BitStringView { bytes, 0 }); }),
        { 0x03, 0x05, 0x00, 0x00, 0x01, 0x02, 0x03 });
    expect_encoding(encode([&](auto& encoder) { return encoder.template write<ReadonlyBytes>(bytes); }),
        { 0x04, 0x04, 0x00, 0x01, 0x02, 0x03 });
    expect_encoding(encode([&](auto& encoder) { return encoder.template write<ReadonlyBytes>(bytes, Crypto::ASN1::Class::Context, static_cast<Crypto::ASN1::Kind>(0)); }),
        { 0x80, 0x04, 0x00, 0x01, 0x02, 0x03 });
}

TEST_CASE(test_encoder_lengths_in_the_fewest_bytes_possible)
{
    auto header_of_octet_string = [](size_t length) {
        auto contents = MUST(ByteBuffer::create_zeroed(length));
        auto encoded = encode([&](auto& encoder) { return encoder.template write<ReadonlyBytes>(contents); });
        return MUST(ByteBuffer::copy(encoded.bytes().trim(encoded.size() - length)));
    };

    expect_encoding(header_of_octet_string(127), { 0x04, 0x7f });
    expect_encoding(header_of_octet_string(128), { 0x04, 0x81, 0x80 });
    expect_encoding(header_of_octet_string(256), { 0x04, 0x82, 0x01, 0x00 });
}

TEST_CASE(test_encoder_constructed)
{
    auto encoded = encode([](auto& encoder) {
        return encoder.write_constructed(Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write(0u));
            TRY(encoder.write(0x1234u));
            return {};
        });
    });
    expect_encoding(encoded, { 0x30, 0x07, 0x02, 0x01, 0x00, 0x02, 0x02, 0x12, 0x34 });
}
