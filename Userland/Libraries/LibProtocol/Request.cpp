/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibProtocol/Request.h>
#include <LibProtocol/RequestClient.h>

namespace Protocol {

Request::Request(RequestClient& client, i32 request_id)
    : m_client(client)
    , m_request_id(request_id)
{
}

bool Request::stop()
{
    on_headers_received = nullptr;
    on_finish = nullptr;
    on_progress = nullptr;
    on_certificate_requested = nullptr;

    m_internal_buffered_data = nullptr;
    m_internal_stream_data = nullptr;
    m_mode = Mode::Unknown;

    return m_client->stop_request({}, *this);
}

void Request::set_request_fd(Badge<Protocol::RequestClient>, int fd)
{
    VERIFY(m_fd == -1);
    m_fd = fd;

    auto notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    auto stream = MUST(Core::File::adopt_fd(fd, Core::File::OpenMode::Read));
    notifier->on_activation = move(m_internal_stream_data->read_notifier->on_activation);
    m_internal_stream_data->read_notifier = move(notifier);
    m_internal_stream_data->read_stream = move(stream);
}

void Request::set_buffered_request_finished_callback(BufferedRequestFinished on_buffered_request_finished)
{
    VERIFY(m_mode == Mode::Unknown);
    m_mode = Mode::Buffered;

    m_internal_buffered_data = make<InternalBufferedData>();

    on_headers_received = [this](auto& headers, auto response_code) {
        m_internal_buffered_data->response_headers = headers;
        m_internal_buffered_data->response_code = move(response_code);
    };

    on_finish = [this, on_buffered_request_finished = move(on_buffered_request_finished)](auto success, auto total_size) {
        auto output_buffer = ByteBuffer::create_uninitialized(m_internal_buffered_data->payload_stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
        m_internal_buffered_data->payload_stream.read_until_filled(output_buffer).release_value_but_fixme_should_propagate_errors();

        on_buffered_request_finished(
            success,
            total_size,
            m_internal_buffered_data->response_headers,
            m_internal_buffered_data->response_code,
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

void Request::did_finish(Badge<RequestClient>, bool success, u64 total_size)
{
    if (on_finish)
        on_finish(success, total_size);
}

void Request::did_progress(Badge<RequestClient>, Optional<u64> total_size, u64 downloaded_size)
{
    if (on_progress)
        on_progress(total_size, downloaded_size);
}

void Request::did_receive_headers(Badge<RequestClient>, HTTP::HeaderMap const& response_headers, Optional<u32> response_code)
{
    if (on_headers_received)
        on_headers_received(response_headers, response_code);
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
        m_internal_stream_data->read_stream = MUST(Core::File::adopt_fd(fd(), Core::File::OpenMode::Read));

    auto user_on_finish = move(on_finish);
    on_finish = [this](auto success, auto total_size) {
        m_internal_stream_data->success = success;
        m_internal_stream_data->total_size = total_size;
        m_internal_stream_data->request_done = true;
        m_internal_stream_data->on_finish();
    };

    m_internal_stream_data->on_finish = [this, user_on_finish = move(user_on_finish)]() {
        if (!m_internal_stream_data->user_finish_called && m_internal_stream_data->read_stream->is_eof()) {
            m_internal_stream_data->user_finish_called = true;
            user_on_finish(m_internal_stream_data->success, m_internal_stream_data->total_size);
        }
    };

    m_internal_stream_data->read_notifier->on_activation = [this, on_data_available = move(on_data_available)]() {
        static constexpr size_t buffer_size = 256 * KiB;
        static char buffer[buffer_size];

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
