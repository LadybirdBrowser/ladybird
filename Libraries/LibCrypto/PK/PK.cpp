/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/PK.h>

namespace Crypto::PK {

static ErrorOr<void> write_algorithm_identifier(ASN1::Encoder& encoder, Span<u32 const> algorithm, AlgorithmParameters const& parameters)
{
    return encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
        TRY(encoder.write(algorithm));
        TRY(parameters.visit(
            [](Empty) -> ErrorOr<void> { return {}; },
            [&](auto const& value) { return encoder.write(value); }));
        return {};
    });
}

ErrorOr<ByteBuffer> wrap_in_private_key_info(ReadonlyBytes private_key, Span<u32 const> algorithm, AlgorithmParameters parameters)
{
    ASN1::Encoder encoder;
    TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
        TRY(encoder.write(0x00u)); // version
        TRY(write_algorithm_identifier(encoder, algorithm, parameters));
        TRY(encoder.write(private_key));
        return {};
    }));

    return encoder.finish();
}

ErrorOr<ByteBuffer> wrap_in_subject_public_key_info(ReadonlyBytes public_key, Span<u32 const> algorithm, AlgorithmParameters parameters)
{
    ASN1::Encoder encoder;
    TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
        TRY(write_algorithm_identifier(encoder, algorithm, parameters));
        TRY(encoder.write(ASN1::BitStringView(public_key, 0)));
        return {};
    }));

    return encoder.finish();
}

}
