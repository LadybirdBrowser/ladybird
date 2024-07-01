/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTLS/TLSv12.h>
#include <errno.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#ifndef SOCK_NONBLOCK
#    include <sys/ioctl.h>
#endif

static Vector<ByteString> s_certificate_store_paths;

namespace TLS {

static thread_local bool g_initialized_wolfssl = false;

static int errno_to_wolfssl_error(int error)
{
    switch (error) {
    case EAGAIN:
        return WOLFSSL_CBIO_ERR_WANT_READ;
    case ETIMEDOUT:
        return WOLFSSL_CBIO_ERR_TIMEOUT;
    case ECONNRESET:
        return WOLFSSL_CBIO_ERR_CONN_RST;
    case EINTR:
        return WOLFSSL_CBIO_ERR_ISR;
    case ECONNREFUSED:
        return WOLFSSL_CBIO_ERR_WANT_READ;
    case ECONNABORTED:
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    default:
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
}

StringView WolfTLS::error_text(WOLFSSL* ssl, int error_code)
{
    auto error = wolfSSL_get_error(ssl, error_code);
    static char buffer[WOLFSSL_MAX_ERROR_SZ];
    auto* ptr = wolfSSL_ERR_error_string(error, buffer);
    return StringView { ptr, strlen(ptr) };
}

ErrorOr<NonnullOwnPtr<WolfTLS>> WolfTLS::connect(const AK::ByteString& host, u16 port)
{
    if (!g_initialized_wolfssl) {
        wolfSSL_Init();
        g_initialized_wolfssl = true;
    }

    auto* context = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (!context)
        return Error::from_string_literal("Failed to create a new TLS context");

    if (s_certificate_store_paths.is_empty())
        s_certificate_store_paths.append("/etc/ssl/cert.pem"); // We're just guessing this, the embedder should provide this.

    for (auto& path : s_certificate_store_paths) {
        if (wolfSSL_CTX_load_verify_locations(context, path.characters(), nullptr) != SSL_SUCCESS)
            return Error::from_string_literal("Failed to load CA certificates");
    }

    auto ssl = wolfSSL_new(context);
    if (!ssl)
        return Error::from_string_literal("Failed to create a new SSL object");

    wolfSSL_SSLSetIOSend(ssl, [](WOLFSSL*, char* buf, int sz, void* ctx) -> int {
        auto& self = *static_cast<WolfTLS*>(ctx);
        auto result = self.m_underlying->write_some({ buf, static_cast<size_t>(sz) });
        if (result.is_error() && result.error().is_errno())
            return errno_to_wolfssl_error(result.error().code());
        if (result.is_error())
            return -1;
        return static_cast<int>(result.value());
    });
    wolfSSL_SSLSetIORecv(ssl, [](WOLFSSL*, char* buf, int sz, void* ctx) -> int {
        auto& self = *static_cast<WolfTLS*>(ctx);
        auto result = self.m_underlying->read_some({ buf, static_cast<size_t>(sz) });
        if (result.is_error() && result.error().is_errno())
            return errno_to_wolfssl_error(result.error().code());
        if (result.is_error())
            return -1;
        return static_cast<int>(result.value().size());
    });

    auto tcp_socket = TRY(Core::TCPSocket::connect(host, port));

    auto object = make<WolfTLS>(context, ssl, move(tcp_socket));
    wolfSSL_SetIOReadCtx(ssl, static_cast<void*>(object.ptr()));
    wolfSSL_SetIOWriteCtx(ssl, static_cast<void*>(object.ptr()));

    if (wolfSSL_CTX_UseSNI(context, WOLFSSL_SNI_HOST_NAME, host.bytes().data(), host.bytes().size()) != WOLFSSL_SUCCESS)
        return Error::from_string_literal("Failed to set SNI hostname");

    if (auto rc = wolfSSL_connect(ssl); rc != SSL_SUCCESS)
        return Error::from_string_view(error_text(ssl, rc));

    return object;
}

WolfTLS::~WolfTLS()
{
    close();
    wolfSSL_free(m_ssl);
    wolfSSL_CTX_free(m_context);
}

ErrorOr<Bytes> WolfTLS::read_some(Bytes bytes)
{
    auto result = wolfSSL_read(m_ssl, bytes.data(), bytes.size());
    if (result < 0)
        return Error::from_string_view(error_text(m_ssl, result));
    return Bytes { bytes.data(), static_cast<size_t>(result) };
}

ErrorOr<size_t> WolfTLS::write_some(ReadonlyBytes bytes)
{
    auto result = wolfSSL_write(m_ssl, bytes.data(), bytes.size());
    if (result < 0)
        return Error::from_string_view(error_text(m_ssl, result));
    return static_cast<size_t>(result);
}

bool WolfTLS::is_eof() const
{
    return m_underlying->is_eof() && wolfSSL_pending(m_ssl) == 0;
}

bool WolfTLS::is_open() const
{
    return m_underlying->is_open();
}

void WolfTLS::close()
{
    wolfSSL_shutdown(m_ssl);
}

ErrorOr<size_t> WolfTLS::pending_bytes() const
{
    return wolfSSL_pending(m_ssl);
}

ErrorOr<bool> WolfTLS::can_read_without_blocking(int) const
{
    return true;
}

ErrorOr<void> WolfTLS::set_blocking(bool)
{
    return {};
}

void WolfTLS::install_certificate_store_paths(Vector<AK::ByteString> paths)
{
    s_certificate_store_paths = move(paths);
}

}
