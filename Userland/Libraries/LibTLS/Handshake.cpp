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
#include <LibTLS/TLSv12.h>

namespace TLS {

ByteBuffer TLSv12::build_hello()
{
    fill_with_random(m_context.local_random);

    auto packet_version = (u16)m_context.options.version;
    auto version = (u16)m_context.options.version;
    PacketBuilder builder { ContentType::HANDSHAKE, packet_version };

    builder.append(to_underlying(HandshakeType::CLIENT_HELLO));

    // hello length (for later)
    u8 dummy[3] = {};
    builder.append(dummy, 3);

    auto start_length = builder.length();

    builder.append(version);
    builder.append(m_context.local_random, sizeof(m_context.local_random));

    builder.append(m_context.session_id_size);
    if (m_context.session_id_size)
        builder.append(m_context.session_id, m_context.session_id_size);

    size_t extension_length = 0;
    size_t alpn_length = 0;
    size_t alpn_negotiated_length = 0;

    // ALPN
    if (!m_context.negotiated_alpn.is_empty()) {
        alpn_negotiated_length = m_context.negotiated_alpn.length();
        alpn_length = alpn_negotiated_length + 1;
        extension_length += alpn_length + 6;
    } else if (m_context.alpn.size()) {
        for (auto& alpn : m_context.alpn) {
            size_t length = alpn.length();
            alpn_length += length + 1;
        }
        if (alpn_length)
            extension_length += alpn_length + 6;
    }

    // Ciphers
    builder.append((u16)(m_context.options.usable_cipher_suites.size() * sizeof(u16)));
    for (auto suite : m_context.options.usable_cipher_suites)
        builder.append((u16)suite);

    // we don't like compression
    VERIFY(!m_context.options.use_compression);
    builder.append((u8)1);
    builder.append((u8)m_context.options.use_compression);

    // set SNI if we have one, and the user hasn't explicitly asked us to omit it.
    auto sni_length = 0;
    if (!m_context.extensions.SNI.is_empty() && m_context.options.use_sni)
        sni_length = m_context.extensions.SNI.length();

    auto elliptic_curves_length = 2 * m_context.options.elliptic_curves.size();
    auto supported_ec_point_formats_length = m_context.options.supported_ec_point_formats.size();
    bool supports_elliptic_curves = elliptic_curves_length && supported_ec_point_formats_length;
    bool enable_extended_master_secret = m_context.options.enable_extended_master_secret;

    // signature_algorithms: 2b extension ID, 2b extension length, 2b vector length, 2xN signatures and hashes
    extension_length += 2 + 2 + 2 + 2 * m_context.options.supported_signature_algorithms.size();

    if (sni_length)
        extension_length += sni_length + 9;

    // Only send elliptic_curves and ec_point_formats extensions if both are supported
    if (supports_elliptic_curves)
        extension_length += 6 + elliptic_curves_length + 5 + supported_ec_point_formats_length;

    if (enable_extended_master_secret)
        extension_length += 4;

    builder.append((u16)extension_length);

    if (sni_length) {
        // SNI extension
        builder.append((u16)ExtensionType::SERVER_NAME);
        // extension length
        builder.append((u16)(sni_length + 5));
        // SNI length
        builder.append((u16)(sni_length + 3));
        // SNI type
        builder.append((u8)0);
        // SNI host length + value
        builder.append((u16)sni_length);
        builder.append((u8 const*)m_context.extensions.SNI.characters(), sni_length);
    }

    // signature_algorithms extension
    builder.append((u16)ExtensionType::SIGNATURE_ALGORITHMS);
    // Extension length
    builder.append((u16)(2 + 2 * m_context.options.supported_signature_algorithms.size()));
    // Vector count
    builder.append((u16)(m_context.options.supported_signature_algorithms.size() * 2));
    // Entries
    for (auto& entry : m_context.options.supported_signature_algorithms) {
        builder.append((u8)entry.hash);
        builder.append((u8)entry.signature);
    }

    if (supports_elliptic_curves) {
        // elliptic_curves extension
        builder.append((u16)ExtensionType::SUPPORTED_GROUPS);
        builder.append((u16)(2 + elliptic_curves_length));
        builder.append((u16)elliptic_curves_length);
        for (auto& curve : m_context.options.elliptic_curves)
            builder.append((u16)curve);

        // ec_point_formats extension
        builder.append((u16)ExtensionType::EC_POINT_FORMATS);
        builder.append((u16)(1 + supported_ec_point_formats_length));
        builder.append((u8)supported_ec_point_formats_length);
        for (auto& format : m_context.options.supported_ec_point_formats)
            builder.append((u8)format);
    }

    if (enable_extended_master_secret) {
        // extended_master_secret extension
        builder.append((u16)ExtensionType::EXTENDED_MASTER_SECRET);
        builder.append((u16)0);
    }

    if (alpn_length) {
        // TODO
        VERIFY_NOT_REACHED();
    }

    // set the "length" field of the packet
    size_t remaining = builder.length() - start_length;
    size_t payload_position = 6;
    builder.set(payload_position, remaining / 0x10000);
    remaining %= 0x10000;
    builder.set(payload_position + 1, remaining / 0x100);
    remaining %= 0x100;
    builder.set(payload_position + 2, remaining);

    auto packet = builder.build();
    update_packet(packet);

    return packet;
}

ByteBuffer TLSv12::build_change_cipher_spec()
{
    PacketBuilder builder { ContentType::CHANGE_CIPHER_SPEC, m_context.options.version, 64 };
    builder.append((u8)1);
    auto packet = builder.build();
    update_packet(packet);
    m_context.local_sequence_number = 0;
    return packet;
}

ByteBuffer TLSv12::build_handshake_finished()
{
    PacketBuilder builder { ContentType::HANDSHAKE, m_context.options.version, 12 + 64 };
    builder.append((u8)HandshakeType::FINISHED);

    // RFC 5246 section 7.4.9: "In previous versions of TLS, the verify_data was always 12 octets
    //                          long.  In the current version of TLS, it depends on the cipher
    //                          suite.  Any cipher suite which does not explicitly specify
    //                          verify_data_length has a verify_data_length equal to 12."
    // Simplification: Assume that verify_data_length is always 12.
    constexpr u32 verify_data_length = 12;

    builder.append_u24(verify_data_length);

    u8 out[verify_data_length];
    auto outbuffer = Bytes { out, verify_data_length };
    ByteBuffer dummy;

    auto digest = m_context.handshake_hash.digest();
    auto hashbuf = ReadonlyBytes { digest.immutable_data(), m_context.handshake_hash.digest_size() };
    pseudorandom_function(outbuffer, m_context.master_key, (u8 const*)"client finished", 15, hashbuf, dummy);

    builder.append(outbuffer);
    auto packet = builder.build();
    update_packet(packet);

    return packet;
}

ssize_t TLSv12::handle_handshake_finished(ReadonlyBytes buffer, WritePacketStage& write_packets)
{
    if (m_context.connection_status < ConnectionStatus::KeyExchange || m_context.connection_status == ConnectionStatus::Established) {
        dbgln("unexpected finished message");
        return (i8)Error::UnexpectedMessage;
    }

    write_packets = WritePacketStage::Initial;

    if (buffer.size() < 3) {
        return (i8)Error::NeedMoreData;
    }

    size_t index = 3;

    u32 size = buffer[0] * 0x10000 + buffer[1] * 0x100 + buffer[2];

    if (size < 12) {
        dbgln_if(TLS_DEBUG, "finished packet smaller than minimum size: {}", size);
        return (i8)Error::BrokenPacket;
    }

    if (size < buffer.size() - index) {
        dbgln_if(TLS_DEBUG, "not enough data after length: {} > {}", size, buffer.size() - index);
        return (i8)Error::NeedMoreData;
    }

    // TODO: Compare Hashes
    dbgln_if(TLS_DEBUG, "FIXME: handle_handshake_finished :: Check message validity");
    m_context.connection_status = ConnectionStatus::Established;

    if (m_handshake_timeout_timer) {
        // Disable the handshake timeout timer as handshake has been established.
        m_handshake_timeout_timer->stop();
        m_handshake_timeout_timer->remove_from_parent();
        m_handshake_timeout_timer = nullptr;
    }

    if (on_connected)
        on_connected();

    return index + size;
}

ssize_t TLSv12::handle_handshake_payload(ReadonlyBytes vbuffer)
{
    if (m_context.connection_status == ConnectionStatus::Established) {
        dbgln_if(TLS_DEBUG, "Renegotiation attempt ignored");
        // FIXME: We should properly say "NoRenegotiation", but that causes a handshake failure
        //        so we just roll with it and pretend that we _did_ renegotiate
        //        This will cause issues when we decide to have long-lasting connections, but
        //        we do not have those at the moment :^)
        return 1;
    }
    auto buffer = vbuffer;
    auto buffer_length = buffer.size();
    auto original_length = buffer_length;
    while (buffer_length >= 4 && !m_context.critical_error) {
        ssize_t payload_res = 0;
        if (buffer_length < 1)
            return (i8)Error::NeedMoreData;
        auto type = static_cast<HandshakeType>(buffer[0]);
        auto write_packets { WritePacketStage::Initial };
        size_t payload_size = buffer[1] * 0x10000 + buffer[2] * 0x100 + buffer[3] + 3;
        dbgln_if(TLS_DEBUG, "payload size: {} buffer length: {}", payload_size, buffer_length);
        if (payload_size + 1 > buffer_length)
            return (i8)Error::NeedMoreData;

        switch (type) {
        case HandshakeType::HELLO_REQUEST_RESERVED:
            if (m_context.handshake_messages[0] >= 1) {
                dbgln("unexpected hello request message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[0];
            dbgln("hello request (renegotiation?)");
            if (m_context.connection_status == ConnectionStatus::Established) {
                // renegotiation
                payload_res = (i8)Error::NoRenegotiation;
            } else {
                // :shrug:
                payload_res = (i8)Error::UnexpectedMessage;
            }
            break;
        case HandshakeType::CLIENT_HELLO:
            // FIXME: We only support client mode right now
            if (m_context.is_server) {
                VERIFY_NOT_REACHED();
            }
            payload_res = (i8)Error::UnexpectedMessage;
            break;
        case HandshakeType::SERVER_HELLO:
            if (m_context.handshake_messages[2] >= 1) {
                dbgln("unexpected server hello message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[2];
            dbgln_if(TLS_DEBUG, "server hello");
            if (m_context.is_server) {
                dbgln("unsupported: server mode");
                VERIFY_NOT_REACHED();
            }
            payload_res = handle_server_hello(buffer.slice(1, payload_size), write_packets);
            break;
        case HandshakeType::HELLO_VERIFY_REQUEST_RESERVED:
            dbgln("unsupported: DTLS");
            payload_res = (i8)Error::UnexpectedMessage;
            break;
        case HandshakeType::CERTIFICATE:
            if (m_context.handshake_messages[4] >= 1) {
                dbgln("unexpected certificate message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[4];
            dbgln_if(TLS_DEBUG, "certificate");
            if (m_context.connection_status == ConnectionStatus::Negotiating) {
                if (m_context.is_server) {
                    dbgln("unsupported: server mode");
                    VERIFY_NOT_REACHED();
                }
                payload_res = handle_certificate(buffer.slice(1, payload_size));
            } else {
                payload_res = (i8)Error::UnexpectedMessage;
            }
            break;
        case HandshakeType::SERVER_KEY_EXCHANGE_RESERVED:
            if (m_context.handshake_messages[5] >= 1) {
                dbgln("unexpected server key exchange message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[5];
            dbgln_if(TLS_DEBUG, "server key exchange");
            if (m_context.is_server) {
                dbgln("unsupported: server mode");
                VERIFY_NOT_REACHED();
            } else {
                payload_res = handle_server_key_exchange(buffer.slice(1, payload_size));
            }
            break;
        case HandshakeType::CERTIFICATE_REQUEST:
            if (m_context.handshake_messages[6] >= 1) {
                dbgln("unexpected certificate request message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[6];
            if (m_context.is_server) {
                dbgln("invalid request");
                dbgln("unsupported: server mode");
                VERIFY_NOT_REACHED();
            } else {
                // we do not support "certificate request"
                dbgln("certificate request");
                if (on_tls_certificate_request)
                    on_tls_certificate_request(*this);
                m_context.client_verified = VerificationNeeded;
            }
            break;
        case HandshakeType::SERVER_HELLO_DONE_RESERVED:
            if (m_context.handshake_messages[7] >= 1) {
                dbgln("unexpected server hello done message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[7];
            dbgln_if(TLS_DEBUG, "server hello done");
            if (m_context.is_server) {
                dbgln("unsupported: server mode");
                VERIFY_NOT_REACHED();
            } else {
                payload_res = handle_server_hello_done(buffer.slice(1, payload_size));
                if (payload_res > 0)
                    write_packets = WritePacketStage::ClientHandshake;
            }
            break;
        case HandshakeType::CERTIFICATE_VERIFY:
            if (m_context.handshake_messages[8] >= 1) {
                dbgln("unexpected certificate verify message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[8];
            dbgln_if(TLS_DEBUG, "certificate verify");
            if (m_context.connection_status == ConnectionStatus::KeyExchange) {
                payload_res = handle_certificate_verify(buffer.slice(1, payload_size));
            } else {
                payload_res = (i8)Error::UnexpectedMessage;
            }
            break;
        case HandshakeType::CLIENT_KEY_EXCHANGE_RESERVED:
            if (m_context.handshake_messages[9] >= 1) {
                dbgln("unexpected client key exchange message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[9];
            dbgln_if(TLS_DEBUG, "client key exchange");
            if (m_context.is_server) {
                dbgln("unsupported: server mode");
                VERIFY_NOT_REACHED();
            } else {
                payload_res = (i8)Error::UnexpectedMessage;
            }
            break;
        case HandshakeType::FINISHED:
            m_context.cached_handshake.clear();
            if (m_context.handshake_messages[10] >= 1) {
                dbgln("unexpected finished message");
                payload_res = (i8)Error::UnexpectedMessage;
                break;
            }
            ++m_context.handshake_messages[10];
            dbgln_if(TLS_DEBUG, "finished");
            payload_res = handle_handshake_finished(buffer.slice(1, payload_size), write_packets);
            if (payload_res > 0) {
                memset(m_context.handshake_messages, 0, sizeof(m_context.handshake_messages));
            }
            break;
        default:
            dbgln("message type not understood: {}", enum_to_string(type));
            return (i8)Error::NotUnderstood;
        }

        if (type != HandshakeType::HELLO_REQUEST_RESERVED) {
            update_hash(buffer.slice(0, payload_size + 1), 0);
        }

        // if something went wrong, send an alert about it
        if (payload_res < 0) {
            switch ((Error)payload_res) {
            case Error::UnexpectedMessage: {
                auto packet = build_alert(true, (u8)AlertDescription::UNEXPECTED_MESSAGE);
                write_packet(packet);
                break;
            }
            case Error::CompressionNotSupported: {
                auto packet = build_alert(true, (u8)AlertDescription::DECOMPRESSION_FAILURE_RESERVED);
                write_packet(packet);
                break;
            }
            case Error::BrokenPacket: {
                auto packet = build_alert(true, (u8)AlertDescription::DECODE_ERROR);
                write_packet(packet);
                break;
            }
            case Error::NotVerified: {
                auto packet = build_alert(true, (u8)AlertDescription::BAD_RECORD_MAC);
                write_packet(packet);
                break;
            }
            case Error::BadCertificate: {
                auto packet = build_alert(true, (u8)AlertDescription::BAD_CERTIFICATE);
                write_packet(packet);
                break;
            }
            case Error::UnsupportedCertificate: {
                auto packet = build_alert(true, (u8)AlertDescription::UNSUPPORTED_CERTIFICATE);
                write_packet(packet);
                break;
            }
            case Error::NoCommonCipher: {
                auto packet = build_alert(true, (u8)AlertDescription::INSUFFICIENT_SECURITY);
                write_packet(packet);
                break;
            }
            case Error::NotUnderstood:
            case Error::OutOfMemory: {
                auto packet = build_alert(true, (u8)AlertDescription::INTERNAL_ERROR);
                write_packet(packet);
                break;
            }
            case Error::NoRenegotiation: {
                auto packet = build_alert(true, (u8)AlertDescription::NO_RENEGOTIATION_RESERVED);
                write_packet(packet);
                break;
            }
            case Error::DecryptionFailed: {
                auto packet = build_alert(true, (u8)AlertDescription::DECRYPTION_FAILED_RESERVED);
                write_packet(packet);
                break;
            }
            case Error::NotSafe: {
                auto packet = build_alert(true, (u8)AlertDescription::DECRYPT_ERROR);
                write_packet(packet);
                break;
            }
            case Error::NeedMoreData:
                // Ignore this, as it's not an "error"
                dbgln_if(TLS_DEBUG, "More data needed");
                break;
            default:
                dbgln("Unknown TLS::Error with value {}", payload_res);
                VERIFY_NOT_REACHED();
                break;
            }
            if (payload_res < 0)
                return payload_res;
        }
        switch (write_packets) {
        case WritePacketStage::Initial:
            // nothing to write
            break;
        case WritePacketStage::ClientHandshake:
            if (m_context.client_verified == VerificationNeeded) {
                dbgln_if(TLS_DEBUG, "> Client Certificate");
                auto packet = build_certificate();
                write_packet(packet);
                m_context.client_verified = Verified;
            }
            {
                dbgln_if(TLS_DEBUG, "> Key exchange");
                auto packet = build_client_key_exchange();
                write_packet(packet);
            }
            {
                dbgln_if(TLS_DEBUG, "> change cipher spec");
                auto packet = build_change_cipher_spec();
                write_packet(packet);
            }
            m_context.cipher_spec_set = 1;
            m_context.local_sequence_number = 0;
            {
                dbgln_if(TLS_DEBUG, "> client finished");
                auto packet = build_handshake_finished();
                write_packet(packet);
            }
            m_context.cipher_spec_set = 0;
            break;
        case WritePacketStage::ServerHandshake:
            // server handshake
            dbgln("UNSUPPORTED: Server mode");
            VERIFY_NOT_REACHED();
            break;
        case WritePacketStage::Finished:
            // finished
            {
                dbgln_if(TLS_DEBUG, "> change cipher spec");
                auto packet = build_change_cipher_spec();
                write_packet(packet);
            }
            {
                dbgln_if(TLS_DEBUG, "> client finished");
                auto packet = build_handshake_finished();
                write_packet(packet);
            }
            m_context.connection_status = ConnectionStatus::Established;
            break;
        }
        payload_size++;
        buffer_length -= payload_size;
        buffer = buffer.slice(payload_size, buffer_length);
    }
    return original_length;
}
}
