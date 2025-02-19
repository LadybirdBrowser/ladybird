/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Promise.h>
#include <LibCrypto/OpenSSL.h>
#include <LibTLS/TLSv12.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace TLS {

ErrorOr<NonnullOwnPtr<TLSv12>> TLSv12::connect(ByteString const& host, u16 port, Options options)
{
    auto tcp_socket = TRY(Core::TCPSocket::connect(host, port));
    return connect_internal(move(tcp_socket), host, options);
}

ErrorOr<NonnullOwnPtr<TLSv12>> TLSv12::connect(Core::SocketAddress address, ByteString const& host, Options options)
{
    auto tcp_socket = TRY(Core::TCPSocket::connect(address));
    return connect_internal(move(tcp_socket), host, options);
}

static void wait_for_activity(int sock, bool read)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);

    if (read)
        select(sock + 1, &fds, nullptr, nullptr, nullptr);
    else
        select(sock + 1, nullptr, &fds, nullptr, nullptr);
}

ErrorOr<Bytes> TLSv12::read_some(Bytes bytes)
{
    auto ret = SSL_read(m_ssl, bytes.data(), bytes.size());
    if (ret <= 0) {
        auto err = SSL_get_error(m_ssl, ret);
        switch (err) {
        case SSL_ERROR_ZERO_RETURN:
            return Bytes { bytes.data(), 0 };
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return Error::from_errno(EAGAIN);
        default:
            return AK::Error::from_string_literal("Failed reading from SSL connection");
        }
    }

    return Bytes { bytes.data(), static_cast<unsigned long>(ret) };
}

ErrorOr<size_t> TLSv12::write_some(ReadonlyBytes bytes)
{
    auto ret = SSL_write(m_ssl, bytes.data(), bytes.size());
    if (ret <= 0) {
        auto err = SSL_get_error(m_ssl, ret);
        switch (err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return Error::from_errno(EAGAIN);
        default:
            return AK::Error::from_string_literal("Failed writing to SSL connection");
        }
    }

    return ret;
}

bool TLSv12::is_eof() const
{
    return BIO_eof(m_bio) == 1;
}

bool TLSv12::is_open() const
{
    return !BIO_get_close(m_bio);
}

void TLSv12::close()
{
    SSL_shutdown(m_ssl);
}

ErrorOr<size_t> TLSv12::pending_bytes() const
{
    return SSL_pending(m_ssl);
}

ErrorOr<bool> TLSv12::can_read_without_blocking(int timeout) const
{
    return m_socket->can_read_without_blocking(timeout);
}

ErrorOr<void> TLSv12::set_blocking(bool)
{
    return Error::from_string_literal("Blocking mode cannot be changed after the SSL connection is established");
}

ErrorOr<void> TLSv12::set_close_on_exec(bool enabled)
{
    return m_socket->set_close_on_exec(enabled);
}

TLSv12::TLSv12(NonnullOwnPtr<Core::TCPSocket> socket, SSL_CTX* ssl_ctx, SSL* ssl, BIO* bio)
    : m_ssl_ctx(ssl_ctx)
    , m_ssl(ssl)
    , m_bio(bio)
    , m_socket(move(socket))
{
    m_socket->on_ready_to_read = [this] {
        if (on_ready_to_read)
            on_ready_to_read();
    };
}

TLSv12::~TLSv12()
{
    SSL_free(m_ssl);
    SSL_CTX_free(m_ssl_ctx);
}

ErrorOr<NonnullOwnPtr<TLSv12>> TLSv12::connect_internal(NonnullOwnPtr<Core::TCPSocket> socket, ByteString const& host, Options options)
{
    TRY(socket->set_blocking(options.blocking));

    auto* ssl_ctx = OPENSSL_TRY_PTR(SSL_CTX_new(TLS_client_method()));
    ArmedScopeGuard free_ssl_ctx = [&] { SSL_CTX_free(ssl_ctx); };

    // Configure the client to abort the handshake if certificate verification fails.
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);

    if (options.root_certificates_path.has_value()) {
        auto path = options.root_certificates_path.value();
        SSL_CTX_load_verify_file(ssl_ctx, path.characters());
    } else {
        // Use the default trusted certificate store
        OPENSSL_TRY(SSL_CTX_set_default_verify_paths(ssl_ctx));
    }

    // Require a minimum TLS version of TLSv1.2.
    OPENSSL_TRY(SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION));

    auto* ssl = OPENSSL_TRY_PTR(SSL_new(ssl_ctx));
    ArmedScopeGuard free_ssl = [&] { SSL_free(ssl); };

    // Tell the server which hostname we are attempting to connect to in case the server supports multiple hosts.
    OPENSSL_TRY(SSL_set_tlsext_host_name(ssl, host.characters()));

    // Ensure we check that the server has supplied a certificate for the hostname that we were expecting.
    OPENSSL_TRY(SSL_set1_host(ssl, host.characters()));

    auto* bio = OPENSSL_TRY_PTR(BIO_new_socket(socket->fd(), 0));

    // SSL takes ownership of the BIO and will handle freeing it
    SSL_set_bio(ssl, bio, bio);

    while (true) {
        auto ret = SSL_connect(ssl);
        if (ret == 1) {
            // Successfully connected
            break;
        }

        switch (SSL_get_error(ssl, ret)) {
        case SSL_ERROR_WANT_READ:
            wait_for_activity(socket->fd(), true);
            break;
        case SSL_ERROR_WANT_WRITE:
            wait_for_activity(socket->fd(), false);
            break;
        case SSL_ERROR_SSL:
            ERR_print_errors_cb(openssl_print_errors, nullptr);
            return Error::from_string_literal("Fatal SSL error connecting to SSL server");
        case SSL_ERROR_SYSCALL:
            return Error::from_errno(errno);
        default:
            return Error::from_string_literal("Failed connecting to SSL server");
        }
    }

    free_ssl.disarm();
    free_ssl_ctx.disarm();

    return adopt_own(*new TLSv12(move(socket), ssl_ctx, ssl, bio));
}

}
