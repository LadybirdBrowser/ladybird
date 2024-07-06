/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTLS/TLSv12.h>

namespace TLS {

ByteBuffer TLSv12::build_alert(bool critical, u8 code)
{
    PacketBuilder builder(ContentType::ALERT, (u16)m_context.options.version);
    builder.append((u8)(critical ? AlertLevel::FATAL : AlertLevel::WARNING));
    builder.append(code);

    if (critical)
        m_context.critical_error = code;

    auto packet = builder.build();
    update_packet(packet);

    return packet;
}

void TLSv12::alert(AlertLevel level, AlertDescription code)
{
    auto the_alert = build_alert(level == AlertLevel::FATAL, (u8)code);
    write_packet(the_alert, true);
    MUST(flush());
}

void TLSv12::write_packet(ByteBuffer& packet, bool immediately)
{
    auto schedule_or_perform_flush = [&](bool immediate) {
        if (m_context.connection_status > ConnectionStatus::Disconnected) {
            if (!m_has_scheduled_write_flush && !immediate) {
                dbgln_if(TLS_DEBUG, "Scheduling write of {}", m_context.tls_buffer.size());
                Core::deferred_invoke([this] { write_into_socket(); });
                m_has_scheduled_write_flush = true;
            } else {
                // multiple packet are available, let's flush some out
                dbgln_if(TLS_DEBUG, "Flushing scheduled write of {}", m_context.tls_buffer.size());
                write_into_socket();
                // the deferred invoke is still in place
                m_has_scheduled_write_flush = true;
            }
        }
    };
    // Record size limit is 18432 bytes, leave some headroom and flush at 16K.
    if (m_context.tls_buffer.size() + packet.size() > 16 * KiB)
        schedule_or_perform_flush(true);

    if (m_context.tls_buffer.try_append(packet.data(), packet.size()).is_error()) {
        // Toooooo bad, drop the record on the ground.
        return;
    }
    schedule_or_perform_flush(immediately);
}

void TLSv12::update_packet(ByteBuffer& packet)
{
    u32 header_size = 5;
    ByteReader::store(packet.offset_pointer(3), AK::convert_between_host_and_network_endian((u16)(packet.size() - header_size)));

    if (packet[0] != (u8)ContentType::CHANGE_CIPHER_SPEC) {
        if (packet[0] == (u8)ContentType::HANDSHAKE && packet.size() > header_size) {
            auto handshake_type = static_cast<HandshakeType>(packet[header_size]);
            if (handshake_type != HandshakeType::HELLO_REQUEST_RESERVED && handshake_type != HandshakeType::HELLO_VERIFY_REQUEST_RESERVED) {
                update_hash(packet.bytes(), header_size);
            }
        }
        if (m_context.cipher_spec_set && m_context.crypto.created) {
            size_t length = packet.size() - header_size;
            size_t block_size = 0;
            size_t padding = 0;
            size_t mac_size = 0;

            m_cipher_local.visit(
                [&](Empty&) { VERIFY_NOT_REACHED(); },
                [&](Crypto::Cipher::AESCipher::GCMMode& gcm) {
                    VERIFY(is_aead());
                    block_size = gcm.cipher().block_size();
                    padding = 0;
                    mac_size = 0; // AEAD provides its own authentication scheme.
                },
                [&](Crypto::Cipher::AESCipher::CBCMode& cbc) {
                    VERIFY(!is_aead());
                    block_size = cbc.cipher().block_size();
                    // If the length is already a multiple a block_size,
                    // an entire block of padding is added.
                    // In short, we _never_ have no padding.
                    mac_size = mac_length();
                    length += mac_size;
                    padding = block_size - length % block_size;
                    length += padding;
                });

            if (m_context.crypto.created == 1) {
                // `buffer' will continue to be encrypted
                auto buffer_result = ByteBuffer::create_uninitialized(length);
                if (buffer_result.is_error()) {
                    dbgln("LibTLS: Failed to allocate enough memory");
                    VERIFY_NOT_REACHED();
                }
                auto buffer = buffer_result.release_value();
                size_t buffer_position = 0;
                auto iv_size = iv_length();

                // copy the packet, sans the header
                buffer.overwrite(buffer_position, packet.offset_pointer(header_size), packet.size() - header_size);
                buffer_position += packet.size() - header_size;

                ByteBuffer ct;

                m_cipher_local.visit(
                    [&](Empty&) { VERIFY_NOT_REACHED(); },
                    [&](Crypto::Cipher::AESCipher::GCMMode& gcm) {
                        VERIFY(is_aead());
                        // We need enough space for a header, the data, a tag, and the IV
                        auto ct_buffer_result = ByteBuffer::create_uninitialized(length + header_size + iv_size + 16);
                        if (ct_buffer_result.is_error()) {
                            dbgln("LibTLS: Failed to allocate enough memory for the ciphertext");
                            VERIFY_NOT_REACHED();
                        }
                        ct = ct_buffer_result.release_value();

                        // copy the header over
                        ct.overwrite(0, packet.data(), header_size - 2);

                        // AEAD AAD (13)
                        // Seq. no (8)
                        // content type (1)
                        // version (2)
                        // length (2)
                        u8 aad[13];
                        Bytes aad_bytes { aad, 13 };
                        FixedMemoryStream aad_stream { aad_bytes };

                        u64 seq_no = AK::convert_between_host_and_network_endian(m_context.local_sequence_number);
                        u16 len = AK::convert_between_host_and_network_endian((u16)(packet.size() - header_size));

                        MUST(aad_stream.write_value(seq_no));                              // sequence number
                        MUST(aad_stream.write_until_depleted(packet.bytes().slice(0, 3))); // content-type + version
                        MUST(aad_stream.write_value(len));                                 // length
                        VERIFY(MUST(aad_stream.tell()) == MUST(aad_stream.size()));

                        // AEAD IV (12)
                        // IV (4)
                        // (Nonce) (8)
                        // -- Our GCM impl takes 16 bytes
                        // zero (4)
                        u8 iv[16];
                        Bytes iv_bytes { iv, 16 };
                        Bytes { m_context.crypto.local_aead_iv, 4 }.copy_to(iv_bytes);
                        fill_with_random(iv_bytes.slice(4, 8));
                        memset(iv_bytes.offset(12), 0, 4);

                        // write the random part of the iv out
                        iv_bytes.slice(4, 8).copy_to(ct.bytes().slice(header_size));

                        // Write the encrypted data and the tag
                        gcm.encrypt(
                            packet.bytes().slice(header_size, length),
                            ct.bytes().slice(header_size + 8, length),
                            iv_bytes,
                            aad_bytes,
                            ct.bytes().slice(header_size + 8 + length, 16));

                        VERIFY(header_size + 8 + length + 16 == ct.size());
                    },
                    [&](Crypto::Cipher::AESCipher::CBCMode& cbc) {
                        VERIFY(!is_aead());
                        // We need enough space for a header, iv_length bytes of IV and whatever the packet contains
                        auto ct_buffer_result = ByteBuffer::create_uninitialized(length + header_size + iv_size);
                        if (ct_buffer_result.is_error()) {
                            dbgln("LibTLS: Failed to allocate enough memory for the ciphertext");
                            VERIFY_NOT_REACHED();
                        }
                        ct = ct_buffer_result.release_value();

                        // copy the header over
                        ct.overwrite(0, packet.data(), header_size - 2);

                        // get the appropriate HMAC value for the entire packet
                        auto mac = hmac_message(packet, {}, mac_size, true);

                        // write the MAC
                        buffer.overwrite(buffer_position, mac.data(), mac.size());
                        buffer_position += mac.size();

                        // Apply the padding (a packet MUST always be padded)
                        memset(buffer.offset_pointer(buffer_position), padding - 1, padding);
                        buffer_position += padding;

                        VERIFY(buffer_position == buffer.size());

                        auto iv_buffer_result = ByteBuffer::create_uninitialized(iv_size);
                        if (iv_buffer_result.is_error()) {
                            dbgln("LibTLS: Failed to allocate memory for IV");
                            VERIFY_NOT_REACHED();
                        }
                        auto iv = iv_buffer_result.release_value();
                        fill_with_random(iv);

                        // write it into the ciphertext portion of the message
                        ct.overwrite(header_size, iv.data(), iv.size());

                        VERIFY(header_size + iv_size + length == ct.size());
                        VERIFY(length % block_size == 0);

                        // get a block to encrypt into
                        auto view = ct.bytes().slice(header_size + iv_size, length);
                        cbc.encrypt(buffer, view, iv);
                    });

                // store the correct ciphertext length into the packet
                u16 ct_length = (u16)ct.size() - header_size;

                ByteReader::store(ct.offset_pointer(header_size - 2), AK::convert_between_host_and_network_endian(ct_length));

                // replace the packet with the ciphertext
                packet = ct;
            }
        }
    }
    ++m_context.local_sequence_number;
}

void TLSv12::update_hash(ReadonlyBytes message, size_t header_size)
{
    dbgln_if(TLS_DEBUG, "Update hash with message of size {}", message.size());
    m_context.handshake_hash.update(message.slice(header_size));
}

void TLSv12::ensure_hmac(size_t digest_size, bool local)
{
    if (local && m_hmac_local)
        return;

    if (!local && m_hmac_remote)
        return;

    auto hash_kind = Crypto::Hash::HashKind::None;

    switch (digest_size) {
    case Crypto::Hash::SHA1::DigestSize:
        hash_kind = Crypto::Hash::HashKind::SHA1;
        break;
    case Crypto::Hash::SHA256::DigestSize:
        hash_kind = Crypto::Hash::HashKind::SHA256;
        break;
    case Crypto::Hash::SHA384::DigestSize:
        hash_kind = Crypto::Hash::HashKind::SHA384;
        break;
    case Crypto::Hash::SHA512::DigestSize:
        hash_kind = Crypto::Hash::HashKind::SHA512;
        break;
    default:
        dbgln("Failed to find a suitable hash for size {}", digest_size);
        break;
    }

    auto hmac = make<Crypto::Authentication::HMAC<Crypto::Hash::Manager>>(ReadonlyBytes { local ? m_context.crypto.local_mac : m_context.crypto.remote_mac, digest_size }, hash_kind);
    if (local)
        m_hmac_local = move(hmac);
    else
        m_hmac_remote = move(hmac);
}

ByteBuffer TLSv12::hmac_message(ReadonlyBytes buf, Optional<ReadonlyBytes> const buf2, size_t mac_length, bool local)
{
    u64 sequence_number = AK::convert_between_host_and_network_endian(local ? m_context.local_sequence_number : m_context.remote_sequence_number);
    ensure_hmac(mac_length, local);
    auto& hmac = local ? *m_hmac_local : *m_hmac_remote;
    if constexpr (TLS_DEBUG) {
        dbgln("========================= PACKET DATA ==========================");
        print_buffer((u8 const*)&sequence_number, sizeof(u64));
        print_buffer(buf.data(), buf.size());
        if (buf2.has_value())
            print_buffer(buf2.value().data(), buf2.value().size());
        dbgln("========================= PACKET DATA ==========================");
    }
    hmac.update((u8 const*)&sequence_number, sizeof(u64));
    hmac.update(buf);
    if (buf2.has_value() && buf2.value().size()) {
        hmac.update(buf2.value());
    }
    auto digest = hmac.digest();
    auto mac_result = ByteBuffer::copy(digest.immutable_data(), digest.data_length());
    if (mac_result.is_error()) {
        dbgln("Failed to calculate message HMAC: Not enough memory");
        return {};
    }

    if constexpr (TLS_DEBUG) {
        dbgln("HMAC of the block for sequence number {}", sequence_number);
        print_buffer(mac_result.value());
    }

    return mac_result.release_value();
}

ssize_t TLSv12::handle_message(ReadonlyBytes buffer)
{
    auto res { 5ll };
    size_t header_size = res;
    ssize_t payload_res = 0;

    dbgln_if(TLS_DEBUG, "buffer size: {}", buffer.size());

    if (buffer.size() < 5) {
        return (i8)Error::NeedMoreData;
    }

    auto type = (ContentType)buffer[0];
    size_t buffer_position { 1 };

    // FIXME: Read the version and verify it

    if constexpr (TLS_DEBUG) {
        auto version = static_cast<ProtocolVersion>(ByteReader::load16(buffer.offset_pointer(buffer_position)));
        dbgln("type={}, version={}", enum_to_string(type), enum_to_string(version));
    }

    buffer_position += 2;

    auto length = AK::convert_between_host_and_network_endian(ByteReader::load16(buffer.offset_pointer(buffer_position)));

    dbgln_if(TLS_DEBUG, "record length: {} at offset: {}", length, buffer_position);
    buffer_position += 2;

    if (buffer_position + length > buffer.size()) {
        dbgln_if(TLS_DEBUG, "record length more than what we have: {}", buffer.size());
        return (i8)Error::NeedMoreData;
    }

    dbgln_if(TLS_DEBUG, "message type: {}, length: {}", enum_to_string(type), length);
    auto plain = buffer.slice(buffer_position, buffer.size() - buffer_position);

    ByteBuffer decrypted;

    if (m_context.cipher_spec_set && type != ContentType::CHANGE_CIPHER_SPEC) {
        if constexpr (TLS_DEBUG) {
            dbgln("Encrypted: ");
            print_buffer(buffer.slice(header_size, length));
        }

        Error return_value = Error::NoError;
        m_cipher_remote.visit(
            [&](Empty&) { VERIFY_NOT_REACHED(); },
            [&](Crypto::Cipher::AESCipher::GCMMode& gcm) {
                VERIFY(is_aead());
                if (length < 24) {
                    dbgln("Invalid packet length");
                    auto packet = build_alert(true, (u8)AlertDescription::DECRYPT_ERROR);
                    write_packet(packet);
                    return_value = Error::BrokenPacket;
                    return;
                }

                auto packet_length = length - iv_length() - 16;
                auto payload = plain;
                auto decrypted_result = ByteBuffer::create_uninitialized(packet_length);
                if (decrypted_result.is_error()) {
                    dbgln("Failed to allocate memory for the packet");
                    return_value = Error::DecryptionFailed;
                    return;
                }
                decrypted = decrypted_result.release_value();

                // AEAD AAD (13)
                // Seq. no (8)
                // content type (1)
                // version (2)
                // length (2)
                u8 aad[13];
                Bytes aad_bytes { aad, 13 };
                FixedMemoryStream aad_stream { aad_bytes };

                u64 seq_no = AK::convert_between_host_and_network_endian(m_context.remote_sequence_number);
                u16 len = AK::convert_between_host_and_network_endian((u16)packet_length);

                MUST(aad_stream.write_value(seq_no));                                    // sequence number
                MUST(aad_stream.write_until_depleted(buffer.slice(0, header_size - 2))); // content-type + version
                MUST(aad_stream.write_value(len));                                       // length
                VERIFY(MUST(aad_stream.tell()) == MUST(aad_stream.size()));

                auto nonce = payload.slice(0, iv_length());
                payload = payload.slice(iv_length());

                // AEAD IV (12)
                // IV (4)
                // (Nonce) (8)
                // -- Our GCM impl takes 16 bytes
                // zero (4)
                u8 iv[16];
                Bytes iv_bytes { iv, 16 };
                Bytes { m_context.crypto.remote_aead_iv, 4 }.copy_to(iv_bytes);
                nonce.copy_to(iv_bytes.slice(4));
                memset(iv_bytes.offset(12), 0, 4);

                auto ciphertext = payload.slice(0, payload.size() - 16);
                auto tag = payload.slice(ciphertext.size());

                auto consistency = gcm.decrypt(
                    ciphertext,
                    decrypted,
                    iv_bytes,
                    aad_bytes,
                    tag);

                if (consistency != Crypto::VerificationConsistency::Consistent) {
                    dbgln("integrity check failed (tag length {})", tag.size());
                    auto packet = build_alert(true, (u8)AlertDescription::BAD_RECORD_MAC);
                    write_packet(packet);

                    return_value = Error::IntegrityCheckFailed;
                    return;
                }

                plain = decrypted;
            },
            [&](Crypto::Cipher::AESCipher::CBCMode& cbc) {
                VERIFY(!is_aead());
                auto iv_size = iv_length();

                auto decrypted_result = cbc.create_aligned_buffer(length - iv_size);
                if (decrypted_result.is_error()) {
                    dbgln("Failed to allocate memory for the packet");
                    return_value = Error::DecryptionFailed;
                    return;
                }
                decrypted = decrypted_result.release_value();
                auto iv = buffer.slice(header_size, iv_size);

                Bytes decrypted_span = decrypted;
                cbc.decrypt(buffer.slice(header_size + iv_size, length - iv_size), decrypted_span, iv);

                length = decrypted_span.size();

                if constexpr (TLS_DEBUG) {
                    dbgln("Decrypted: ");
                    print_buffer(decrypted);
                }

                auto mac_size = mac_length();
                if (length < mac_size) {
                    dbgln("broken packet");
                    auto packet = build_alert(true, (u8)AlertDescription::DECRYPT_ERROR);
                    write_packet(packet);
                    return_value = Error::BrokenPacket;
                    return;
                }

                length -= mac_size;

                u8 const* message_hmac = decrypted_span.offset(length);
                u8 temp_buf[5];
                memcpy(temp_buf, buffer.offset_pointer(0), 3);
                *(u16*)(temp_buf + 3) = AK::convert_between_host_and_network_endian(length);
                auto hmac = hmac_message({ temp_buf, 5 }, decrypted_span.slice(0, length), mac_size);
                auto message_mac = ReadonlyBytes { message_hmac, mac_size };
                if (hmac != message_mac) {
                    dbgln("integrity check failed (mac length {})", mac_size);
                    dbgln("mac received:");
                    print_buffer(message_mac);
                    dbgln("mac computed:");
                    print_buffer(hmac);
                    auto packet = build_alert(true, (u8)AlertDescription::BAD_RECORD_MAC);
                    write_packet(packet);

                    return_value = Error::IntegrityCheckFailed;
                    return;
                }
                plain = decrypted.bytes().slice(0, length);
            });

        if (return_value != Error::NoError) {
            return (i8)return_value;
        }
    }
    m_context.remote_sequence_number++;

    switch (type) {
    case ContentType::APPLICATION_DATA:
        if (m_context.connection_status != ConnectionStatus::Established) {
            dbgln("unexpected application data");
            payload_res = (i8)Error::UnexpectedMessage;
            auto packet = build_alert(true, (u8)AlertDescription::UNEXPECTED_MESSAGE);
            write_packet(packet);
        } else {
            dbgln_if(TLS_DEBUG, "application data message of size {}", plain.size());

            if (m_context.application_buffer.try_append(plain).is_error()) {
                payload_res = (i8)Error::DecryptionFailed;
                auto packet = build_alert(true, (u8)AlertDescription::DECRYPTION_FAILED_RESERVED);
                write_packet(packet);
            } else {
                notify_client_for_app_data();
            }
        }
        break;
    case ContentType::HANDSHAKE:
        dbgln_if(TLS_DEBUG, "tls handshake message");
        payload_res = handle_handshake_payload(plain);
        break;
    case ContentType::CHANGE_CIPHER_SPEC:
        if (m_context.connection_status != ConnectionStatus::KeyExchange) {
            dbgln("unexpected change cipher message");
            auto packet = build_alert(true, (u8)AlertDescription::UNEXPECTED_MESSAGE);
            write_packet(packet);
            payload_res = (i8)Error::UnexpectedMessage;
        } else {
            dbgln_if(TLS_DEBUG, "change cipher spec message");
            m_context.cipher_spec_set = true;
            m_context.remote_sequence_number = 0;
        }
        break;
    case ContentType::ALERT:
        dbgln_if(TLS_DEBUG, "alert message of length {}", length);
        if (length >= 2) {
            if constexpr (TLS_DEBUG)
                print_buffer(plain);

            auto level = plain[0];
            auto code = plain[1];
            dbgln_if(TLS_DEBUG, "Alert received with level {}, code {}", level, code);

            if (level == (u8)AlertLevel::FATAL) {
                dbgln("We were alerted of a critical error: {} ({})", code, enum_to_string((AlertDescription)code));
                m_context.critical_error = code;
                try_disambiguate_error();
                res = (i8)Error::UnknownError;
            }

            if (code == (u8)AlertDescription::CLOSE_NOTIFY) {
                res += 2;
                alert(AlertLevel::FATAL, AlertDescription::CLOSE_NOTIFY);
                if (!m_context.cipher_spec_set) {
                    // AWS CloudFront hits this.
                    dbgln("Server sent a close notify and we haven't agreed on a cipher suite. Treating it as a handshake failure.");
                    m_context.critical_error = (u8)AlertDescription::HANDSHAKE_FAILURE;
                    try_disambiguate_error();
                }
                m_context.close_notify = true;
            }
            m_context.error_code = (Error)code;
            check_connection_state(false);
            notify_client_for_app_data(); // Give the user one more chance to observe the EOF
        }
        break;
    default:
        dbgln("message not understood");
        return (i8)Error::NotUnderstood;
    }

    if (payload_res < 0)
        return payload_res;

    if (res > 0)
        return header_size + length;

    return res;
}

}
