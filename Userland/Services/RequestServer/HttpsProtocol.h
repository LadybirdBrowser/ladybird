/*
 * Copyright (c) 2018-2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibHTTP/HttpsJob.h>
#include <LibURL/URL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/HttpsRequest.h>
#include <RequestServer/Protocol.h>
#include <RequestServer/Request.h>

namespace RequestServer {

class HttpsProtocol final : public Protocol {
public:
    using JobType = HTTP::HttpsJob;
    using RequestType = HttpsRequest;

    ~HttpsProtocol() override = default;

    static void install();

private:
    HttpsProtocol();

    virtual OwnPtr<Request> start_request(i32, ConnectionFromClient&, ByteString const& method, URL::URL const&, HTTP::HeaderMap const& headers, ReadonlyBytes body, Core::ProxyData proxy_data = {}) override;
};

}
