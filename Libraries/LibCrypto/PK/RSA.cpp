/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/Random.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/ASN1/PEM.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/PK/RSA.h>

namespace {
// Used by ASN1 macros
static String s_error_string;
}

namespace Crypto::PK {

ErrorOr<RSA::KeyPairType> RSA::parse_rsa_key(ReadonlyBytes der, bool is_private, Vector<StringView> current_scope)
{
    KeyPairType keypair;

    ASN1::Decoder decoder(der);

    if (is_private) {
        // RSAPrivateKey ::= SEQUENCE {
        //      version             Version,
        //      modulus             INTEGER,
        //      publicExponent      INTEGER,
        //      privateExponent     INTEGER,
        //      prime1              INTEGER,
        //      prime2              INTEGER,
        //      exponent1           INTEGER,
        //      exponent2           INTEGER,
        //      coefficient         INTEGER,
        //      otherPrimeInfos     OtherPrimeInfos OPTIONAL
        // }

        ENTER_TYPED_SCOPE(Sequence, "RSAPrivateKey"sv);

        PUSH_SCOPE("version");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, version);
        POP_SCOPE();
        if (version != 0) {
            ERROR_WITH_SCOPE(TRY(String::formatted("Invalid version value at {}", current_scope)));
        }

        PUSH_SCOPE("modulus");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, modulus);
        POP_SCOPE();

        PUSH_SCOPE("publicExponent");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, public_exponent);
        POP_SCOPE();

        PUSH_SCOPE("privateExponent");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, private_exponent);
        POP_SCOPE();

        PUSH_SCOPE("prime1");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, prime1);
        POP_SCOPE();

        PUSH_SCOPE("prime2");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, prime2);
        POP_SCOPE();

        PUSH_SCOPE("exponent1");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, exponent1);
        POP_SCOPE();

        PUSH_SCOPE("exponent2");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, exponent2);
        POP_SCOPE();

        PUSH_SCOPE("coefficient");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, coefficient);
        POP_SCOPE();

        keypair.private_key = {
            modulus,
            private_exponent,
            public_exponent,
            prime1,
            prime2,
            exponent1,
            exponent2,
            coefficient,
        };
        keypair.public_key = { modulus, public_exponent };

        EXIT_SCOPE();
        return keypair;
    } else {
        // RSAPublicKey ::= SEQUENCE {
        //      modulus         INTEGER,
        //      publicExponent  INTEGER
        // }

        ENTER_TYPED_SCOPE(Sequence, "RSAPublicKey"sv);

        PUSH_SCOPE("modulus");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, modulus);
        POP_SCOPE();

        PUSH_SCOPE("publicExponent");
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, public_exponent);
        POP_SCOPE();

        keypair.public_key = { move(modulus), move(public_exponent) };

        EXIT_SCOPE();
        return keypair;
    }
}

void RSA::encrypt(ReadonlyBytes in, Bytes& out)
{
    dbgln_if(CRYPTO_DEBUG, "in size: {}", in.size());
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    if (!(in_integer < m_public_key.modulus())) {
        dbgln("value too large for key");
        out = {};
        return;
    }
    auto exp = NumberTheory::ModularPower(in_integer, m_public_key.public_exponent(), m_public_key.modulus());
    auto size = exp.export_data(out);
    auto outsize = out.size();
    if (size != outsize) {
        dbgln("POSSIBLE RSA BUG!!! Size mismatch: {} requested but {} bytes generated", outsize, size);
        out = out.slice(outsize - size, size);
    }
}

void RSA::decrypt(ReadonlyBytes in, Bytes& out)
{
    // FIXME: Actually use the private key properly

    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    auto exp = NumberTheory::ModularPower(in_integer, m_private_key.private_exponent(), m_private_key.modulus());
    auto size = exp.export_data(out);

    auto align = m_private_key.length();
    auto aligned_size = (size + align - 1) / align * align;

    for (auto i = size; i < aligned_size; ++i)
        out[out.size() - i - 1] = 0; // zero the non-aligned values
    out = out.slice(out.size() - aligned_size, aligned_size);
}

void RSA::sign(ReadonlyBytes in, Bytes& out)
{
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    auto exp = NumberTheory::ModularPower(in_integer, m_private_key.private_exponent(), m_private_key.modulus());
    auto size = exp.export_data(out);
    out = out.slice(out.size() - size, size);
}

void RSA::verify(ReadonlyBytes in, Bytes& out)
{
    auto in_integer = UnsignedBigInteger::import_data(in.data(), in.size());
    auto exp = NumberTheory::ModularPower(in_integer, m_public_key.public_exponent(), m_public_key.modulus());
    auto size = exp.export_data(out);
    out = out.slice(out.size() - size, size);
}

void RSA::import_private_key(ReadonlyBytes bytes, bool pem)
{
    ByteBuffer decoded_bytes;
    if (pem) {
        auto decoded = decode_pem(bytes);
        if (decoded.type == PEMType::RSAPrivateKey) {
            decoded_bytes = decoded.data;
        } else if (decoded.type == PEMType::PrivateKey) {
            ASN1::Decoder decoder(decoded.data);
            auto maybe_key = Certificate::parse_private_key_info(decoder, {});
            if (maybe_key.is_error()) {
                dbgln("Failed to parse private key info: {}", maybe_key.error());
                VERIFY_NOT_REACHED();
            }

            m_private_key = maybe_key.release_value().rsa;
            return;
        } else {
            dbgln("Expected a PEM encoded private key");
            VERIFY_NOT_REACHED();
        }
    }

    auto maybe_key = parse_rsa_key(decoded_bytes, true, {});
    if (maybe_key.is_error()) {
        dbgln("Failed to parse RSA private key: {}", maybe_key.error());
        VERIFY_NOT_REACHED();
    }
    m_private_key = maybe_key.release_value().private_key;
}

void RSA::import_public_key(ReadonlyBytes bytes, bool pem)
{
    ByteBuffer decoded_bytes;
    if (pem) {
        auto decoded = decode_pem(bytes);
        if (decoded.type == PEMType::RSAPublicKey) {
            decoded_bytes = decoded.data;
        } else if (decoded.type == PEMType::PublicKey) {
            ASN1::Decoder decoder(decoded.data);
            auto maybe_key = Certificate::parse_subject_public_key_info(decoder, {});
            if (maybe_key.is_error()) {
                dbgln("Failed to parse subject public key info: {}", maybe_key.error());
                VERIFY_NOT_REACHED();
            }

            m_public_key = maybe_key.release_value().rsa;
            return;
        } else {
            dbgln("Expected a PEM encoded public key");
            VERIFY_NOT_REACHED();
        }
    }

    auto maybe_key = parse_rsa_key(decoded_bytes, false, {});
    if (maybe_key.is_error()) {
        dbgln("Failed to parse RSA public key: {}", maybe_key.error());
        VERIFY_NOT_REACHED();
    }
    m_public_key = maybe_key.release_value().public_key;
}

void RSA_PKCS1_EME::encrypt(ReadonlyBytes in, Bytes& out)
{
    auto mod_len = (m_public_key.modulus().trimmed_length() * sizeof(u32) * 8 + 7) / 8;
    dbgln_if(CRYPTO_DEBUG, "key size: {}", mod_len);
    if (in.size() > mod_len - 11) {
        dbgln("message too long :(");
        out = out.trim(0);
        return;
    }
    if (out.size() < mod_len) {
        dbgln("output buffer too small");
        return;
    }

    auto ps_length = mod_len - in.size() - 3;
    Vector<u8, 8096> ps;
    ps.resize(ps_length);

    fill_with_random(ps);
    // since fill_with_random can create zeros (shocking!)
    // we have to go through and un-zero the zeros
    for (size_t i = 0; i < ps_length; ++i) {
        while (!ps[i])
            ps[i] = get_random<u8>();
    }

    u8 paddings[] { 0x00, 0x02 };

    out.overwrite(0, paddings, 2);
    out.overwrite(2, ps.data(), ps_length);
    out.overwrite(2 + ps_length, paddings, 1);
    out.overwrite(3 + ps_length, in.data(), in.size());
    out = out.trim(3 + ps_length + in.size()); // should be a single block

    dbgln_if(CRYPTO_DEBUG, "padded output size: {} buffer size: {}", 3 + ps_length + in.size(), out.size());

    RSA::encrypt(out, out);
}
void RSA_PKCS1_EME::decrypt(ReadonlyBytes in, Bytes& out)
{
    auto mod_len = (m_public_key.modulus().trimmed_length() * sizeof(u32) * 8 + 7) / 8;
    if (in.size() != mod_len) {
        dbgln("decryption error: wrong amount of data: {}", in.size());
        out = out.trim(0);
        return;
    }

    RSA::decrypt(in, out);

    if (out.size() < RSA::output_size()) {
        dbgln("decryption error: not enough data after decryption: {}", out.size());
        out = out.trim(0);
        return;
    }

    if (out[0] != 0x00) {
        dbgln("invalid padding byte 0 : {}", out[0]);
        return;
    }

    if (out[1] != 0x02) {
        dbgln("invalid padding byte 1 : {}", out[1]);
        return;
    }

    size_t offset = 2;
    while (offset < out.size() && out[offset])
        ++offset;

    if (offset == out.size()) {
        dbgln("garbage data, no zero to split padding");
        return;
    }

    ++offset;

    if (offset - 3 < 8) {
        dbgln("PS too small");
        return;
    }

    out = out.slice(offset, out.size() - offset);
}

void RSA_PKCS1_EME::sign(ReadonlyBytes, Bytes&)
{
    dbgln("FIXME: RSA_PKCS_EME::sign");
}
void RSA_PKCS1_EME::verify(ReadonlyBytes, Bytes&)
{
    dbgln("FIXME: RSA_PKCS_EME::verify");
}
}
