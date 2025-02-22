/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Socket.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibTLS/OpenSSLForward.h>

namespace TLS {

struct Options {

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

    OPTION_WITH_DEFAULTS(Optional<ByteString>, root_certificates_path, )
    OPTION_WITH_DEFAULTS(bool, blocking, true)
};

class TLSv12 final : public Core::Socket {
public:
    /// Reads into a buffer, with the maximum size being the size of the buffer.
    /// The amount of bytes read can be smaller than the size of the buffer.
    /// Returns either the bytes that were read, or an error in the case of
    /// failure.
    virtual ErrorOr<Bytes> read_some(Bytes) override;

    /// Tries to write the entire contents of the buffer. It is possible for
    /// less than the full buffer to be written. Returns either the amount of
    /// bytes written into the stream, or an error in the case of failure.
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;

    virtual bool is_eof() const override;
    virtual bool is_open() const override;

    virtual void close() override;

    virtual ErrorOr<size_t> pending_bytes() const override;
    virtual ErrorOr<bool> can_read_without_blocking(int = 0) const override;
    virtual ErrorOr<void> set_blocking(bool block) override;
    virtual ErrorOr<void> set_close_on_exec(bool enabled) override;

    static ErrorOr<NonnullOwnPtr<TLSv12>> connect(Core::SocketAddress const&, ByteString const& host, Options = {});
    static ErrorOr<NonnullOwnPtr<TLSv12>> connect(ByteString const& host, u16 port, Options = {});

    ~TLSv12() override;

private:
    explicit TLSv12(NonnullOwnPtr<Core::TCPSocket>, SSL_CTX*, SSL*, BIO*);

    static ErrorOr<NonnullOwnPtr<TLSv12>> connect_internal(NonnullOwnPtr<Core::TCPSocket>, ByteString const&, Options);

    void handle_fatal_error();

    SSL_CTX* m_ssl_ctx { nullptr };
    SSL* m_ssl { nullptr };
    BIO* m_bio { nullptr };

    // Keep this around or the socket will be closed
    NonnullOwnPtr<Core::TCPSocket> m_socket;
};

}
