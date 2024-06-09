/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/MemoryStream.h>
#include <AK/RefCounted.h>
#include <AK/WeakPtr.h>
#include <LibCore/Notifier.h>
#include <LibHTTP/HeaderMap.h>
#include <LibIPC/Forward.h>

namespace Protocol {

class RequestClient;

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

    using BufferedRequestFinished = Function<void(bool success, u64 total_size, HTTP::HeaderMap const& response_headers, Optional<u32> response_code, ReadonlyBytes payload)>;

    // Configure the request such that the entirety of the response data is buffered. The callback receives that data and
    // the response headers all at once. Using this method is mutually exclusive with `set_unbuffered_data_received_callback`.
    void set_buffered_request_finished_callback(BufferedRequestFinished);

    using HeadersReceived = Function<void(HTTP::HeaderMap const& response_headers, Optional<u32> response_code)>;
    using DataReceived = Function<void(ReadonlyBytes data)>;
    using RequestFinished = Function<void(bool success, u64 total_size)>;

    // Configure the request such that the response data is provided unbuffered as it is received. Using this method is
    // mutually exclusive with `set_buffered_request_finished_callback`.
    void set_unbuffered_request_callbacks(HeadersReceived, DataReceived, RequestFinished);

    Function<void(Optional<u64> total_size, u64 downloaded_size)> on_progress;
    Function<CertificateAndKey()> on_certificate_requested;

    void did_finish(Badge<RequestClient>, bool success, u64 total_size);
    void did_progress(Badge<RequestClient>, Optional<u64> total_size, u64 downloaded_size);
    void did_receive_headers(Badge<RequestClient>, HTTP::HeaderMap const& response_headers, Optional<u32> response_code);
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
    };

    struct InternalStreamData {
        InternalStreamData() { }

        OwnPtr<Stream> read_stream;
        RefPtr<Core::Notifier> read_notifier;
        bool success;
        u32 total_size { 0 };
        bool request_done { false };
        Function<void()> on_finish {};
        bool user_finish_called { false };
    };

    OwnPtr<InternalBufferedData> m_internal_buffered_data;
    OwnPtr<InternalStreamData> m_internal_stream_data;
};

}
