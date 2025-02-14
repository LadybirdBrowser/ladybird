/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IPv4Address.h>
#include <AK/Queue.h>
#include <AK/WeakPtr.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibCrypto/Authentication/HMAC.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/Curves/EllipticCurve.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibTLS/CipherSuite.h>
#include <LibTLS/TLSPacketBuilder.h>

namespace TLS {

using Crypto::Certificate::Certificate;

inline void print_buffer(ReadonlyBytes buffer)
{
    dbgln("{:hex-dump}", buffer);
}

inline void print_buffer(ByteBuffer const& buffer)
{
    print_buffer(buffer.bytes());
}

inline void print_buffer(u8 const* buffer, size_t size)
{
    print_buffer(ReadonlyBytes { buffer, size });
}

class Socket;

enum class Error : i8 {
    NoError = 0,
    UnknownError = -1,
    BrokenPacket = -2,
    NotUnderstood = -3,
    NoCommonCipher = -5,
    UnexpectedMessage = -6,
    CloseConnection = -7,
    CompressionNotSupported = -8,
    NotVerified = -9,
    NotSafe = -10,
    IntegrityCheckFailed = -11,
    ErrorAlert = -12,
    BrokenConnection = -13,
    BadCertificate = -14,
    UnsupportedCertificate = -15,
    NoRenegotiation = -16,
    FeatureNotSupported = -17,
    DecryptionFailed = -20,
    NeedMoreData = -21,
    TimedOut = -22,
    OutOfMemory = -23,
};

enum class WritePacketStage {
    Initial = 0,
    ClientHandshake = 1,
    ServerHandshake = 2,
    Finished = 3,
};

enum class ConnectionStatus {
    Disconnected,
    Negotiating,
    KeyExchange,
    Renegotiating,
    Established,
};

enum ClientVerificationStaus {
    Verified,
    VerificationNeeded,
};

// Note for the 16 iv length instead of 8:
// 4 bytes of fixed IV, 8 random (nonce) bytes, 4 bytes for counter
// GCM specifically asks us to transmit only the nonce, the counter is zero
// and the fixed IV is derived from the premaster key.
//
// The cipher suite list below is ordered based on the recommendations from Mozilla.
// When changing the supported cipher suites, please consult the webpage below for
// the preferred order.
//
// https://wiki.mozilla.org/Security/Server_Side_TLS
#define ENUMERATE_CIPHERS(C)                                                                                                                                      \
    C(true, CipherSuite::TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, KeyExchangeAlgorithm::ECDHE_ECDSA, CipherAlgorithm::AES_128_GCM, Crypto::Hash::SHA256, 8, true) \
    C(true, CipherSuite::TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, KeyExchangeAlgorithm::ECDHE_RSA, CipherAlgorithm::AES_128_GCM, Crypto::Hash::SHA256, 8, true)     \
    C(true, CipherSuite::TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, KeyExchangeAlgorithm::ECDHE_ECDSA, CipherAlgorithm::AES_256_GCM, Crypto::Hash::SHA384, 8, true) \
    C(true, CipherSuite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, KeyExchangeAlgorithm::ECDHE_RSA, CipherAlgorithm::AES_256_GCM, Crypto::Hash::SHA384, 8, true)     \
    C(true, CipherSuite::TLS_DHE_RSA_WITH_AES_128_GCM_SHA256, KeyExchangeAlgorithm::DHE_RSA, CipherAlgorithm::AES_128_GCM, Crypto::Hash::SHA256, 8, true)         \
    C(true, CipherSuite::TLS_DHE_RSA_WITH_AES_256_GCM_SHA384, KeyExchangeAlgorithm::DHE_RSA, CipherAlgorithm::AES_256_GCM, Crypto::Hash::SHA384, 8, true)         \
    C(true, CipherSuite::TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, KeyExchangeAlgorithm::ECDHE_ECDSA, CipherAlgorithm::AES_128_CBC, Crypto::Hash::SHA1, 16, false)    \
    C(true, CipherSuite::TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA, KeyExchangeAlgorithm::ECDHE_RSA, CipherAlgorithm::AES_128_CBC, Crypto::Hash::SHA1, 16, false)        \
    C(true, CipherSuite::TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA, KeyExchangeAlgorithm::ECDHE_ECDSA, CipherAlgorithm::AES_256_CBC, Crypto::Hash::SHA1, 16, false)    \
    C(true, CipherSuite::TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA, KeyExchangeAlgorithm::ECDHE_RSA, CipherAlgorithm::AES_256_CBC, Crypto::Hash::SHA1, 16, false)        \
    C(true, CipherSuite::TLS_RSA_WITH_AES_128_GCM_SHA256, KeyExchangeAlgorithm::RSA, CipherAlgorithm::AES_128_GCM, Crypto::Hash::SHA256, 8, true)                 \
    C(true, CipherSuite::TLS_RSA_WITH_AES_256_GCM_SHA384, KeyExchangeAlgorithm::RSA, CipherAlgorithm::AES_256_GCM, Crypto::Hash::SHA384, 8, true)                 \
    C(true, CipherSuite::TLS_RSA_WITH_AES_128_CBC_SHA256, KeyExchangeAlgorithm::RSA, CipherAlgorithm::AES_128_CBC, Crypto::Hash::SHA256, 16, false)               \
    C(true, CipherSuite::TLS_RSA_WITH_AES_256_CBC_SHA256, KeyExchangeAlgorithm::RSA, CipherAlgorithm::AES_256_CBC, Crypto::Hash::SHA256, 16, false)               \
    C(true, CipherSuite::TLS_RSA_WITH_AES_128_CBC_SHA, KeyExchangeAlgorithm::RSA, CipherAlgorithm::AES_128_CBC, Crypto::Hash::SHA1, 16, false)                    \
    C(true, CipherSuite::TLS_RSA_WITH_AES_256_CBC_SHA, KeyExchangeAlgorithm::RSA, CipherAlgorithm::AES_256_CBC, Crypto::Hash::SHA1, 16, false)

constexpr KeyExchangeAlgorithm get_key_exchange_algorithm(CipherSuite suite)
{
    switch (suite) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return key_exchange;
        ENUMERATE_CIPHERS(C)
#undef C
    default:
        return KeyExchangeAlgorithm::Invalid;
    }
}

constexpr CipherAlgorithm get_cipher_algorithm(CipherSuite suite)
{
    switch (suite) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return cipher;
        ENUMERATE_CIPHERS(C)
#undef C
    default:
        return CipherAlgorithm::Invalid;
    }
}

ErrorOr<SupportedGroup> oid_to_curve(Vector<int> curve);

struct Options {
    static Vector<CipherSuite> default_usable_cipher_suites()
    {
        Vector<CipherSuite> cipher_suites;
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    if constexpr (is_supported)                                              \
        cipher_suites.empend(suite);
        ENUMERATE_CIPHERS(C)
#undef C
        return cipher_suites;
    }
    Vector<CipherSuite> usable_cipher_suites = default_usable_cipher_suites();

#define OPTION_WITH_DEFAULTS(typ, name, ...) \
    static typ default_##name()              \
    {                                        \
        return typ { __VA_ARGS__ };          \
    }                                        \
    typ name = default_##name();             \
    Options& set_##name(typ new_value)&      \
    {                                        \
        name = move(new_value);              \
        return *this;                        \
    }                                        \
    Options&& set_##name(typ new_value)&&    \
    {                                        \
        name = move(new_value);              \
        return move(*this);                  \
    }

    OPTION_WITH_DEFAULTS(ProtocolVersion, version, ProtocolVersion::VERSION_1_2)
    OPTION_WITH_DEFAULTS(Vector<SignatureAndHashAlgorithm>, supported_signature_algorithms,
        { HashAlgorithm::SHA512, SignatureAlgorithm::RSA },
        { HashAlgorithm::SHA384, SignatureAlgorithm::RSA },
        { HashAlgorithm::SHA256, SignatureAlgorithm::RSA },
        { HashAlgorithm::SHA1, SignatureAlgorithm::RSA },
        { HashAlgorithm::SHA256, SignatureAlgorithm::ECDSA },
        { HashAlgorithm::SHA384, SignatureAlgorithm::ECDSA },
        { HashAlgorithm::INTRINSIC, SignatureAlgorithm::ED25519 });
    OPTION_WITH_DEFAULTS(Vector<SupportedGroup>, elliptic_curves,
        SupportedGroup::X25519,
        SupportedGroup::SECP256R1,
        SupportedGroup::SECP384R1,
        SupportedGroup::X448)
    OPTION_WITH_DEFAULTS(Vector<ECPointFormat>, supported_ec_point_formats, ECPointFormat::UNCOMPRESSED)

    OPTION_WITH_DEFAULTS(bool, use_sni, true)
    OPTION_WITH_DEFAULTS(bool, use_compression, false)
    OPTION_WITH_DEFAULTS(bool, validate_certificates, true)
    OPTION_WITH_DEFAULTS(bool, allow_self_signed_certificates, false)
    OPTION_WITH_DEFAULTS(Optional<Vector<Certificate>>, root_certificates, )
    OPTION_WITH_DEFAULTS(Function<void(AlertDescription)>, alert_handler, [](auto) { })
    OPTION_WITH_DEFAULTS(Function<void()>, finish_callback, [] { })
    OPTION_WITH_DEFAULTS(Function<Vector<Certificate>()>, certificate_provider, [] { return Vector<Certificate> {}; })
    OPTION_WITH_DEFAULTS(bool, enable_extended_master_secret, true)

#undef OPTION_WITH_DEFAULTS
};

class SegmentedBuffer {
public:
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] bool is_empty() const { return m_size == 0; }
    void transfer(Bytes dest, size_t size)
    {
        VERIFY(size <= dest.size());
        size_t transferred = 0;
        while (transferred < size) {
            auto& buffer = m_buffers.head();
            size_t to_transfer = min(buffer.size() - m_offset_into_current_buffer, size - transferred);
            memcpy(dest.offset(transferred), buffer.data() + m_offset_into_current_buffer, to_transfer);
            transferred += to_transfer;
            m_offset_into_current_buffer += to_transfer;
            if (m_offset_into_current_buffer >= buffer.size()) {
                m_buffers.dequeue();
                m_offset_into_current_buffer = 0;
            }
            m_size -= to_transfer;
        }
    }

    AK::ErrorOr<void> try_append(ReadonlyBytes data)
    {
        if (Checked<size_t>::addition_would_overflow(m_size, data.size()))
            return AK::Error::from_errno(EOVERFLOW);

        m_size += data.size();
        m_buffers.enqueue(TRY(ByteBuffer::copy(data)));
        return {};
    }

private:
    size_t m_size { 0 };
    Queue<ByteBuffer> m_buffers;
    size_t m_offset_into_current_buffer { 0 };
};

struct Context {
    bool verify_chain(StringView host) const;
    bool verify_certificate_pair(Certificate const& subject, Certificate const& issuer) const;

    Options options;

    u8 remote_random[32];
    u8 local_random[32];
    u8 session_id[32];
    u8 session_id_size { 0 };
    CipherSuite cipher;
    bool is_server { false };
    Vector<Certificate> certificates;
    Certificate private_key;
    Vector<Certificate> client_certificates;
    ByteBuffer master_key;
    ByteBuffer premaster_key;
    u8 cipher_spec_set { 0 };
    struct {
        int created { 0 };
        u8 remote_mac[32];
        u8 local_mac[32];
        u8 local_iv[16];
        u8 remote_iv[16];
        u8 local_aead_iv[4];
        u8 remote_aead_iv[4];
    } crypto;

    Crypto::Hash::Manager handshake_hash;

    ByteBuffer message_buffer;
    u64 remote_sequence_number { 0 };
    u64 local_sequence_number { 0 };

    ConnectionStatus connection_status { ConnectionStatus::Disconnected };
    bool should_expect_successful_read { false };
    u8 critical_error { 0 };
    Error error_code { Error::NoError };

    ByteBuffer tls_buffer;

    SegmentedBuffer application_buffer;

    bool is_child { false };

    struct {
        // Server Name Indicator
        ByteString SNI; // I hate your existence
        bool extended_master_secret { false };
    } extensions;

    u8 request_client_certificate { 0 };

    ByteBuffer cached_handshake;

    ClientVerificationStaus client_verified { Verified };

    bool connection_finished { false };
    bool close_notify { false };
    bool has_invoked_finish_or_error_callback { false };

    // message flags
    u8 handshake_messages[11] { 0 };
    ByteBuffer user_data;
    HashMap<ByteString, Certificate> root_certificates;

    Vector<ByteString> alpn;
    StringView negotiated_alpn;

    size_t send_retries { 0 };

    time_t handshake_initiation_timestamp { 0 };

    struct {
        ByteBuffer p;
        ByteBuffer g;
        ByteBuffer Ys;
    } server_diffie_hellman_params;

    OwnPtr<Crypto::Curves::EllipticCurve> server_key_exchange_curve;
};

class TLSv12 final : public Core::Socket {
private:
    Core::Socket& underlying_stream()
    {
        return *m_stream.visit([&](auto& stream) -> Core::Socket* { return stream; });
    }
    Core::Socket const& underlying_stream() const
    {
        return *m_stream.visit([&](auto& stream) -> Core::Socket const* { return stream; });
    }

public:
    /// Reads into a buffer, with the maximum size being the size of the buffer.
    /// The amount of bytes read can be smaller than the size of the buffer.
    /// Returns either the bytes that were read, or an errno in the case of
    /// failure.
    virtual ErrorOr<Bytes> read_some(Bytes) override;

    /// Tries to write the entire contents of the buffer. It is possible for
    /// less than the full buffer to be written. Returns either the amount of
    /// bytes written into the stream, or an errno in the case of failure.
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;

    virtual bool is_eof() const override { return m_context.application_buffer.is_empty() && (m_context.connection_finished || underlying_stream().is_eof()); }

    virtual bool is_open() const override { return is_established(); }
    virtual void close() override;

    virtual ErrorOr<size_t> pending_bytes() const override { return m_context.application_buffer.size(); }
    virtual ErrorOr<bool> can_read_without_blocking(int = 0) const override { return !m_context.application_buffer.is_empty(); }
    virtual ErrorOr<void> set_blocking(bool block) override
    {
        VERIFY(!block);
        return {};
    }
    virtual ErrorOr<void> set_close_on_exec(bool enabled) override { return underlying_stream().set_close_on_exec(enabled); }

    virtual void set_notifications_enabled(bool enabled) override { underlying_stream().set_notifications_enabled(enabled); }

    static ErrorOr<NonnullOwnPtr<TLSv12>> connect(Core::SocketAddress, ByteString const& host, Options = {});
    static ErrorOr<NonnullOwnPtr<TLSv12>> connect(ByteString const& host, u16 port, Options = {});
    static ErrorOr<NonnullOwnPtr<TLSv12>> connect(ByteString const& host, Core::Socket& underlying_stream, Options = {});

    using StreamVariantType = Variant<OwnPtr<Core::Socket>, Core::Socket*>;
    explicit TLSv12(StreamVariantType, Options);

    bool is_established() const { return m_context.connection_status == ConnectionStatus::Established; }

    void set_sni(StringView sni)
    {
        if (m_context.is_server || m_context.critical_error || m_context.connection_status != ConnectionStatus::Disconnected) {
            dbgln("invalid state for set_sni");
            return;
        }
        m_context.extensions.SNI = sni;
    }

    void set_root_certificates(Vector<Certificate>);

    static Vector<Certificate> parse_pem_certificate(ReadonlyBytes certificate_pem_buffer, ReadonlyBytes key_pem_buffer);

    StringView alpn() const { return m_context.negotiated_alpn; }

    bool supports_cipher(CipherSuite suite) const
    {
        switch (suite) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return is_supported;
            ENUMERATE_CIPHERS(C)
#undef C
        default:
            return false;
        }
    }

    bool supports_version(ProtocolVersion v) const
    {
        return v == ProtocolVersion::VERSION_1_2;
    }

    void alert(AlertLevel, AlertDescription);

    Function<void(AlertDescription)> on_tls_error;
    Function<void()> on_tls_finished;
    Function<void(TLSv12&)> on_tls_certificate_request;
    Function<void()> on_connected;

private:
    void setup_connection();

    void consume(ReadonlyBytes record);

    ByteBuffer hmac_message(ReadonlyBytes buf, Optional<ReadonlyBytes> const buf2, size_t mac_length, bool local = false);
    void ensure_hmac(size_t digest_size, bool local);

    void update_packet(ByteBuffer& packet);
    void update_hash(ReadonlyBytes in, size_t header_size);

    void write_packet(ByteBuffer& packet, bool immediately = false);

    ByteBuffer build_client_key_exchange();
    ByteBuffer build_server_key_exchange();

    ByteBuffer build_hello();
    ByteBuffer build_handshake_finished();
    ByteBuffer build_certificate();
    ByteBuffer build_alert(bool critical, u8 code);
    ByteBuffer build_change_cipher_spec();
    void build_rsa_pre_master_secret(PacketBuilder&);
    void build_dhe_rsa_pre_master_secret(PacketBuilder&);
    void build_ecdhe_rsa_pre_master_secret(PacketBuilder&);

    ErrorOr<bool> flush();
    void write_into_socket();
    ErrorOr<void> read_from_socket();

    bool check_connection_state(bool read);
    void notify_client_for_app_data();

    ssize_t handle_server_hello(ReadonlyBytes, WritePacketStage&);
    ssize_t handle_handshake_finished(ReadonlyBytes, WritePacketStage&);
    ssize_t handle_certificate(ReadonlyBytes);
    ssize_t handle_server_key_exchange(ReadonlyBytes);
    ssize_t handle_dhe_rsa_server_key_exchange(ReadonlyBytes);
    ssize_t handle_ecdhe_server_key_exchange(ReadonlyBytes, u8& server_public_key_length);
    ssize_t handle_ecdhe_rsa_server_key_exchange(ReadonlyBytes);
    ssize_t handle_ecdhe_ecdsa_server_key_exchange(ReadonlyBytes);
    ssize_t handle_server_hello_done(ReadonlyBytes);
    ssize_t handle_certificate_verify(ReadonlyBytes);
    ssize_t handle_handshake_payload(ReadonlyBytes);
    ssize_t handle_message(ReadonlyBytes);

    void pseudorandom_function(Bytes output, ReadonlyBytes secret, u8 const* label, size_t label_length, ReadonlyBytes seed, ReadonlyBytes seed_b);

    ssize_t verify_rsa_server_key_exchange(ReadonlyBytes server_key_info_buffer, ReadonlyBytes signature_buffer);
    ssize_t verify_ecdsa_server_key_exchange(ReadonlyBytes server_key_info_buffer, ReadonlyBytes signature_buffer);

    size_t key_length() const
    {
        switch (m_context.cipher) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return cipher_key_size(cipher) / 8;
            ENUMERATE_CIPHERS(C)
#undef C
        default:
            return 128 / 8;
        }
    }

    size_t mac_length() const
    {
        switch (m_context.cipher) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return hash ::digest_size();
            ENUMERATE_CIPHERS(C)
#undef C
        default:
            return Crypto::Hash::SHA256::digest_size();
        }
    }

    Crypto::Hash::HashKind hmac_hash() const
    {
        switch (mac_length()) {
        case Crypto::Hash::SHA512::DigestSize:
            return Crypto::Hash::HashKind::SHA512;
        case Crypto::Hash::SHA384::DigestSize:
            return Crypto::Hash::HashKind::SHA384;
        case Crypto::Hash::SHA256::DigestSize:
        case Crypto::Hash::SHA1::DigestSize:
        default:
            return Crypto::Hash::HashKind::SHA256;
        }
    }

    size_t iv_length() const
    {
        switch (m_context.cipher) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return iv_size;
            ENUMERATE_CIPHERS(C)
#undef C
        default:
            return 16;
        }
    }

    bool is_aead() const
    {
        switch (m_context.cipher) {
#define C(is_supported, suite, key_exchange, cipher, hash, iv_size, is_aead) \
    case suite:                                                              \
        return is_aead;
            ENUMERATE_CIPHERS(C)
#undef C
        default:
            return false;
        }
    }

    bool expand_key();

    bool compute_master_secret_from_pre_master_secret(size_t length);

    void try_disambiguate_error() const;

    bool m_eof { false };
    StreamVariantType m_stream;
    Context m_context;

    OwnPtr<Crypto::Authentication::HMAC<Crypto::Hash::Manager>> m_hmac_local;
    OwnPtr<Crypto::Authentication::HMAC<Crypto::Hash::Manager>> m_hmac_remote;

    using CipherVariant = Variant<
        Empty,
        Crypto::Cipher::AESCipher::CBCMode,
        Crypto::Cipher::AESCipher::GCMMode>;
    CipherVariant m_cipher_local {};
    CipherVariant m_cipher_remote {};

    bool m_has_scheduled_write_flush { false };
    bool m_has_scheduled_app_data_flush { false };
    i32 m_max_wait_time_for_handshake_in_seconds { 10 };

    RefPtr<Core::Timer> m_handshake_timeout_timer;
};

}
