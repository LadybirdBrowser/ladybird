/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/Random.h>

#include <LibCore/Timer.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Curves/Ed25519.h>
#include <LibCrypto/Curves/EllipticCurve.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/Curves/X25519.h>
#include <LibCrypto/Curves/X448.h>
#include <LibCrypto/PK/Code/EMSA_PKCS1_V1_5.h>
#include <LibTLS/TLSv12.h>

namespace TLS {

ssize_t TLSv12::handle_server_hello(ReadonlyBytes buffer, WritePacketStage& write_packets)
{
    write_packets = WritePacketStage::Initial;
    if (m_context.connection_status != ConnectionStatus::Disconnected && m_context.connection_status != ConnectionStatus::Renegotiating) {
        dbgln("unexpected hello message");
        return (i8)Error::UnexpectedMessage;
    }
    ssize_t res = 0;
    size_t min_hello_size = 41;

    if (min_hello_size > buffer.size()) {
        dbgln("need more data");
        return (i8)Error::NeedMoreData;
    }
    size_t following_bytes = buffer[0] * 0x10000 + buffer[1] * 0x100 + buffer[2];
    res += 3;
    if (buffer.size() - res < following_bytes) {
        dbgln("not enough data after header: {} < {}", buffer.size() - res, following_bytes);
        return (i8)Error::NeedMoreData;
    }

    if (buffer.size() - res < 2) {
        dbgln("not enough data for version");
        return (i8)Error::NeedMoreData;
    }
    auto version = static_cast<ProtocolVersion>(AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res))));

    res += 2;
    if (!supports_version(version))
        return (i8)Error::NotSafe;

    memcpy(m_context.remote_random, buffer.offset_pointer(res), sizeof(m_context.remote_random));
    res += sizeof(m_context.remote_random);

    u8 session_length = buffer[res++];
    if (buffer.size() - res < session_length) {
        dbgln("not enough data for session id");
        return (i8)Error::NeedMoreData;
    }

    if (session_length && session_length <= 32) {
        memcpy(m_context.session_id, buffer.offset_pointer(res), session_length);
        m_context.session_id_size = session_length;
        if constexpr (TLS_DEBUG) {
            dbgln("Remote session ID:");
            print_buffer(ReadonlyBytes { m_context.session_id, session_length });
        }
    } else {
        m_context.session_id_size = 0;
    }
    res += session_length;

    if (buffer.size() - res < 2) {
        dbgln("not enough data for cipher suite listing");
        return (i8)Error::NeedMoreData;
    }
    auto cipher = static_cast<CipherSuite>(AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res))));
    res += 2;
    if (!supports_cipher(cipher)) {
        m_context.cipher = CipherSuite::TLS_NULL_WITH_NULL_NULL;
        dbgln("No supported cipher could be agreed upon");
        return (i8)Error::NoCommonCipher;
    }
    m_context.cipher = cipher;
    dbgln_if(TLS_DEBUG, "Cipher: {}", enum_to_string(cipher));

    // Simplification: We only support handshake hash functions via HMAC
    m_context.handshake_hash.initialize(hmac_hash());

    // Compression method
    if (buffer.size() - res < 1)
        return (i8)Error::NeedMoreData;
    u8 compression = buffer[res++];
    if (compression != 0)
        return (i8)Error::CompressionNotSupported;

    if (m_context.connection_status != ConnectionStatus::Renegotiating)
        m_context.connection_status = ConnectionStatus::Negotiating;
    if (m_context.is_server) {
        dbgln("unsupported: server mode");
        write_packets = WritePacketStage::ServerHandshake;
    }

    // Presence of extensions is determined by availability of bytes after compression_method
    if (buffer.size() - res >= 2) {
        auto extensions_bytes_total = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res += 2)));
        dbgln_if(TLS_DEBUG, "Extensions bytes total: {}", extensions_bytes_total);
    }

    while (buffer.size() - res >= 4) {
        auto extension_type = (ExtensionType)AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res)));
        res += 2;
        u16 extension_length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res)));
        res += 2;

        dbgln_if(TLS_DEBUG, "Extension {} with length {}", enum_to_string(extension_type), extension_length);

        if (buffer.size() - res < extension_length)
            return (i8)Error::NeedMoreData;

        if (extension_type == ExtensionType::SERVER_NAME) {
            // RFC6066 section 3: SNI extension_data can be empty in the server hello
            if (extension_length > 0) {
                // ServerNameList total size
                if (buffer.size() - res < 2)
                    return (i8)Error::NeedMoreData;
                auto sni_name_list_bytes = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res += 2)));
                dbgln_if(TLS_DEBUG, "SNI: expecting ServerNameList of {} bytes", sni_name_list_bytes);

                // Exactly one ServerName should be present
                if (buffer.size() - res < 3)
                    return (i8)Error::NeedMoreData;
                auto sni_name_type = (NameType)(*(u8 const*)buffer.offset_pointer(res++));
                auto sni_name_length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res += 2)));

                if (sni_name_type != NameType::HOST_NAME)
                    return (i8)Error::NotUnderstood;

                if (sizeof(sni_name_type) + sizeof(sni_name_length) + sni_name_length != sni_name_list_bytes)
                    return (i8)Error::BrokenPacket;

                // Read out the host_name
                if (buffer.size() - res < sni_name_length)
                    return (i8)Error::NeedMoreData;
                m_context.extensions.SNI = ByteString { (char const*)buffer.offset_pointer(res), sni_name_length };
                res += sni_name_length;
                dbgln("SNI host_name: {}", m_context.extensions.SNI);
            }
        } else if (extension_type == ExtensionType::APPLICATION_LAYER_PROTOCOL_NEGOTIATION && m_context.alpn.size()) {
            if (buffer.size() - res > 2) {
                auto alpn_length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(res)));
                if (alpn_length && alpn_length <= extension_length - 2) {
                    u8 const* alpn = buffer.offset_pointer(res + 2);
                    size_t alpn_position = 0;
                    while (alpn_position < alpn_length) {
                        u8 alpn_size = alpn[alpn_position++];
                        if (alpn_size + alpn_position >= extension_length)
                            break;
                        ByteString alpn_str { (char const*)alpn + alpn_position, alpn_length };
                        if (alpn_size && m_context.alpn.contains_slow(alpn_str)) {
                            m_context.negotiated_alpn = alpn_str;
                            dbgln("negotiated alpn: {}", alpn_str);
                            break;
                        }
                        alpn_position += alpn_length;
                        if (!m_context.is_server) // server hello must contain one ALPN
                            break;
                    }
                }
            }
            res += extension_length;
        } else if (extension_type == ExtensionType::SIGNATURE_ALGORITHMS) {
            dbgln("supported signatures: ");
            print_buffer(buffer.slice(res, extension_length));
            res += extension_length;
            // FIXME: what are we supposed to do here?
        } else if (extension_type == ExtensionType::EC_POINT_FORMATS) {
            // RFC8422 section 5.2: A server that selects an ECC cipher suite in response to a ClientHello message
            // including a Supported Point Formats Extension appends this extension (along with others) to its
            // ServerHello message, enumerating the point formats it can parse. The Supported Point Formats Extension,
            // when used, MUST contain the value 0 (uncompressed) as one of the items in the list of point formats.
            //
            // The current implementation only supports uncompressed points, and the server is required to support
            // uncompressed points. Therefore, this extension can be safely ignored as it should always inform us
            // that the server supports uncompressed points.
            res += extension_length;
        } else if (extension_type == ExtensionType::EXTENDED_MASTER_SECRET) {
            m_context.extensions.extended_master_secret = true;
            res += extension_length;
        } else {
            dbgln("Encountered unknown extension {} with length {}", enum_to_string(extension_type), extension_length);
            res += extension_length;
        }
    }

    return res;
}

ssize_t TLSv12::handle_server_hello_done(ReadonlyBytes buffer)
{
    if (buffer.size() < 3)
        return (i8)Error::NeedMoreData;

    size_t size = buffer[0] * 0x10000 + buffer[1] * 0x100 + buffer[2];

    if (buffer.size() - 3 < size)
        return (i8)Error::NeedMoreData;

    return size + 3;
}

ByteBuffer TLSv12::build_server_key_exchange()
{
    dbgln("FIXME: build_server_key_exchange");
    return {};
}

ssize_t TLSv12::handle_server_key_exchange(ReadonlyBytes buffer)
{
    switch (get_key_exchange_algorithm(m_context.cipher)) {
    case KeyExchangeAlgorithm::RSA:
    case KeyExchangeAlgorithm::DH_DSS:
    case KeyExchangeAlgorithm::DH_RSA:
        // RFC 5246 section 7.4.3. Server Key Exchange Message
        // It is not legal to send the server key exchange message for RSA, DH_DSS, DH_RSA
        dbgln("Server key exchange received for RSA, DH_DSS or DH_RSA is not legal");
        return (i8)Error::UnexpectedMessage;
    case KeyExchangeAlgorithm::DHE_DSS:
        dbgln("Server key exchange for DHE_DSS is not implemented");
        TODO();
        break;
    case KeyExchangeAlgorithm::DHE_RSA:
        return handle_dhe_rsa_server_key_exchange(buffer);
    case KeyExchangeAlgorithm::DH_anon:
        dbgln("Server key exchange for DH_anon is not implemented");
        TODO();
        break;
    case KeyExchangeAlgorithm::ECDHE_RSA:
        return handle_ecdhe_rsa_server_key_exchange(buffer);
    case KeyExchangeAlgorithm::ECDHE_ECDSA:
        return handle_ecdhe_ecdsa_server_key_exchange(buffer);
    case KeyExchangeAlgorithm::ECDH_ECDSA:
    case KeyExchangeAlgorithm::ECDH_RSA:
    case KeyExchangeAlgorithm::ECDH_anon:
        dbgln("Server key exchange for ECDHE algorithms is not implemented");
        TODO();
        break;
    default:
        dbgln("Unknown server key exchange algorithm");
        VERIFY_NOT_REACHED();
        break;
    }
    return 0;
}

ssize_t TLSv12::handle_dhe_rsa_server_key_exchange(ReadonlyBytes buffer)
{
    auto dh_p_length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(3)));
    auto dh_p = buffer.slice(5, dh_p_length);
    auto p_result = ByteBuffer::copy(dh_p);
    if (p_result.is_error()) {
        dbgln("dhe_rsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    m_context.server_diffie_hellman_params.p = p_result.release_value();

    auto dh_g_length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(5 + dh_p_length)));
    auto dh_g = buffer.slice(7 + dh_p_length, dh_g_length);
    auto g_result = ByteBuffer::copy(dh_g);
    if (g_result.is_error()) {
        dbgln("dhe_rsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    m_context.server_diffie_hellman_params.g = g_result.release_value();

    auto dh_Ys_length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(7 + dh_p_length + dh_g_length)));
    auto dh_Ys = buffer.slice(9 + dh_p_length + dh_g_length, dh_Ys_length);
    auto Ys_result = ByteBuffer::copy(dh_Ys);
    if (Ys_result.is_error()) {
        dbgln("dhe_rsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    m_context.server_diffie_hellman_params.Ys = Ys_result.release_value();

    if constexpr (TLS_DEBUG) {
        dbgln("dh_p: {:hex-dump}", dh_p);
        dbgln("dh_g: {:hex-dump}", dh_g);
        dbgln("dh_Ys: {:hex-dump}", dh_Ys);
    }

    auto server_key_info = buffer.slice(3, 6 + dh_p_length + dh_g_length + dh_Ys_length);
    auto signature = buffer.slice(9 + dh_p_length + dh_g_length + dh_Ys_length);
    return verify_rsa_server_key_exchange(server_key_info, signature);
}

ssize_t TLSv12::handle_ecdhe_server_key_exchange(ReadonlyBytes buffer, u8& server_public_key_length)
{
    if (buffer.size() < 7)
        return (i8)Error::NeedMoreData;

    auto curve_type = buffer[3];
    if (curve_type != (u8)ECCurveType::NAMED_CURVE)
        return (i8)Error::NotUnderstood;

    auto curve = static_cast<SupportedGroup>(AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(4))));
    if (!m_context.options.elliptic_curves.contains_slow(curve))
        return (i8)Error::NotUnderstood;

    switch ((SupportedGroup)curve) {
    case SupportedGroup::X25519:
        m_context.server_key_exchange_curve = make<Crypto::Curves::X25519>();
        break;
    case SupportedGroup::X448:
        m_context.server_key_exchange_curve = make<Crypto::Curves::X448>();
        break;
    case SupportedGroup::SECP256R1:
        m_context.server_key_exchange_curve = make<Crypto::Curves::SECP256r1>();
        break;
    case SupportedGroup::SECP384R1:
        m_context.server_key_exchange_curve = make<Crypto::Curves::SECP384r1>();
        break;
    default:
        return (i8)Error::NotUnderstood;
    }

    server_public_key_length = buffer[6];
    if (server_public_key_length != m_context.server_key_exchange_curve->key_size())
        return (i8)Error::NotUnderstood;

    if (buffer.size() < 7u + server_public_key_length)
        return (i8)Error::NeedMoreData;

    auto server_public_key = buffer.slice(7, server_public_key_length);
    auto server_public_key_copy_result = ByteBuffer::copy(server_public_key);
    if (server_public_key_copy_result.is_error()) {
        dbgln("ecdhe_rsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    m_context.server_diffie_hellman_params.p = server_public_key_copy_result.release_value();

    if constexpr (TLS_DEBUG) {
        dbgln("ECDHE server public key: {:hex-dump}", server_public_key);
    }

    return 0;
}

ssize_t TLSv12::handle_ecdhe_rsa_server_key_exchange(ReadonlyBytes buffer)
{
    u8 server_public_key_length;
    if (auto result = handle_ecdhe_server_key_exchange(buffer, server_public_key_length)) {
        return result;
    }

    auto server_key_info = buffer.slice(3, 4 + server_public_key_length);
    auto signature = buffer.slice(7 + server_public_key_length);
    return verify_rsa_server_key_exchange(server_key_info, signature);
}

ssize_t TLSv12::verify_rsa_server_key_exchange(ReadonlyBytes server_key_info_buffer, ReadonlyBytes signature_buffer)
{
    auto signature_hash = signature_buffer[0];
    auto signature_algorithm = static_cast<SignatureAlgorithm>(signature_buffer[1]);
    if (signature_algorithm != SignatureAlgorithm::RSA) {
        dbgln("verify_rsa_server_key_exchange failed: Signature algorithm is not RSA, instead {}", enum_to_string(signature_algorithm));
        return (i8)Error::NotUnderstood;
    }

    auto signature_length = AK::convert_between_host_and_network_endian(ByteReader::load16(signature_buffer.offset_pointer(2)));
    auto signature = signature_buffer.slice(4, signature_length);

    if (m_context.certificates.is_empty()) {
        dbgln("verify_rsa_server_key_exchange failed: Attempting to verify signature without certificates");
        return (i8)Error::NotSafe;
    }
    // RFC5246 section 7.4.2: The sender's certificate MUST come first in the list.
    auto certificate_public_key = m_context.certificates.first().public_key;
    Crypto::PK::RSAPrivateKey dummy_private_key;
    auto rsa = Crypto::PK::RSA(certificate_public_key.rsa, dummy_private_key);

    auto signature_verify_buffer_result = ByteBuffer::create_uninitialized(signature_length);
    if (signature_verify_buffer_result.is_error()) {
        dbgln("verify_rsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    auto signature_verify_buffer = signature_verify_buffer_result.release_value();
    auto signature_verify_bytes = signature_verify_buffer.bytes();
    rsa.verify(signature, signature_verify_bytes);

    auto message_result = ByteBuffer::create_uninitialized(64 + server_key_info_buffer.size());
    if (message_result.is_error()) {
        dbgln("verify_rsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    auto message = message_result.release_value();
    message.overwrite(0, m_context.local_random, 32);
    message.overwrite(32, m_context.remote_random, 32);
    message.overwrite(64, server_key_info_buffer.data(), server_key_info_buffer.size());

    Crypto::Hash::HashKind hash_kind;
    switch ((HashAlgorithm)signature_hash) {
    case HashAlgorithm::SHA1:
        hash_kind = Crypto::Hash::HashKind::SHA1;
        break;
    case HashAlgorithm::SHA256:
        hash_kind = Crypto::Hash::HashKind::SHA256;
        break;
    case HashAlgorithm::SHA384:
        hash_kind = Crypto::Hash::HashKind::SHA384;
        break;
    case HashAlgorithm::SHA512:
        hash_kind = Crypto::Hash::HashKind::SHA512;
        break;
    default:
        dbgln("verify_rsa_server_key_exchange failed: Hash algorithm is not SHA1/256/384/512, instead {}", signature_hash);
        return (i8)Error::NotUnderstood;
    }

    auto pkcs1 = Crypto::PK::EMSA_PKCS1_V1_5<Crypto::Hash::Manager>(hash_kind);
    auto verification = pkcs1.verify(message, signature_verify_bytes, signature_length * 8);

    if (verification == Crypto::VerificationConsistency::Inconsistent) {
        dbgln("verify_rsa_server_key_exchange failed: Verification of signature inconsistent");
        return (i8)Error::NotSafe;
    }

    return 0;
}

ssize_t TLSv12::handle_ecdhe_ecdsa_server_key_exchange(ReadonlyBytes buffer)
{
    u8 server_public_key_length;
    if (auto result = handle_ecdhe_server_key_exchange(buffer, server_public_key_length)) {
        return result;
    }

    auto server_key_info = buffer.slice(3, 4 + server_public_key_length);
    auto signature = buffer.slice(7 + server_public_key_length);
    return verify_ecdsa_server_key_exchange(server_key_info, signature);
}

ssize_t TLSv12::verify_ecdsa_server_key_exchange(ReadonlyBytes server_key_info_buffer, ReadonlyBytes signature_buffer)
{
    auto signature_hash = signature_buffer[0];
    auto signature_algorithm = signature_buffer[1];
    if (signature_algorithm != (u8)SignatureAlgorithm::ECDSA) {
        dbgln("verify_ecdsa_server_key_exchange failed: Signature algorithm is not ECDSA, instead {}", signature_algorithm);
        return (i8)Error::NotUnderstood;
    }

    auto signature_length = AK::convert_between_host_and_network_endian(ByteReader::load16(signature_buffer.offset_pointer(2)));
    auto signature = signature_buffer.slice(4, signature_length);

    if (m_context.certificates.is_empty()) {
        dbgln("verify_ecdsa_server_key_exchange failed: Attempting to verify signature without certificates");
        return (i8)Error::NotSafe;
    }
    ReadonlyBytes server_point = m_context.certificates.first().public_key.raw_key;

    auto message_result = ByteBuffer::create_uninitialized(64 + server_key_info_buffer.size());
    if (message_result.is_error()) {
        dbgln("verify_ecdsa_server_key_exchange failed: Not enough memory");
        return (i8)Error::OutOfMemory;
    }
    auto message = message_result.release_value();
    message.overwrite(0, m_context.local_random, 32);
    message.overwrite(32, m_context.remote_random, 32);
    message.overwrite(64, server_key_info_buffer.data(), server_key_info_buffer.size());

    Crypto::Hash::HashKind hash_kind;
    switch ((HashAlgorithm)signature_hash) {
    case HashAlgorithm::SHA256:
        hash_kind = Crypto::Hash::HashKind::SHA256;
        break;
    case HashAlgorithm::SHA384:
        hash_kind = Crypto::Hash::HashKind::SHA384;
        break;
    case HashAlgorithm::SHA512:
        hash_kind = Crypto::Hash::HashKind::SHA512;
        break;
    default:
        dbgln("verify_ecdsa_server_key_exchange failed: Hash algorithm is not SHA256/384/512, instead {}", signature_hash);
        return (i8)Error::NotUnderstood;
    }

    ErrorOr<bool> res = AK::Error::from_errno(ENOTSUP);
    auto& public_key = m_context.certificates.first().public_key;
    switch (public_key.algorithm.ec_parameters) {
    case SupportedGroup::SECP256R1: {
        Crypto::Hash::Manager manager(hash_kind);
        manager.update(message);
        auto digest = manager.digest();

        Crypto::Curves::SECP256r1 curve;
        res = curve.verify(digest.bytes(), server_point, signature);
        break;
    }
    case SupportedGroup::SECP384R1: {
        Crypto::Hash::Manager manager(hash_kind);
        manager.update(message);
        auto digest = manager.digest();

        Crypto::Curves::SECP384r1 curve;
        res = curve.verify(digest.bytes(), server_point, signature);
        break;
    }
    default: {
        dbgln("verify_ecdsa_server_key_exchange failed: Server certificate public key algorithm is not supported: {}", to_underlying(public_key.algorithm.ec_parameters));
        break;
    }
    }

    if (res.is_error()) {
        dbgln("verify_ecdsa_server_key_exchange failed: {}", res.error());
        return (i8)Error::NotUnderstood;
    }

    bool verification_ok = res.release_value();
    if (!verification_ok) {
        dbgln("verify_ecdsa_server_key_exchange failed: Verification of signature failed");
        return (i8)Error::NotSafe;
    }

    return 0;
}

}
