/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IPv4Address.h>
#include <LibCore/Socket.h>
#include <LibTLS/Certificate.h>

struct WOLFSSL_CTX;
struct WOLFSSL;
namespace TLS {

class WolfTLS final : public Core::Socket {
public:
    WolfTLS(WOLFSSL_CTX* context, WOLFSSL* ssl, NonnullOwnPtr<Core::TCPSocket> underlying)
        : m_context(context)
        , m_ssl(ssl)
        , m_underlying(move(underlying))
    {
        m_underlying->on_ready_to_read = [this] {
            if (on_ready_to_read)
                on_ready_to_read();
        };
    }

    ~WolfTLS();

    static void install_certificate_store_paths(Vector<ByteString>);

    static ErrorOr<NonnullOwnPtr<WolfTLS>> connect(ByteString const& host, u16 port);
    ErrorOr<Bytes> read_some(Bytes bytes) override;
    ErrorOr<size_t> write_some(ReadonlyBytes bytes) override;
    bool is_eof() const override;
    bool is_open() const override;
    void close() override;
    ErrorOr<size_t> pending_bytes() const override;
    ErrorOr<bool> can_read_without_blocking(int timeout) const override;
    ErrorOr<void> set_blocking(bool enabled) override;
    ErrorOr<void> set_close_on_exec(bool enabled) override { return m_underlying->set_close_on_exec(enabled); }
    void set_notifications_enabled(bool enabled) override { m_underlying->set_notifications_enabled(enabled); }

private:
    static StringView error_text(WOLFSSL*, int error_code);

    WOLFSSL_CTX* m_context { nullptr };
    WOLFSSL* m_ssl { nullptr };
    NonnullOwnPtr<Core::TCPSocket> m_underlying;
};
}
