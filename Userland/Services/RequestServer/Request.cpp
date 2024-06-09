/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Request.h>

namespace RequestServer {

Request::Request(ConnectionFromClient& client, NonnullOwnPtr<Core::File>&& output_stream, i32 request_id)
    : m_client(client)
    , m_id(request_id)
    , m_output_stream(move(output_stream))
{
}

void Request::stop()
{
    m_client.did_finish_request({}, *this, false);
}

void Request::set_response_headers(HTTP::HeaderMap response_headers)
{
    m_response_headers = move(response_headers);
    m_client.did_receive_headers({}, *this);
}

void Request::set_certificate(ByteString, ByteString)
{
}

void Request::did_finish(bool success)
{
    m_client.did_finish_request({}, *this, success);
}

void Request::did_progress(Optional<u64> total_size, u64 downloaded_size)
{
    m_total_size = total_size;
    m_downloaded_size = downloaded_size;
    m_client.did_progress_request({}, *this);
}

void Request::did_request_certificates()
{
    m_client.did_request_certificates({}, *this);
}

}
