/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/ASN1/PEM.h>
#include <LibCrypto/PK/RSA.h>

namespace Crypto::PK {

static constexpr Array<int, 7> pkcs8_rsa_key_oid { 1, 2, 840, 113549, 1, 1, 1 };

RSA::KeyPairType RSA::parse_rsa_key(ReadonlyBytes der)
{
    // we are going to assign to at least one of these
    KeyPairType keypair;

    ASN1::Decoder decoder(der);
    // There are four possible (supported) formats:
    // PKCS#1 private key
    // PKCS#1 public key
    // PKCS#8 private key
    // PKCS#8 public key

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

    bool has_read_error = false;

    auto const check_if_pkcs8_rsa_key = [&] {
        // see if it's a sequence:
        auto tag_result = decoder.peek();
        if (tag_result.is_error()) {
            // Decode error :shrug:
            dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 public key parse failed: {}", tag_result.error());
            return false;
        }

        auto tag = tag_result.value();
        if (tag.kind != ASN1::Kind::Sequence) {
            // We don't know what this is, but it sure isn't a PKCS#8 key.
            dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 public key parse failed: Expected a Sequence but got {}", ASN1::kind_name(tag.kind));
            return false;
        }

        // It's a sequence, now let's see if it's actually an RSA key.
        auto error = decoder.enter();
        if (error.is_error()) {
            // Shenanigans!
            dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 public key parse failed: {}", error.error());
            return false;
        }

        ScopeGuard leave { [&] {
            auto error = decoder.leave();
            if (error.is_error()) {
                dbgln_if(RSA_PARSE_DEBUG, "RSA key parse failed: {}", error.error());
                has_read_error = true;
            }
        } };

        // Now let's read the OID.
        auto oid_result = decoder.read<Vector<int>>();
        if (oid_result.is_error()) {
            dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 public key parse failed: {}", oid_result.error());
            return false;
        }

        auto oid = oid_result.release_value();
        // Now let's check that the OID matches "RSA key"
        if (oid != pkcs8_rsa_key_oid) {
            // Oh well. not an RSA key at all.
            dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 public key parse failed: Not an RSA key");
            return false;
        }

        return true;
    };

    auto integer_result = decoder.read<UnsignedBigInteger>();

    if (!integer_result.is_error()) {
        auto first_integer = integer_result.release_value();

        // It's either a PKCS#1 key, or a PKCS#8 private key.
        // Check for the PKCS#8 private key right away.
        if (check_if_pkcs8_rsa_key()) {
            if (has_read_error)
                return keypair;
            // Now read the private key, which is actually an octet string containing the PKCS#1 encoded private key.
            auto data_result = decoder.read<StringView>();
            if (data_result.is_error()) {
                dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 private key parse failed: {}", data_result.error());
                return keypair;
            }
            return parse_rsa_key(data_result.value().bytes());
        }

        if (has_read_error)
            return keypair;

        // It's not a PKCS#8 key, so it's a PKCS#1 key (or something we don't support)
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

    // It wasn't a PKCS#1 key, let's try our luck with PKCS#8.
    if (!check_if_pkcs8_rsa_key())
        return keypair;

    if (has_read_error)
        return keypair;

    // Now we have a bit string, which contains the PKCS#1 encoded public key.
    auto data_result = decoder.read<BitmapView>();
    if (data_result.is_error()) {
        dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#8 public key parse failed: {}", data_result.error());
        return keypair;
    }

    // Now just read it as a PKCS#1 DER.
    auto data = data_result.release_value();
    // FIXME: This is pretty awkward, maybe just generate a zero'd out ByteBuffer from the parser instead?
    auto padded_data_result = ByteBuffer::create_zeroed(data.size_in_bytes());
    if (padded_data_result.is_error()) {
        dbgln_if(RSA_PARSE_DEBUG, "RSA PKCS#1 key parse failed: Not enough memory");
        return keypair;
    }
    auto padded_data = padded_data_result.release_value();
    padded_data.overwrite(0, data.data(), data.size_in_bytes());

    return parse_rsa_key(padded_data.bytes());
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
    ByteBuffer buffer;
    if (pem) {
        buffer = decode_pem(bytes);
        bytes = buffer;
    }

    auto key = parse_rsa_key(bytes);
    if (!key.private_key.length()) {
        dbgln("We expected to see a private key, but we found none");
        VERIFY_NOT_REACHED();
    }
    m_private_key = key.private_key;
}

void RSA::import_public_key(ReadonlyBytes bytes, bool pem)
{
    ByteBuffer buffer;
    if (pem) {
        buffer = decode_pem(bytes);
        bytes = buffer;
    }

    auto key = parse_rsa_key(bytes);
    if (!key.public_key.length()) {
        dbgln("We expected to see a public key, but we found none");
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
