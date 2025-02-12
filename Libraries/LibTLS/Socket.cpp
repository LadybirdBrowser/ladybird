/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/DateTime.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibCore/Timer.h>
#include <LibTLS/TLSv12.h>

// Each record can hold at most 18432 bytes, leaving some headroom and rounding down to
// a nice number gives us a maximum of 16 KiB for user-supplied application data,
// which will be sent as a single record containing a single ApplicationData message.
constexpr static size_t MaximumApplicationDataChunkSize = 16 * KiB;

namespace TLS {

ErrorOr<Bytes> TLSv12::read_some(Bytes bytes)
{
    m_eof = false;
    auto size_to_read = min(bytes.size(), m_context.application_buffer.size());
    if (size_to_read == 0) {
        m_eof = true;
        return Bytes {};
    }

    m_context.application_buffer.transfer(bytes, size_to_read);
    return Bytes { bytes.data(), size_to_read };
}

ErrorOr<size_t> TLSv12::write_some(ReadonlyBytes bytes)
{
    if (m_context.connection_status != ConnectionStatus::Established) {
        dbgln_if(TLS_DEBUG, "write request while not connected");
        return AK::Error::from_string_literal("TLS write request while not connected");
    }

    for (size_t offset = 0; offset < bytes.size(); offset += MaximumApplicationDataChunkSize) {
        PacketBuilder builder { ContentType::APPLICATION_DATA, m_context.options.version, bytes.size() - offset };
        builder.append(bytes.slice(offset, min(bytes.size() - offset, MaximumApplicationDataChunkSize)));
        auto packet = builder.build();

        update_packet(packet);
        write_packet(packet);
    }

    return bytes.size();
}

ErrorOr<NonnullOwnPtr<TLSv12>> TLSv12::connect(ByteString const& host, u16 port, Options options)
{
#ifdef AK_OS_WINDOWS
    (void)host;
    (void)port;
    (void)options;
    return AK::Error::from_string_literal("TODO: Unable to connect via hostname on Windows");
#else
    auto promise = Core::Promise<Empty>::construct();
    OwnPtr<Core::Socket> tcp_socket = TRY(Core::TCPSocket::connect(host, port));
    TRY(tcp_socket->set_blocking(false));
    auto tls_socket = make<TLSv12>(move(tcp_socket), move(options));
    tls_socket->set_sni(host);
    tls_socket->on_connected = [&] {
        promise->resolve({});
    };
    tls_socket->on_tls_error = [&](auto alert) {
        tls_socket->try_disambiguate_error();
        promise->reject(AK::Error::from_string_view(enum_to_string(alert)));
    };

    TRY(promise->await());

    tls_socket->on_tls_error = nullptr;
    tls_socket->on_connected = nullptr;
    tls_socket->m_context.should_expect_successful_read = true;
    return tls_socket;
#endif
}

ErrorOr<NonnullOwnPtr<TLSv12>> TLSv12::connect(Core::SocketAddress address, ByteString const& host, Options options)
{
#ifdef AK_OS_WINDOWS
    (void)address;
    (void)host;
    (void)options;
    return AK::Error::from_string_literal("TODO: Unable to connect via address on Windows");
#else
    auto promise = Core::Promise<Empty>::construct();
    OwnPtr<Core::Socket> tcp_socket = TRY(Core::TCPSocket::connect(address));
    TRY(tcp_socket->set_blocking(false));
    auto tls_socket = make<TLSv12>(move(tcp_socket), move(options));
    tls_socket->set_sni(host);
    tls_socket->on_connected = [&] {
        promise->resolve({});
    };
    tls_socket->on_tls_error = [&](auto alert) {
        tls_socket->try_disambiguate_error();
        promise->reject(AK::Error::from_string_view(enum_to_string(alert)));
    };

    TRY(promise->await());

    tls_socket->on_tls_error = nullptr;
    tls_socket->on_connected = nullptr;
    tls_socket->m_context.should_expect_successful_read = true;
    return tls_socket;
#endif
}

ErrorOr<NonnullOwnPtr<TLSv12>> TLSv12::connect(ByteString const& host, Core::Socket& underlying_stream, Options options)
{
    auto promise = Core::Promise<Empty>::construct();
    TRY(underlying_stream.set_blocking(false));
    auto tls_socket = make<TLSv12>(&underlying_stream, move(options));
    tls_socket->set_sni(host);
    tls_socket->on_connected = [&] {
        promise->resolve({});
    };
    tls_socket->on_tls_error = [&](auto alert) {
        tls_socket->try_disambiguate_error();
        promise->reject(AK::Error::from_string_view(enum_to_string(alert)));
    };
    TRY(promise->await());

    tls_socket->on_tls_error = nullptr;
    tls_socket->on_connected = nullptr;
    tls_socket->m_context.should_expect_successful_read = true;
    return tls_socket;
}

void TLSv12::setup_connection()
{
    Core::deferred_invoke([this] {
        auto& stream = underlying_stream();
        stream.on_ready_to_read = [this] {
            auto result = read_from_socket();
            if (result.is_error())
                dbgln("Read error: {}", result.error());
        };

        m_handshake_timeout_timer = Core::Timer::create_single_shot(
            m_max_wait_time_for_handshake_in_seconds * 1000, [&] {
                dbgln("Handshake timeout :(");
                auto timeout_diff = Core::DateTime::now().timestamp() - m_context.handshake_initiation_timestamp;
                // If the timeout duration was actually within the max wait time (with a margin of error),
                // we're not operating slow, so the server timed out.
                // otherwise, it's our fault that the negotiation is taking too long, so extend the timer :P
                if (timeout_diff < m_max_wait_time_for_handshake_in_seconds + 1) {
                    // The server did not respond fast enough,
                    // time the connection out.
                    alert(AlertLevel::FATAL, AlertDescription::USER_CANCELED);
                    m_context.tls_buffer.clear();
                    m_context.error_code = Error::TimedOut;
                    m_context.critical_error = (u8)Error::TimedOut;
                    check_connection_state(false); // Notify the client.
                } else {
                    // Extend the timer, we are too slow.
                    m_handshake_timeout_timer->restart(m_max_wait_time_for_handshake_in_seconds * 1000);
                }
            });
        auto packet = build_hello();
        write_packet(packet);
        write_into_socket();
        m_handshake_timeout_timer->start();
        m_context.handshake_initiation_timestamp = Core::DateTime::now().timestamp();
    });
    m_has_scheduled_write_flush = true;
}

void TLSv12::notify_client_for_app_data()
{
    if (m_context.application_buffer.size() > 0) {
        if (on_ready_to_read)
            on_ready_to_read();
    } else {
        if (m_context.connection_finished && !m_context.has_invoked_finish_or_error_callback) {
            m_context.has_invoked_finish_or_error_callback = true;
            if (on_tls_finished)
                on_tls_finished();
        }
    }
    m_has_scheduled_app_data_flush = false;
}

ErrorOr<void> TLSv12::read_from_socket()
{
    // If there's anything before we consume stuff, let the client know
    // since we won't be consuming things if the connection is terminated.
    notify_client_for_app_data();

    ScopeGuard notify_guard {
        [this] {
            // If anything new shows up, tell the client about the event.
            notify_client_for_app_data();
        }
    };

    if (!check_connection_state(true))
        return {};

    u8 buffer[16 * KiB];
    Bytes bytes { buffer, array_size(buffer) };
    Bytes read_bytes {};
    auto& stream = underlying_stream();
    do {
        auto result = stream.read_some(bytes);
        if (result.is_error()) {
            if (result.error().is_errno() && result.error().code() != EINTR) {
                if (result.error().code() != EAGAIN)
                    dbgln("TLS Socket read failed, error: {}", result.error());
                break;
            }
            continue;
        }
        read_bytes = result.release_value();
        consume(read_bytes);
    } while (!read_bytes.is_empty() && !m_context.critical_error);

    if (m_context.should_expect_successful_read && read_bytes.is_empty()) {
        // read_some() returned an empty span, this is either an EOF (from improper closure)
        // or some sort of weird even that is showing itself as an EOF.
        // To guard against servers closing the connection weirdly or just improperly, make sure
        // to check the connection state here and send the appropriate notifications.
        stream.close();

        check_connection_state(true);
    }

    return {};
}

void TLSv12::write_into_socket()
{
    dbgln_if(TLS_DEBUG, "Flushing cached records: {} established? {}", m_context.tls_buffer.size(), is_established());

    m_has_scheduled_write_flush = false;
    if (!check_connection_state(false))
        return;

    MUST(flush());
}

bool TLSv12::check_connection_state(bool read)
{
    if (m_context.connection_finished)
        return false;

    if (m_context.close_notify)
        m_context.connection_finished = true;

    auto& stream = underlying_stream();

    if (!stream.is_open()) {
        // an abrupt closure (the server is a jerk)
        dbgln_if(TLS_DEBUG, "Socket not open, assuming abrupt closure");
        m_context.connection_finished = true;
        m_context.connection_status = ConnectionStatus::Disconnected;
        close();
        m_context.has_invoked_finish_or_error_callback = true;
        if (on_ready_to_read)
            on_ready_to_read(); // Notify the client about the weird event.
        if (on_tls_finished)
            on_tls_finished();
        return false;
    }

    if (read && stream.is_eof()) {
        if (m_context.application_buffer.size() == 0 && m_context.connection_status != ConnectionStatus::Disconnected) {
            m_context.has_invoked_finish_or_error_callback = true;
            if (on_tls_finished)
                on_tls_finished();
        }
        return false;
    }

    if (m_context.critical_error) {
        dbgln_if(TLS_DEBUG, "CRITICAL ERROR {} :(", m_context.critical_error);

        m_context.has_invoked_finish_or_error_callback = true;
        if (on_tls_error)
            on_tls_error((AlertDescription)m_context.critical_error);
        m_context.connection_finished = true;
        m_context.connection_status = ConnectionStatus::Disconnected;
        close();
        return false;
    }

    if (((read && m_context.application_buffer.size() == 0) || !read) && m_context.connection_finished) {
        if (m_context.application_buffer.size() == 0 && m_context.connection_status != ConnectionStatus::Disconnected) {
            m_context.has_invoked_finish_or_error_callback = true;
            if (on_tls_finished)
                on_tls_finished();
        }
        m_context.connection_status = ConnectionStatus::Disconnected;
        if (m_context.tls_buffer.size()) {
            dbgln_if(TLS_DEBUG, "connection closed without finishing data transfer, {} bytes still in buffer and {} bytes in application buffer",
                m_context.tls_buffer.size(),
                m_context.application_buffer.size());
        }
        if (!m_context.application_buffer.size()) {
            return false;
        }
    }
    return true;
}

ErrorOr<bool> TLSv12::flush()
{
    ByteBuffer out = move(m_context.tls_buffer);
    auto out_bytes = out.bytes();

    if (out_bytes.is_empty())
        return true;

    if constexpr (TLS_DEBUG) {
        dbgln("SENDING...");
        print_buffer(out_bytes);
    }

    auto& stream = underlying_stream();
    Optional<AK::Error> error;
    size_t written;
    do {
        auto result = stream.write_some(out_bytes);
        if (result.is_error()) {
            if (result.error().code() != EINTR && result.error().code() != EAGAIN) {
                error = result.release_error();
                dbgln("TLS Socket write error: {}", *error);
                break;
            }
            continue;
        }
        written = result.value();
        out_bytes = out_bytes.slice(written);
    } while (!out_bytes.is_empty());

    if (out_bytes.is_empty() && !error.has_value())
        return true;

    if (!out_bytes.is_empty())
        dbgln("Dropping {} bytes worth of TLS records on the floor", out_bytes.size());
    return false;
}

void TLSv12::close()
{
    if (underlying_stream().is_open())
        alert(AlertLevel::FATAL, AlertDescription::CLOSE_NOTIFY);
    // bye bye.
    m_context.connection_status = ConnectionStatus::Disconnected;
}

}
