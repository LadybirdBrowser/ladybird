/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BufferedStream.h>
#include <AK/Function.h>
#include <AK/Stream.h>
#include <AK/Time.h>
#include <LibCore/Notifier.h>
#include <LibCore/SocketAddress.h>

namespace Core {

/// The Socket class is the base class for all concrete BSD-style socket
/// classes. Sockets are non-seekable streams which can be read byte-wise.
class Socket : public Stream {
public:
    enum class PreventSIGPIPE {
        No,
        Yes,
    };

    enum class SocketType {
        Stream,
        Datagram,
    };

    Socket(Socket&&) = default;
    Socket& operator=(Socket&&) = default;

    /// Checks how many bytes of data are currently available to read on the
    /// socket. For datagram-based socket, this is the size of the first
    /// datagram that can be read. Returns either the amount of bytes, or an
    /// errno in the case of failure.
    virtual ErrorOr<size_t> pending_bytes() const = 0;
    /// Returns whether there's any data that can be immediately read, or an
    /// errno on failure.
    virtual ErrorOr<bool> can_read_without_blocking(int timeout = 0) const = 0;
    // Sets the blocking mode of the socket. If blocking mode is disabled, reads
    // will fail with EAGAIN when there's no data available to read, and writes
    // will fail with EAGAIN when the data cannot be written without blocking
    // (due to the send buffer being full, for example).
    virtual ErrorOr<void> set_blocking(bool enabled) = 0;
    // Sets the close-on-exec mode of the socket. If close-on-exec mode is
    // enabled, then the socket will be automatically closed by the kernel when
    // an exec call happens.
    virtual ErrorOr<void> set_close_on_exec(bool enabled) = 0;

    /// Disables any listening mechanisms that this socket uses.
    /// Can be called with 'false' when `on_ready_to_read` notifications are no longer needed.
    /// Conversely, set_notifications_enabled(true) will re-enable notifications.
    virtual void set_notifications_enabled(bool) { }

    // FIXME: This will need to be updated when IPv6 socket arrives. Perhaps a
    //        base class for all address types is appropriate.
    static ErrorOr<Vector<Variant<IPv4Address, IPv6Address>>> resolve_host(ByteString const&, SocketType);

    Function<void()> on_ready_to_read;

protected:
    enum class SocketDomain {
        Local,
        Inet,
        Inet6,
    };

    explicit Socket(PreventSIGPIPE prevent_sigpipe = PreventSIGPIPE::Yes)
        : m_prevent_sigpipe(prevent_sigpipe == PreventSIGPIPE::Yes)
    {
    }

    static ErrorOr<int> create_fd(SocketDomain, SocketType);

    static ErrorOr<void> connect_local(int fd, ByteString const& path);
    static ErrorOr<void> connect_inet(int fd, SocketAddress const&);

    int default_flags() const
    {
        int flags = 0;
        if (m_prevent_sigpipe)
            flags |= MSG_NOSIGNAL;
        return flags;
    }

private:
    bool m_prevent_sigpipe { true };
};

/// A reusable socket maintains state about being connected in addition to
/// normal Socket capabilities, and can be reconnected once disconnected.
class ReusableSocket : public Socket {
public:
    /// Returns whether the socket is currently connected.
    virtual bool is_connected() = 0;
    /// Reconnects the socket to the given host and port. Returns EALREADY if
    /// is_connected() is true.
    virtual ErrorOr<void> reconnect(ByteString const& host, u16 port) = 0;
    /// Connects the socket to the given socket address (IP address + port).
    /// Returns EALREADY is_connected() is true.
    virtual ErrorOr<void> reconnect(SocketAddress const&) = 0;
};

class PosixSocketHelper {
    AK_MAKE_NONCOPYABLE(PosixSocketHelper);

public:
    template<typename T>
    PosixSocketHelper(Badge<T>)
    requires(IsBaseOf<Socket, T>)
    {
    }

    PosixSocketHelper(PosixSocketHelper&& other)
    {
        operator=(move(other));
    }

    PosixSocketHelper& operator=(PosixSocketHelper&& other)
    {
        m_fd = exchange(other.m_fd, -1);
        m_last_read_was_eof = exchange(other.m_last_read_was_eof, false);
        m_notifier = move(other.m_notifier);
        return *this;
    }

    int fd() const { return m_fd; }
    void set_fd(int fd) { m_fd = fd; }

    ErrorOr<Bytes> read(Bytes, int flags);
    ErrorOr<size_t> write(ReadonlyBytes, int flags);

    bool is_eof() const { return !is_open() || m_last_read_was_eof; }
    void did_reach_eof_on_read();
    bool is_open() const { return m_fd != -1; }
    void close();

    ErrorOr<size_t> pending_bytes() const;
    ErrorOr<bool> can_read_without_blocking(int timeout) const;

    ErrorOr<void> set_blocking(bool enabled);
    ErrorOr<void> set_close_on_exec(bool enabled);
    ErrorOr<void> set_receive_timeout(AK::Duration timeout);

    void setup_notifier();
    RefPtr<Core::Notifier> notifier() { return m_notifier; }

private:
    int m_fd { -1 };
    bool m_last_read_was_eof { false };
    RefPtr<Core::Notifier> m_notifier;
};

class TCPSocket final : public Socket {
public:
    static ErrorOr<NonnullOwnPtr<TCPSocket>> connect(ByteString const& host, u16 port);
    static ErrorOr<NonnullOwnPtr<TCPSocket>> connect(SocketAddress const& address);
    static ErrorOr<NonnullOwnPtr<TCPSocket>> connect(SocketAddress const& address, ByteString const&) { return connect(address); }
    static ErrorOr<NonnullOwnPtr<TCPSocket>> adopt_fd(int fd);

    TCPSocket(TCPSocket&& other)
        : Socket(static_cast<Socket&&>(other))
        , m_helper(move(other.m_helper))
    {
        if (is_open())
            setup_notifier();
    }

    TCPSocket& operator=(TCPSocket&& other)
    {
        Socket::operator=(static_cast<Socket&&>(other));
        m_helper = move(other.m_helper);
        if (is_open())
            setup_notifier();

        return *this;
    }

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override { return m_helper.read(buffer, default_flags()); }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override { return m_helper.write(buffer, default_flags()); }
    virtual bool is_eof() const override { return m_helper.is_eof(); }
    virtual bool is_open() const override { return m_helper.is_open(); }
    virtual void close() override { m_helper.close(); }
    virtual ErrorOr<size_t> pending_bytes() const override { return m_helper.pending_bytes(); }
    virtual ErrorOr<bool> can_read_without_blocking(int timeout = 0) const override { return m_helper.can_read_without_blocking(timeout); }
    virtual void set_notifications_enabled(bool enabled) override
    {
        if (auto notifier = m_helper.notifier())
            notifier->set_enabled(enabled);
    }
    ErrorOr<void> set_blocking(bool enabled) override { return m_helper.set_blocking(enabled); }
    ErrorOr<void> set_close_on_exec(bool enabled) override { return m_helper.set_close_on_exec(enabled); }

    int fd() const { return m_helper.fd(); }

    virtual ~TCPSocket() override { close(); }

private:
    explicit TCPSocket(PreventSIGPIPE prevent_sigpipe = PreventSIGPIPE::Yes)
        : Socket(prevent_sigpipe)
    {
    }

    void setup_notifier()
    {
        VERIFY(is_open());

        m_helper.setup_notifier();
        m_helper.notifier()->on_activation = [this] {
            if (on_ready_to_read)
                on_ready_to_read();
        };
    }

    PosixSocketHelper m_helper { Badge<TCPSocket> {} };
};

class UDPSocket final : public Socket {
public:
    static ErrorOr<NonnullOwnPtr<UDPSocket>> connect(ByteString const& host, u16 port, Optional<AK::Duration> timeout = {});
    static ErrorOr<NonnullOwnPtr<UDPSocket>> connect(SocketAddress const& address, Optional<AK::Duration> timeout = {});
    static ErrorOr<NonnullOwnPtr<UDPSocket>> connect(SocketAddress const& address, ByteString const&, Optional<AK::Duration> timeout = {}) { return connect(address, timeout); }

    UDPSocket(UDPSocket&& other)
        : Socket(static_cast<Socket&&>(other))
        , m_helper(move(other.m_helper))
    {
        if (is_open())
            setup_notifier();
    }

    UDPSocket& operator=(UDPSocket&& other)
    {
        Socket::operator=(static_cast<Socket&&>(other));
        m_helper = move(other.m_helper);
        if (is_open())
            setup_notifier();

        return *this;
    }

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override
    {
        auto pending_bytes = TRY(this->pending_bytes());
        if (pending_bytes > buffer.size()) {
            // With UDP datagrams, reading a datagram into a buffer that's
            // smaller than the datagram's size will cause the rest of the
            // datagram to be discarded. That's not very nice, so let's bail
            // early, telling the caller that he should allocate a bigger
            // buffer.
            return Error::from_errno(EMSGSIZE);
        }

        return m_helper.read(buffer, default_flags());
    }

    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override { return m_helper.write(buffer, default_flags()); }
    virtual bool is_eof() const override { return m_helper.is_eof(); }
    virtual bool is_open() const override { return m_helper.is_open(); }
    virtual void close() override { m_helper.close(); }
    virtual ErrorOr<size_t> pending_bytes() const override { return m_helper.pending_bytes(); }
    virtual ErrorOr<bool> can_read_without_blocking(int timeout = 0) const override { return m_helper.can_read_without_blocking(timeout); }
    virtual void set_notifications_enabled(bool enabled) override
    {
        if (auto notifier = m_helper.notifier())
            notifier->set_enabled(enabled);
    }
    ErrorOr<void> set_blocking(bool enabled) override { return m_helper.set_blocking(enabled); }
    ErrorOr<void> set_close_on_exec(bool enabled) override { return m_helper.set_close_on_exec(enabled); }

    virtual ~UDPSocket() override { close(); }

private:
    explicit UDPSocket(PreventSIGPIPE prevent_sigpipe = PreventSIGPIPE::Yes)
        : Socket(prevent_sigpipe)
    {
    }

    void setup_notifier()
    {
        VERIFY(is_open());

        m_helper.setup_notifier();
        m_helper.notifier()->on_activation = [this] {
            if (on_ready_to_read)
                on_ready_to_read();
        };
    }

    PosixSocketHelper m_helper { Badge<UDPSocket> {} };
};

class LocalSocket final : public Socket {
public:
    static ErrorOr<NonnullOwnPtr<LocalSocket>> connect(ByteString const& path, PreventSIGPIPE = PreventSIGPIPE::Yes);
    static ErrorOr<NonnullOwnPtr<LocalSocket>> adopt_fd(int fd, PreventSIGPIPE = PreventSIGPIPE::Yes);

    LocalSocket(LocalSocket&& other)
        : Socket(static_cast<Socket&&>(other))
        , m_helper(move(other.m_helper))
    {
        if (is_open())
            setup_notifier();
    }

    LocalSocket& operator=(LocalSocket&& other)
    {
        Socket::operator=(static_cast<Socket&&>(other));
        m_helper = move(other.m_helper);
        if (is_open())
            setup_notifier();

        return *this;
    }

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override { return m_helper.read(buffer, default_flags()); }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override { return m_helper.write(buffer, default_flags()); }
    virtual bool is_eof() const override { return m_helper.is_eof(); }
    virtual bool is_open() const override { return m_helper.is_open(); }
    virtual void close() override { m_helper.close(); }
    virtual ErrorOr<size_t> pending_bytes() const override { return m_helper.pending_bytes(); }
    virtual ErrorOr<bool> can_read_without_blocking(int timeout = 0) const override { return m_helper.can_read_without_blocking(timeout); }
    virtual ErrorOr<void> set_blocking(bool enabled) override { return m_helper.set_blocking(enabled); }
    virtual ErrorOr<void> set_close_on_exec(bool enabled) override { return m_helper.set_close_on_exec(enabled); }
    virtual void set_notifications_enabled(bool enabled) override
    {
        if (auto notifier = m_helper.notifier())
            notifier->set_enabled(enabled);
    }

    ErrorOr<int> receive_fd(int flags);
    ErrorOr<void> send_fd(int fd);

    ErrorOr<Bytes> receive_message(Bytes buffer, int flags, Vector<int>& fds);
    ErrorOr<ssize_t> send_message(ReadonlyBytes msg, int flags, Vector<int, 1> fds = {});

    ErrorOr<pid_t> peer_pid() const;
    ErrorOr<Bytes> read_without_waiting(Bytes buffer);

    /// Release the fd associated with this LocalSocket. After the fd is
    /// released, the socket will be considered "closed" and all operations done
    /// on it will fail with ENOTCONN. Fails with ENOTCONN if the socket is
    /// already closed.
    ErrorOr<int> release_fd();

    Optional<int> fd() const;
    RefPtr<Core::Notifier> notifier() { return m_helper.notifier(); }

    virtual ~LocalSocket() { close(); }

    static size_t const MAX_TRANSFER_FDS;

private:
    explicit LocalSocket(PreventSIGPIPE prevent_sigpipe = PreventSIGPIPE::Yes)
        : Socket(prevent_sigpipe)
    {
    }

    void setup_notifier()
    {
        VERIFY(is_open());

        m_helper.setup_notifier();
        m_helper.notifier()->on_activation = [this] {
            if (on_ready_to_read)
                on_ready_to_read();
        };
    }

    PosixSocketHelper m_helper { Badge<LocalSocket> {} };
};

template<typename T>
concept SocketLike = IsBaseOf<Socket, T>;

class BufferedSocketBase : public Socket {
public:
    virtual ErrorOr<StringView> read_line(Bytes buffer) = 0;
    virtual ErrorOr<Bytes> read_until(Bytes buffer, StringView candidate) = 0;
    virtual ErrorOr<bool> can_read_line() = 0;
    virtual ErrorOr<bool> can_read_up_to_delimiter(ReadonlyBytes delimiter) = 0;
    virtual size_t buffer_size() const = 0;
};

template<SocketLike T>
class BufferedSocket final : public BufferedSocketBase {
    friend BufferedHelper<T>;

public:
    static ErrorOr<NonnullOwnPtr<BufferedSocket<T>>> create(NonnullOwnPtr<T> stream, size_t buffer_size = 16384)
    {
        return BufferedHelper<T>::template create_buffered<BufferedSocket>(move(stream), buffer_size);
    }

    BufferedSocket(BufferedSocket&& other)
        : BufferedSocketBase(static_cast<BufferedSocketBase&&>(other))
        , m_helper(move(other.m_helper))
    {
        setup_notifier();
    }

    BufferedSocket& operator=(BufferedSocket&& other)
    {
        Socket::operator=(static_cast<Socket&&>(other));
        m_helper = move(other.m_helper);

        setup_notifier();
        return *this;
    }

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override { return m_helper.read(move(buffer)); }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override { return m_helper.stream().write_some(buffer); }
    virtual bool is_eof() const override { return m_helper.is_eof(); }
    virtual bool is_open() const override { return m_helper.stream().is_open(); }
    virtual void close() override { m_helper.stream().close(); }
    virtual ErrorOr<size_t> pending_bytes() const override
    {
        return TRY(m_helper.stream().pending_bytes()) + m_helper.buffered_data_size();
    }
    virtual ErrorOr<bool> can_read_without_blocking(int timeout = 0) const override { return m_helper.buffered_data_size() > 0 || TRY(m_helper.stream().can_read_without_blocking(timeout)); }
    virtual ErrorOr<void> set_blocking(bool enabled) override { return m_helper.stream().set_blocking(enabled); }
    virtual ErrorOr<void> set_close_on_exec(bool enabled) override { return m_helper.stream().set_close_on_exec(enabled); }
    virtual void set_notifications_enabled(bool enabled) override { m_helper.stream().set_notifications_enabled(enabled); }

    virtual ErrorOr<StringView> read_line(Bytes buffer) override { return m_helper.read_line(move(buffer)); }
    virtual ErrorOr<bool> can_read_line() override
    {
        return TRY(m_helper.can_read_up_to_delimiter("\n"sv.bytes())) || m_helper.is_eof_with_data_left_over();
    }
    virtual ErrorOr<Bytes> read_until(Bytes buffer, StringView candidate) override { return m_helper.read_until(move(buffer), move(candidate)); }
    template<size_t N>
    ErrorOr<Bytes> read_until_any_of(Bytes buffer, Array<StringView, N> candidates) { return m_helper.read_until_any_of(move(buffer), move(candidates)); }
    virtual ErrorOr<bool> can_read_up_to_delimiter(ReadonlyBytes delimiter) override { return m_helper.can_read_up_to_delimiter(delimiter); }

    virtual size_t buffer_size() const override { return m_helper.buffer_size(); }

    virtual ~BufferedSocket() override = default;

private:
    BufferedSocket(NonnullOwnPtr<T> stream, CircularBuffer buffer)
        : m_helper(Badge<BufferedSocket<T>> {}, move(stream), move(buffer))
    {
        setup_notifier();
    }

    void setup_notifier()
    {
        m_helper.stream().on_ready_to_read = [this] {
            if (on_ready_to_read)
                on_ready_to_read();
        };
    }

    BufferedHelper<T> m_helper;
};

using BufferedTCPSocket = BufferedSocket<TCPSocket>;
using BufferedUDPSocket = BufferedSocket<UDPSocket>;
using BufferedLocalSocket = BufferedSocket<LocalSocket>;

/// A BasicReusableSocket allows one to use one of the base Core::Stream classes
/// as a ReusableSocket. It does not preserve any connection state or options,
/// and instead just recreates the stream when reconnecting.
template<SocketLike T>
class BasicReusableSocket final : public ReusableSocket {
public:
    static ErrorOr<NonnullOwnPtr<BasicReusableSocket<T>>> connect(ByteString const& host, u16 port)
    {
        return make<BasicReusableSocket<T>>(TRY(T::connect(host, port)));
    }

    static ErrorOr<NonnullOwnPtr<BasicReusableSocket<T>>> connect(SocketAddress const& address)
    {
        return make<BasicReusableSocket<T>>(TRY(T::connect(address)));
    }

    virtual bool is_connected() override
    {
        return m_socket.is_open();
    }

    virtual ErrorOr<void> reconnect(ByteString const& host, u16 port) override
    {
        if (is_connected())
            return Error::from_errno(EALREADY);

        m_socket = TRY(T::connect(host, port));
        return {};
    }

    virtual ErrorOr<void> reconnect(SocketAddress const& address) override
    {
        if (is_connected())
            return Error::from_errno(EALREADY);

        m_socket = TRY(T::connect(address));
        return {};
    }

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override { return m_socket.read(move(buffer)); }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override { return m_socket.write(buffer); }
    virtual bool is_eof() const override { return m_socket.is_eof(); }
    virtual bool is_open() const override { return m_socket.is_open(); }
    virtual void close() override { m_socket.close(); }
    virtual ErrorOr<size_t> pending_bytes() const override { return m_socket.pending_bytes(); }
    virtual ErrorOr<bool> can_read_without_blocking(int timeout = 0) const override { return m_socket.can_read_without_blocking(timeout); }
    virtual ErrorOr<void> set_blocking(bool enabled) override { return m_socket.set_blocking(enabled); }
    virtual ErrorOr<void> set_close_on_exec(bool enabled) override { return m_socket.set_close_on_exec(enabled); }

private:
    BasicReusableSocket(NonnullOwnPtr<T> socket)
        : m_socket(move(socket))
    {
    }

    NonnullOwnPtr<T> m_socket;
};

using ReusableTCPSocket = BasicReusableSocket<TCPSocket>;
using ReusableUDPSocket = BasicReusableSocket<UDPSocket>;

}
