/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibHTTP/Job.h>
#include <LibURL/URL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/HttpRequest.h>
#include <RequestServer/Protocol.h>
#include <RequestServer/Request.h>

namespace RequestServer {

class HttpProtocol final : public Protocol {
public:
    using JobType = HTTP::Job;
    using RequestType = HttpRequest;

    ~HttpProtocol() override = default;

    static void install();

private:
    HttpProtocol();

    virtual OwnPtr<Request> start_request(i32, ConnectionFromClient&, ByteString const& method, URL::URL const&, HTTP::HeaderMap const& headers, ReadonlyBytes body, Core::ProxyData proxy_data = {}) override;
};

}
