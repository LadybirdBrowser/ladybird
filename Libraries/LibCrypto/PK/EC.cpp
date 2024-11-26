/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Certificate/Certificate.h>

namespace Crypto::PK {

template<>
ErrorOr<ByteBuffer> ECPrivateKey<IntegerType>::export_as_der() const
{
    ASN1::Encoder encoder;
    TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
        TRY(encoder.write(1u)); // version

        auto d_bytes = TRY(ByteBuffer::create_uninitialized(m_d.byte_length()));
        auto d_size = m_d.export_data(d_bytes.span());
        TRY(encoder.write<ReadonlyBytes>(d_bytes.span().slice(0, d_size)));

        if (m_parameters.has_value()) {
            TRY(encoder.write_constructed(ASN1::Class::Context, static_cast<ASN1::Kind>(0), [&]() -> ErrorOr<void> {
                TRY(encoder.write<Vector<int>>(*m_parameters, {}, ASN1::Kind::ObjectIdentifier));
                return {};
            }));
        }

        if (m_public_key.has_value()) {
            TRY(encoder.write_constructed(ASN1::Class::Context, static_cast<ASN1::Kind>(1), [&]() -> ErrorOr<void> {
                auto public_key_bytes = TRY(m_public_key->to_uncompressed());
                TRY(encoder.write(ASN1::BitStringView(public_key_bytes, 0)));

                return {};
            }));
        }

        return {};
    }));

    return encoder.finish();
}

// https://www.rfc-editor.org/rfc/rfc5915#section-3
ErrorOr<EC::KeyPairType> EC::parse_ec_key(ReadonlyBytes der)
{
    // ECPrivateKey ::= SEQUENCE {
    //      version         INTEGER { ecPrivkeyVer1(1) }(ecPrivkeyVer1),
    //      privateKey      OCTET STRING,
    //      parameters  [0] ECParameters {{ NamedCurve }} OPTIONAL,
    //      publicKey   [1] BIT STRING OPTIONAL
    // }
    KeyPairType keypair;

    ASN1::Decoder decoder(der);

    auto tag = TRY(decoder.peek());
    if (tag.kind != ASN1::Kind::Sequence) {
        auto message = TRY(String::formatted("EC key parse failed: Expected a Sequence but got {}", ASN1::kind_name(tag.kind)));
        return Error::from_string_view(message.bytes_as_string_view());
    }

    TRY(decoder.enter());

    auto version = TRY(decoder.read<UnsignedBigInteger>());
    if (version != 1) {
        auto message = TRY(String::formatted("EC key parse failed: Invalid version {}", version));
        return Error::from_string_view(message.bytes_as_string_view());
    }

    auto private_key = TRY(decoder.read<StringView>());

    Optional<Vector<int>> parameters;
    if (!decoder.eof()) {
        auto tag = TRY(decoder.peek());
        if (static_cast<u8>(tag.kind) == 0) {
            TRY(decoder.rewrite_tag(ASN1::Kind::Sequence));
            TRY(decoder.enter());

            parameters = TRY(Crypto::Certificate::parse_ec_parameters(decoder, {}));

            TRY(decoder.leave());
        }
    }

    Optional<ECPublicKey<>> public_key;
    if (!decoder.eof()) {
        auto tag = TRY(decoder.peek());
        if (static_cast<u8>(tag.kind) == 1) {
            TRY(decoder.rewrite_tag(ASN1::Kind::Sequence));
            TRY(decoder.enter());

            auto public_key_bits = TRY(decoder.read<ASN1::BitStringView>());
            auto public_key_bytes = TRY(public_key_bits.raw_bytes());
            if (public_key_bytes.size() != 1 + private_key.length() * 2) {
                return Error::from_string_literal("EC key parse failed: Invalid public key length");
            }

            if (public_key_bytes[0] != 0x04) {
                return Error::from_string_literal("EC key parse failed: Unsupported public key format");
            }

            public_key = ::Crypto::PK::ECPublicKey<> {
                UnsignedBigInteger::import_data(public_key_bytes.slice(1, private_key.length())),
                UnsignedBigInteger::import_data(public_key_bytes.slice(1 + private_key.length(), private_key.length())),
            };

            TRY(decoder.leave());
        }
    }

    keypair.private_key = ECPrivateKey {
        UnsignedBigInteger::import_data(private_key),
        parameters,
        public_key,
    };

    return keypair;
}

}
