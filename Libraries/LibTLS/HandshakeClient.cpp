/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Hex.h>
#include <AK/Random.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibCrypto/NumberTheory/ModularFunctions.h>
#include <LibTLS/TLSv12.h>

namespace TLS {

bool TLSv12::expand_key()
{
    u8 key[192]; // soooooooo many constants
    auto key_buffer = Bytes { key, sizeof(key) };

    auto is_aead = this->is_aead();

    if (m_context.master_key.size() == 0) {
        dbgln("expand_key() with empty master key");
        return false;
    }

    auto key_size = key_length();
    VERIFY(key_size);
    auto mac_size = mac_length();
    auto iv_size = iv_length();

    pseudorandom_function(
        key_buffer,
        m_context.master_key,
        (u8 const*)"key expansion", 13,
        ReadonlyBytes { m_context.remote_random, sizeof(m_context.remote_random) },
        ReadonlyBytes { m_context.local_random, sizeof(m_context.local_random) });

    size_t offset = 0;
    if (is_aead) {
        iv_size = 4; // Explicit IV size.
    } else {
        memcpy(m_context.crypto.local_mac, key + offset, mac_size);
        offset += mac_size;
        memcpy(m_context.crypto.remote_mac, key + offset, mac_size);
        offset += mac_size;
    }

    auto client_key = key + offset;
    offset += key_size;
    auto server_key = key + offset;
    offset += key_size;
    auto client_iv = key + offset;
    offset += iv_size;
    auto server_iv = key + offset;
    offset += iv_size;

    if constexpr (TLS_DEBUG) {
        dbgln("client key");
        print_buffer(client_key, key_size);
        dbgln("server key");
        print_buffer(server_key, key_size);
        dbgln("client iv");
        print_buffer(client_iv, iv_size);
        dbgln("server iv");
        print_buffer(server_iv, iv_size);
        if (!is_aead) {
            dbgln("client mac key");
            print_buffer(m_context.crypto.local_mac, mac_size);
            dbgln("server mac key");
            print_buffer(m_context.crypto.remote_mac, mac_size);
        }
    }

    switch (get_cipher_algorithm(m_context.cipher)) {
    case CipherAlgorithm::AES_128_CBC:
    case CipherAlgorithm::AES_256_CBC: {
        VERIFY(!is_aead);
        memcpy(m_context.crypto.local_iv, client_iv, iv_size);
        memcpy(m_context.crypto.remote_iv, server_iv, iv_size);

        m_cipher_local = Crypto::Cipher::AESCipher::CBCMode(ReadonlyBytes { client_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Encryption, Crypto::Cipher::PaddingMode::RFC5246);
        m_cipher_remote = Crypto::Cipher::AESCipher::CBCMode(ReadonlyBytes { server_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Decryption, Crypto::Cipher::PaddingMode::RFC5246);
        break;
    }
    case CipherAlgorithm::AES_128_GCM:
    case CipherAlgorithm::AES_256_GCM: {
        VERIFY(is_aead);
        memcpy(m_context.crypto.local_aead_iv, client_iv, iv_size);
        memcpy(m_context.crypto.remote_aead_iv, server_iv, iv_size);

        m_cipher_local = Crypto::Cipher::AESCipher::GCMMode(ReadonlyBytes { client_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Encryption, Crypto::Cipher::PaddingMode::RFC5246);
        m_cipher_remote = Crypto::Cipher::AESCipher::GCMMode(ReadonlyBytes { server_key, key_size }, key_size * 8, Crypto::Cipher::Intent::Decryption, Crypto::Cipher::PaddingMode::RFC5246);
        break;
    }
    case CipherAlgorithm::AES_128_CCM:
        dbgln("Requested unimplemented AES CCM cipher");
        TODO();
    case CipherAlgorithm::AES_128_CCM_8:
        dbgln("Requested unimplemented AES CCM-8 block cipher");
        TODO();
    default:
        dbgln("Requested unknown block cipher");
        VERIFY_NOT_REACHED();
    }

    m_context.crypto.created = 1;

    return true;
}

bool TLSv12::compute_master_secret_from_pre_master_secret(size_t length)
{
    if (m_context.premaster_key.size() == 0 || length < 48) {
        dbgln("there's no way I can make a master secret like this");
        dbgln("I'd like to talk to your manager about this length of {}", length);
        return false;
    }

    if (m_context.master_key.try_resize(length).is_error()) {
        dbgln("Couldn't allocate enough space for the master key :(");
        return false;
    }

    if (m_context.extensions.extended_master_secret) {
        Crypto::Hash::Manager handshake_hash_copy = m_context.handshake_hash.copy();
        auto digest = handshake_hash_copy.digest();
        auto session_hash = ReadonlyBytes { digest.immutable_data(), handshake_hash_copy.digest_size() };

        pseudorandom_function(
            m_context.master_key,
            m_context.premaster_key,
            (u8 const*)"extended master secret", 22,
            session_hash,
            {});
    } else {
        pseudorandom_function(
            m_context.master_key,
            m_context.premaster_key,
            (u8 const*)"master secret", 13,
            ReadonlyBytes { m_context.local_random, sizeof(m_context.local_random) },
            ReadonlyBytes { m_context.remote_random, sizeof(m_context.remote_random) });
    }

    m_context.premaster_key.clear();
    if constexpr (TLS_DEBUG) {
        dbgln("master key:");
        print_buffer(m_context.master_key);
    }

    expand_key();
    return true;
}

void TLSv12::build_rsa_pre_master_secret(PacketBuilder& builder)
{
    u8 random_bytes[48];
    size_t bytes = 48;

    fill_with_random(random_bytes);

    // remove zeros from the random bytes
    for (size_t i = 0; i < bytes; ++i) {
        if (!random_bytes[i])
            random_bytes[i--] = get_random<u8>();
    }

    if (m_context.is_server) {
        dbgln("Server mode not supported");
        return;
    } else {
        *(u16*)random_bytes = AK::convert_between_host_and_network_endian((u16)ProtocolVersion::VERSION_1_2);
    }

    auto premaster_key_result = ByteBuffer::copy(random_bytes, bytes);
    if (premaster_key_result.is_error()) {
        dbgln("RSA premaster key generation failed, not enough memory");
        return;
    }
    m_context.premaster_key = premaster_key_result.release_value();

    // RFC5246 section 7.4.2: The sender's certificate MUST come first in the list.
    auto& certificate = m_context.certificates.first();
    if constexpr (TLS_DEBUG) {
        dbgln("PreMaster secret");
        print_buffer(m_context.premaster_key);
    }

    Crypto::PK::RSA_PKCS1_EME rsa(certificate.public_key.rsa.modulus(), 0, certificate.public_key.rsa.public_exponent());

    Vector<u8, 32> out;
    out.resize(rsa.output_size());
    auto outbuf = out.span();
    rsa.encrypt(m_context.premaster_key, outbuf);

    if constexpr (TLS_DEBUG) {
        dbgln("Encrypted: ");
        print_buffer(outbuf);
    }

    builder.append_u24(outbuf.size() + 2);
    builder.append((u16)outbuf.size());
    builder.append(outbuf);
}

void TLSv12::build_dhe_rsa_pre_master_secret(PacketBuilder& builder)
{
    auto& dh = m_context.server_diffie_hellman_params;
    auto dh_p = Crypto::UnsignedBigInteger::import_data(dh.p.data(), dh.p.size());
    auto dh_g = Crypto::UnsignedBigInteger::import_data(dh.g.data(), dh.g.size());
    auto dh_Ys = Crypto::UnsignedBigInteger::import_data(dh.Ys.data(), dh.Ys.size());
    auto dh_key_size = dh.p.size();

    auto dh_random = Crypto::NumberTheory::random_number(0, dh_p);
    auto dh_Yc = Crypto::NumberTheory::ModularPower(dh_g, dh_random, dh_p);
    auto dh_Yc_bytes_result = ByteBuffer::create_uninitialized(dh_key_size);
    if (dh_Yc_bytes_result.is_error()) {
        dbgln("Failed to build DHE_RSA premaster secret: not enough memory");
        return;
    }
    auto dh_Yc_bytes = dh_Yc_bytes_result.release_value();
    dh_Yc.export_data(dh_Yc_bytes);

    auto premaster_key = Crypto::NumberTheory::ModularPower(dh_Ys, dh_random, dh_p);
    auto premaster_key_result = ByteBuffer::create_uninitialized(dh_key_size);
    if (premaster_key_result.is_error()) {
        dbgln("Failed to build DHE_RSA premaster secret: not enough memory");
        return;
    }
    m_context.premaster_key = premaster_key_result.release_value();
    premaster_key.export_data(m_context.premaster_key, true);

    dh.p.clear();
    dh.g.clear();
    dh.Ys.clear();

    if constexpr (TLS_DEBUG) {
        dbgln("dh_random: {}", dh_random.to_base_deprecated(16));
        dbgln("dh_Yc: {:hex-dump}", (ReadonlyBytes)dh_Yc_bytes);
        dbgln("premaster key: {:hex-dump}", (ReadonlyBytes)m_context.premaster_key);
    }

    builder.append_u24(dh_key_size + 2);
    builder.append((u16)dh_key_size);
    builder.append(dh_Yc_bytes);
}

void TLSv12::build_ecdhe_rsa_pre_master_secret(PacketBuilder& builder)
{
    // Create a random private key
    auto private_key_result = m_context.server_key_exchange_curve->generate_private_key();
    if (private_key_result.is_error()) {
        dbgln("Failed to build ECDHE_RSA premaster secret: not enough memory");
        return;
    }
    auto private_key = private_key_result.release_value();

    // Calculate the public key from the private key
    auto public_key_result = m_context.server_key_exchange_curve->generate_public_key(private_key);
    if (public_key_result.is_error()) {
        dbgln("Failed to build ECDHE_RSA premaster secret: not enough memory");
        return;
    }
    auto public_key = public_key_result.release_value();

    // Calculate the shared point by multiplying the client private key and the server public key
    ReadonlyBytes server_public_key_bytes = m_context.server_diffie_hellman_params.p;
    auto shared_point_result = m_context.server_key_exchange_curve->compute_coordinate(private_key, server_public_key_bytes);
    if (shared_point_result.is_error()) {
        dbgln("Failed to build ECDHE_RSA premaster secret: not enough memory");
        return;
    }
    auto shared_point = shared_point_result.release_value();

    // Derive the premaster key from the shared point
    auto premaster_key_result = m_context.server_key_exchange_curve->derive_premaster_key(shared_point);
    if (premaster_key_result.is_error()) {
        dbgln("Failed to build ECDHE_RSA premaster secret: not enough memory");
        return;
    }
    m_context.premaster_key = premaster_key_result.release_value();

    if constexpr (TLS_DEBUG) {
        dbgln("Build ECDHE_RSA pre master secret");
        dbgln("client private key: {:hex-dump}", (ReadonlyBytes)private_key);
        dbgln("client public key:  {:hex-dump}", (ReadonlyBytes)public_key);
        dbgln("premaster key:      {:hex-dump}", (ReadonlyBytes)m_context.premaster_key);
    }

    builder.append_u24(public_key.size() + 1);
    builder.append((u8)public_key.size());
    builder.append(public_key);
}

ByteBuffer TLSv12::build_certificate()
{
    PacketBuilder builder { ContentType::HANDSHAKE, m_context.options.version };

    Vector<Certificate const&> certificates;
    Vector<Certificate>* local_certificates = nullptr;

    if (m_context.is_server) {
        dbgln("Unsupported: Server mode");
        VERIFY_NOT_REACHED();
    } else {
        local_certificates = &m_context.client_certificates;
    }

    constexpr size_t der_length_delta = 3;
    constexpr size_t certificate_vector_header_size = 3;

    size_t total_certificate_size = 0;

    for (size_t i = 0; i < local_certificates->size(); ++i) {
        auto& certificate = local_certificates->at(i);
        if (!certificate.der.is_empty()) {
            total_certificate_size += certificate.der.size() + der_length_delta;

            // FIXME: Check for and respond with only the requested certificate types.
            if (true) {
                certificates.append(certificate);
            }
        }
    }

    builder.append((u8)HandshakeType::CERTIFICATE);

    if (!total_certificate_size) {
        dbgln_if(TLS_DEBUG, "No certificates, sending empty certificate message");
        builder.append_u24(certificate_vector_header_size);
        builder.append_u24(total_certificate_size);
    } else {
        builder.append_u24(total_certificate_size + certificate_vector_header_size); // 3 bytes for header
        builder.append_u24(total_certificate_size);

        for (auto& certificate : certificates) {
            if (!certificate.der.is_empty()) {
                builder.append_u24(certificate.der.size());
                builder.append(certificate.der.bytes());
            }
        }
    }
    auto packet = builder.build();
    update_packet(packet);
    return packet;
}

ByteBuffer TLSv12::build_client_key_exchange()
{
    bool chain_verified = m_context.verify_chain(m_context.extensions.SNI);
    if (!chain_verified) {
        dbgln("certificate verification failed :(");
        alert(AlertLevel::FATAL, AlertDescription::BAD_CERTIFICATE);
        return {};
    }

    PacketBuilder builder { ContentType::HANDSHAKE, m_context.options.version };
    builder.append((u8)HandshakeType::CLIENT_KEY_EXCHANGE_RESERVED);

    switch (get_key_exchange_algorithm(m_context.cipher)) {
    case KeyExchangeAlgorithm::RSA:
        build_rsa_pre_master_secret(builder);
        break;
    case KeyExchangeAlgorithm::DHE_DSS:
        dbgln("Client key exchange for DHE_DSS is not implemented");
        TODO();
        break;
    case KeyExchangeAlgorithm::DH_DSS:
    case KeyExchangeAlgorithm::DH_RSA:
        dbgln("Client key exchange for DH algorithms is not implemented");
        TODO();
        break;
    case KeyExchangeAlgorithm::DHE_RSA:
        build_dhe_rsa_pre_master_secret(builder);
        break;
    case KeyExchangeAlgorithm::DH_anon:
        dbgln("Client key exchange for DH_anon is not implemented");
        TODO();
        break;
    case KeyExchangeAlgorithm::ECDHE_RSA:
    case KeyExchangeAlgorithm::ECDHE_ECDSA:
        build_ecdhe_rsa_pre_master_secret(builder);
        break;
    case KeyExchangeAlgorithm::ECDH_ECDSA:
    case KeyExchangeAlgorithm::ECDH_RSA:
    case KeyExchangeAlgorithm::ECDH_anon:
        dbgln("Client key exchange for ECDHE algorithms is not implemented");
        TODO();
        break;
    default:
        dbgln("Unknown client key exchange algorithm");
        VERIFY_NOT_REACHED();
        break;
    }

    m_context.connection_status = ConnectionStatus::KeyExchange;

    auto packet = builder.build();

    update_packet(packet);

    if (!compute_master_secret_from_pre_master_secret(48)) {
        dbgln("oh noes we could not derive a master key :(");
    }

    return packet;
}

}
