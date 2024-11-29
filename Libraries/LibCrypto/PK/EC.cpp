/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Certificate/Certificate.h>

namespace {
// Used by ASN1 macros
static String s_error_string;
}

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

static ErrorOr<ECPublicKey<>> read_ec_public_key(ReadonlyBytes bytes, Vector<StringView> current_scope)
{
    // NOTE: Public keys do not have an ASN1 structure
    if (bytes.size() < 1) {
        ERROR_WITH_SCOPE("Invalid public key length");
    }

    if (bytes[0] == 0x04) {
        auto half_size = (bytes.size() - 1) / 2;
        if (1 + half_size * 2 != bytes.size()) {
            ERROR_WITH_SCOPE("Invalid public key length");
        }

        return ::Crypto::PK::ECPublicKey<> {
            UnsignedBigInteger::import_data(bytes.slice(1, half_size)),
            UnsignedBigInteger::import_data(bytes.slice(1 + half_size, half_size)),
        };
    } else {
        ERROR_WITH_SCOPE("Unsupported public key format");
    }
}

// https://www.rfc-editor.org/rfc/rfc5915#section-3
ErrorOr<EC::KeyPairType> EC::parse_ec_key(ReadonlyBytes der, bool is_private, Vector<StringView> current_scope)
{
    KeyPairType keypair;

    ASN1::Decoder decoder(der);

    if (is_private) {
        // ECPrivateKey ::= SEQUENCE {
        //      version         INTEGER { ecPrivkeyVer1(1) }(ecPrivkeyVer1),
        //      privateKey      OCTET STRING,
        //      parameters  [0] ECParameters {{ NamedCurve }} OPTIONAL,
        //      publicKey   [1] BIT STRING OPTIONAL
        // }

        ENTER_TYPED_SCOPE(Sequence, "ECPrivateKey"sv);

        PUSH_SCOPE("version");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, version);
        POP_SCOPE();

        PUSH_SCOPE("privateKey");
        READ_OBJECT(OctetString, StringView, private_key_bytes);
        POP_SCOPE();

        auto private_key = UnsignedBigInteger::import_data(private_key_bytes);

        Optional<Vector<int>> parameters;
        if (!decoder.eof()) {
            auto tag = TRY(decoder.peek());
            if (static_cast<u8>(tag.kind) == 0) {
                REWRITE_TAG(Sequence);
                ENTER_TYPED_SCOPE(Sequence, "parameters"sv);
                parameters = TRY(Crypto::Certificate::parse_ec_parameters(decoder, {}));
                EXIT_SCOPE();
            }
        }

        Optional<ECPublicKey<>> public_key;
        if (!decoder.eof()) {
            auto tag = TRY(decoder.peek());
            if (static_cast<u8>(tag.kind) == 1) {
                REWRITE_TAG(Sequence);
                ENTER_TYPED_SCOPE(Sequence, "publicKey"sv);
                READ_OBJECT(BitString, Crypto::ASN1::BitStringView, public_key_bits);

                auto public_key_bytes = TRY(public_key_bits.raw_bytes());
                auto maybe_public_key = read_ec_public_key(public_key_bytes, current_scope);
                if (maybe_public_key.is_error()) {
                    ERROR_WITH_SCOPE(maybe_public_key.release_error());
                }

                keypair.public_key = maybe_public_key.release_value();
                public_key = keypair.public_key;
                if (keypair.public_key.x().byte_length() != private_key.byte_length() || keypair.public_key.y().byte_length() != private_key.byte_length()) {
                    ERROR_WITH_SCOPE("Invalid public key length");
                }

                EXIT_SCOPE();
            }
        }

        keypair.private_key = ECPrivateKey { private_key, parameters, public_key };

        EXIT_SCOPE();
        return keypair;
    } else {
        auto maybe_key = read_ec_public_key(der, current_scope);
        if (maybe_key.is_error()) {
            ERROR_WITH_SCOPE(maybe_key.release_error());
        }

        keypair.public_key = maybe_key.release_value();
        return keypair;
    }
}

}
