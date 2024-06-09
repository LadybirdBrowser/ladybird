/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibURL/URL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/HttpCommon.h>
#include <RequestServer/HttpProtocol.h>
#include <RequestServer/Request.h>

namespace RequestServer {

HttpProtocol::HttpProtocol()
    : Protocol("http")
{
}

OwnPtr<Request> HttpProtocol::start_request(i32 request_id, ConnectionFromClient& client, ByteString const& method, URL::URL const& url, HTTP::HeaderMap const& headers, ReadonlyBytes body, Core::ProxyData proxy_data)
{
    return Detail::start_request(Badge<HttpProtocol> {}, request_id, client, method, url, headers, body, get_pipe_for_request(), proxy_data);
}

void HttpProtocol::install()
{
    Protocol::install(adopt_own(*new HttpProtocol()));
}

}
