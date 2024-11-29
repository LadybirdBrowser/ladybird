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

namespace Crypto::PK {

RSA::KeyPairType RSA::parse_rsa_key(ReadonlyBytes der)
{
    // we are going to assign to at least one of these
    KeyPairType keypair;

    ASN1::Decoder decoder(der);
    // There are two possible (supported) formats:
    // PKCS#1 private key
    // PKCS#1 public key

    // They're all a single sequence, so let's check that first
    {
        auto result = decoder.peek();
        if (result.is_error()) {
            // Bad data.
            dbgln_if(RSA_PARSE_DEBUG, "RSA key parse failed: {}", result.error());
            return keypair;
        }
        auto tag = result.value();
        if (tag.kind != ASN1::Kind::Sequence) {
            dbgln_if(RSA_PARSE_DEBUG, "RSA key parse failed: Expected a Sequence but got {}", ASN1::kind_name(tag.kind));
            return keypair;
        }
    }

    // Then enter the sequence
    {
        auto error = decoder.enter();
        if (error.is_error()) {
            // Something was weird with the input.
            dbgln_if(RSA_PARSE_DEBUG, "RSA key parse failed: {}", error.error());
            return keypair;
        }
    }

    auto integer_result = decoder.read<UnsignedBigInteger>();
    if (integer_result.is_error()) {
        dbgln_if(RSA_PARSE_DEBUG, "RSA key parse failed: {}", integer_result.error());
        return keypair;
    }

    auto first_integer = integer_result.release_value();

    // It's a PKCS#1 key (or something we don't support)
    // if the first integer is zero or one, it's a private key.
    if (first_integer == 0) {
        // This is a private key, parse the rest.
        auto modulus_result = decoder.read<UnsignedBigInteger>();
        auto public_exponent_result = decoder.read<UnsignedBigInteger>();
        auto private_exponent_result = decoder.read<UnsignedBigInteger>();
        auto prime1_result = decoder.read<UnsignedBigInteger>();
        auto prime2_result = decoder.read<UnsignedBigInteger>();
        auto exponent1_result = decoder.read<UnsignedBigInteger>();
        auto exponent2_result = decoder.read<UnsignedBigInteger>();
        auto coefficient_result = decoder.read<UnsignedBigInteger>();

        Array results = { &modulus_result, &public_exponent_result, &private_exponent_result, &prime1_result, &prime2_result, &exponent1_result, &exponent2_result, &coefficient_result };
        for (auto& result : results) {
            if (result->is_error()) {
                dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#1 private key parse failed: {}", result->error());
                return keypair;
            }
        }

        keypair.private_key = {
            modulus_result.value(),
            private_exponent_result.release_value(),
            public_exponent_result.value(),
            prime1_result.release_value(),
            prime2_result.release_value(),
            exponent1_result.release_value(),
            exponent2_result.release_value(),
            coefficient_result.release_value(),
        };
        keypair.public_key = { modulus_result.release_value(), public_exponent_result.release_value() };

        return keypair;
    }

    if (first_integer == 1) {
        // This is a multi-prime key, we don't support that.
        dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#1 private key parse failed: Multi-prime key not supported");
        return keypair;
    }

    auto&& modulus = move(first_integer);

    // Try reading a public key, `first_integer` is the modulus.
    auto public_exponent_result = decoder.read<UnsignedBigInteger>();
    if (public_exponent_result.is_error()) {
        // Bad public key.
        dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#1 public key parse failed: {}", public_exponent_result.error());
        return keypair;
    }

    auto public_exponent = public_exponent_result.release_value();
    keypair.public_key.set(move(modulus), move(public_exponent));

    return keypair;
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

    auto key = parse_rsa_key(decoded_bytes);
    if (key.private_key.length() == 0) {
        dbgln("Failed to parse RSA private key");
        VERIFY_NOT_REACHED();
    }
    m_private_key = key.private_key;
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

    auto key = parse_rsa_key(decoded_bytes);
    if (key.public_key.length() == 0) {
        dbgln("Failed to parse RSA public key");
        VERIFY_NOT_REACHED();
    }
    m_public_key = key.public_key;
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
