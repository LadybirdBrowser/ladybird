/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibTest/TestCase.h>

TEST_CASE(expanded_private_key_uses_primitive_octet_string)
{
    auto expanded_key = TRY_OR_FAIL(ByteBuffer::create_zeroed(1632));
    Crypto::ASN1::Encoder encoder;
    TRY_OR_FAIL(encoder.write<ReadonlyBytes>(expanded_key, Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString));
    auto primitive = encoder.finish();

    auto parsed = Crypto::PK::MLKEM::parse_mlkem_key(Crypto::PK::MLKEMSize::MLKEM512, primitive, {});
    EXPECT(!parsed.is_error());
}

TEST_CASE(constructed_expanded_private_key_is_rejected)
{
    auto expanded_key = TRY_OR_FAIL(ByteBuffer::create_zeroed(1632));
    Crypto::ASN1::Encoder encoder;
    TRY_OR_FAIL(encoder.write<ReadonlyBytes>(expanded_key, Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::OctetString));
    auto primitive = encoder.finish();

    auto constructed = TRY_OR_FAIL(ByteBuffer::create_uninitialized(4 + primitive.size()));
    Array<u8, 4> header { 0x24, 0x82, 0x06, 0x64 };
    constructed.overwrite(0, header.data(), header.size());
    constructed.overwrite(header.size(), primitive.data(), primitive.size());

    auto parsed = Crypto::PK::MLKEM::parse_mlkem_key(Crypto::PK::MLKEMSize::MLKEM512, constructed, {});
    EXPECT(parsed.is_error());
}
