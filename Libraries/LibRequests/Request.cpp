/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>

namespace Requests {

ErrorOr<NonnullOwnPtr<ReadStream>> ReadStream::create(int reader_fd)
{
#if defined(AK_OS_WINDOWS)
    auto local_socket = TRY(Core::LocalSocket::adopt_fd(reader_fd));
    auto notifier = local_socket->notifier();
    VERIFY(notifier);
    return adopt_own(*new ReadStream(move(local_socket), notifier.release_nonnull()));
#else
    auto file = TRY(Core::File::adopt_fd(reader_fd, Core::File::OpenMode::Read));
    auto notifier = Core::Notifier::construct(reader_fd, Core::Notifier::Type::Read);
    return adopt_own(*new ReadStream(move(file), move(notifier)));
#endif
}

Request::Request(RequestClient& client, i32 request_id)
    : m_client(client)
    , m_request_id(request_id)
{
}

bool Request::stop()
{
    on_headers_received = nullptr;
    on_finish = nullptr;
    on_certificate_requested = nullptr;

    m_internal_buffered_data = nullptr;
    m_internal_stream_data = nullptr;
    m_mode = Mode::Unknown;

    return m_client->stop_request({}, *this);
}

void Request::set_request_fd(Badge<Requests::RequestClient>, int fd)
{
    // If the request was stopped while this IPC was in-flight, just bail.
    if (!m_internal_stream_data)
        return;

    VERIFY(m_fd == -1);
    m_fd = fd;

    auto read_stream = MUST(ReadStream::create(fd));
    auto notifier = read_stream->notifier();
    notifier->on_activation = move(m_internal_stream_data->read_notifier->on_activation);
    m_internal_stream_data->read_notifier = notifier;
    m_internal_stream_data->read_stream = move(read_stream);
}

void Request::set_buffered_request_finished_callback(BufferedRequestFinished on_buffered_request_finished)
{
    VERIFY(m_mode == Mode::Unknown);
    m_mode = Mode::Buffered;

    m_internal_buffered_data = make<InternalBufferedData>();

    on_headers_received = [this](auto& headers, auto response_code, auto const& reason_phrase) {
        m_internal_buffered_data->response_headers = headers;
        m_internal_buffered_data->response_code = move(response_code);
        m_internal_buffered_data->reason_phrase = reason_phrase;
    };

    on_finish = [this, on_buffered_request_finished = move(on_buffered_request_finished)](auto total_size, auto& timing_info, auto network_error) {
        auto output_buffer = ByteBuffer::create_uninitialized(m_internal_buffered_data->payload_stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
        m_internal_buffered_data->payload_stream.read_until_filled(output_buffer).release_value_but_fixme_should_propagate_errors();

        on_buffered_request_finished(
            total_size,
            timing_info,
            network_error,
            m_internal_buffered_data->response_headers,
            m_internal_buffered_data->response_code,
            m_internal_buffered_data->reason_phrase,
            output_buffer);
    };

    set_up_internal_stream_data([this](auto read_bytes) {
        // FIXME: What do we do if this fails?
        m_internal_buffered_data->payload_stream.write_until_depleted(read_bytes).release_value_but_fixme_should_propagate_errors();
    });
}

void Request::set_unbuffered_request_callbacks(HeadersReceived on_headers_received, DataReceived on_data_received, RequestFinished on_finish)
{
    VERIFY(m_mode == Mode::Unknown);
    m_mode = Mode::Unbuffered;

    this->on_headers_received = move(on_headers_received);
    this->on_finish = move(on_finish);

    set_up_internal_stream_data(move(on_data_received));
}

void Request::did_finish(Badge<RequestClient>, u64 total_size, RequestTimingInfo const& timing_info, Optional<NetworkError> const& network_error)
{
    if (on_finish)
        on_finish(total_size, timing_info, network_error);
}

void Request::did_receive_headers(Badge<RequestClient>, HTTP::HeaderMap const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase)
{
    if (on_headers_received)
        on_headers_received(response_headers, response_code, reason_phrase);
}

void Request::did_request_certificates(Badge<RequestClient>)
{
    if (on_certificate_requested) {
        auto result = on_certificate_requested();
        if (!m_client->set_certificate({}, *this, result.certificate, result.key)) {
            dbgln("Request: set_certificate failed");
        }
    }
}

void Request::set_up_internal_stream_data(DataReceived on_data_available)
{
    VERIFY(!m_internal_stream_data);

    m_internal_stream_data = make<InternalStreamData>();
    m_internal_stream_data->read_notifier = Core::Notifier::construct(fd(), Core::Notifier::Type::Read);
    if (fd() != -1)
        m_internal_stream_data->read_stream = MUST(ReadStream::create(fd()));

    auto user_on_finish = move(on_finish);
    on_finish = [this](auto total_size, auto const& timing_info, auto network_error) {
        // If the request was stopped while this IPC was in-flight, just bail.
        if (!m_internal_stream_data)
            return;

        m_internal_stream_data->total_size = total_size;
        m_internal_stream_data->network_error = network_error;
        m_internal_stream_data->timing_info = timing_info;
        m_internal_stream_data->request_done = true;
        m_internal_stream_data->on_finish();
    };

    m_internal_stream_data->on_finish = [this, user_on_finish = move(user_on_finish)]() {
        // If the request was stopped while this IPC was in-flight, just bail.
        if (!m_internal_stream_data)
            return;

        if (!m_internal_stream_data->user_finish_called && (!m_internal_stream_data->read_stream || m_internal_stream_data->read_stream->is_eof())) {
            m_internal_stream_data->user_finish_called = true;
            user_on_finish(m_internal_stream_data->total_size, m_internal_stream_data->timing_info, m_internal_stream_data->network_error);
        }
    };

    m_internal_stream_data->read_notifier->on_activation = [this, on_data_available = move(on_data_available)]() {
        static constexpr size_t buffer_size = 256 * KiB;
        static char buffer[buffer_size];

        // If the request was stopped while this IPC was in-flight, just bail.
        if (!m_internal_stream_data)
            return;

        do {
            auto result = m_internal_stream_data->read_stream->read_some({ buffer, buffer_size });
            if (result.is_error() && (!result.error().is_errno() || (result.error().is_errno() && result.error().code() != EINTR)))
                break;
            if (result.is_error())
                continue;

            auto read_bytes = result.release_value();
            if (read_bytes.is_empty())
                break;

            on_data_available(read_bytes);
        } while (true);

        if (m_internal_stream_data->read_stream->is_eof())
            m_internal_stream_data->read_notifier->close();

        if (m_internal_stream_data->request_done)
            m_internal_stream_data->on_finish();
    };
}

}
