/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/MemoryStream.h>
#include <AK/RefCounted.h>
#include <AK/WeakPtr.h>
#include <LibCore/Notifier.h>
#include <LibHTTP/HeaderMap.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>

namespace Requests {

class RequestClient;

class ReadStream {
public:
    static ErrorOr<NonnullOwnPtr<ReadStream>> create(int reader_fd);

    NonnullRefPtr<Core::Notifier> const& notifier() const { return m_notifier; }

    bool is_eof() const { return m_stream->is_eof(); }

    ErrorOr<Bytes> read_some(Bytes bytes) { return m_stream->read_some(bytes); }

private:
    ReadStream(NonnullOwnPtr<Stream> stream, NonnullRefPtr<Core::Notifier> notifier)
        : m_stream(move(stream))
        , m_notifier(move(notifier))
    {
    }

    NonnullOwnPtr<Stream> m_stream;
    NonnullRefPtr<Core::Notifier> m_notifier;
};

class Request : public RefCounted<Request> {
public:
    struct CertificateAndKey {
        ByteString certificate;
        ByteString key;
    };

    static NonnullRefPtr<Request> create_from_id(Badge<RequestClient>, RequestClient& client, i32 request_id)
    {
        return adopt_ref(*new Request(client, request_id));
    }

    int id() const { return m_request_id; }
    int fd() const { return m_fd; }
    bool stop();

    using BufferedRequestFinished = Function<void(u64 total_size, RequestTimingInfo const& timing_info, Optional<NetworkError> const& network_error, HTTP::HeaderMap const& response_headers, Optional<u32> response_code, Optional<String> reason_phrase, ReadonlyBytes payload)>;

    // Configure the request such that the entirety of the response data is buffered. The callback receives that data and
    // the response headers all at once. Using this method is mutually exclusive with `set_unbuffered_data_received_callback`.
    void set_buffered_request_finished_callback(BufferedRequestFinished);

    using HeadersReceived = Function<void(HTTP::HeaderMap const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase)>;
    using DataReceived = Function<void(ReadonlyBytes data)>;
    using RequestFinished = Function<void(u64 total_size, RequestTimingInfo const& timing_info, Optional<NetworkError> network_error)>;

    // Configure the request such that the response data is provided unbuffered as it is received. Using this method is
    // mutually exclusive with `set_buffered_request_finished_callback`.
    void set_unbuffered_request_callbacks(HeadersReceived, DataReceived, RequestFinished);

    Function<CertificateAndKey()> on_certificate_requested;

    void did_finish(Badge<RequestClient>, u64 total_size, RequestTimingInfo const& timing_info, Optional<NetworkError> const& network_error);
    void did_receive_headers(Badge<RequestClient>, HTTP::HeaderMap const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase);
    void did_request_certificates(Badge<RequestClient>);

    RefPtr<Core::Notifier>& write_notifier(Badge<RequestClient>) { return m_write_notifier; }
    void set_request_fd(Badge<RequestClient>, int fd);

private:
    explicit Request(RequestClient&, i32 request_id);

    void set_up_internal_stream_data(DataReceived on_data_available);

    WeakPtr<RequestClient> m_client;
    int m_request_id { -1 };
    RefPtr<Core::Notifier> m_write_notifier;
    int m_fd { -1 };

    enum class Mode {
        Buffered,
        Unbuffered,
        Unknown,
    };
    Mode m_mode { Mode::Unknown };

    HeadersReceived on_headers_received;
    RequestFinished on_finish;

    struct InternalBufferedData {
        AllocatingMemoryStream payload_stream;
        HTTP::HeaderMap response_headers;
        Optional<u32> response_code;
        Optional<String> reason_phrase;
    };

    struct InternalStreamData {
        InternalStreamData() { }

        OwnPtr<ReadStream> read_stream;
        RefPtr<Core::Notifier> read_notifier;
        u32 total_size { 0 };
        Optional<NetworkError> network_error;
        bool request_done { false };
        RequestTimingInfo timing_info;
        Function<void()> on_finish {};
        bool user_finish_called { false };
    };

    OwnPtr<InternalBufferedData> m_internal_buffered_data;
    OwnPtr<InternalStreamData> m_internal_stream_data;
};

}
