/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <LibCore/Forward.h>
#include <LibHTTP/HttpsJob.h>
#include <RequestServer/Request.h>

namespace RequestServer {

class HttpsRequest final : public Request {
public:
    virtual ~HttpsRequest() override;
    static NonnullOwnPtr<HttpsRequest> create_with_job(Badge<HttpsProtocol>&&, ConnectionFromClient&, NonnullRefPtr<HTTP::HttpsJob>, NonnullOwnPtr<Core::File>&&, i32);

    HTTP::HttpsJob& job() { return m_job; }
    HTTP::HttpsJob const& job() const { return m_job; }

    virtual URL::URL url() const override { return m_job->url(); }
    virtual void cancel() override { m_job->cancel(); }

private:
    explicit HttpsRequest(ConnectionFromClient&, NonnullRefPtr<HTTP::HttpsJob>, NonnullOwnPtr<Core::File>&&, i32);

    virtual void set_certificate(ByteString certificate, ByteString key) override;

    NonnullRefPtr<HTTP::HttpsJob> m_job;
};

}
