/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/Forward.h>
#include <LibHTTP/Forward.h>
#include <RequestServer/Request.h>

namespace RequestServer {

class HttpRequest final : public Request {
public:
    virtual ~HttpRequest() override;
    static NonnullOwnPtr<HttpRequest> create_with_job(Badge<HttpProtocol>&&, ConnectionFromClient&, NonnullRefPtr<HTTP::Job>, NonnullOwnPtr<Core::File>&&, i32);

    HTTP::Job& job() { return m_job; }
    HTTP::Job const& job() const { return m_job; }

    virtual URL::URL url() const override { return m_job->url(); }
    virtual void cancel() override { m_job->cancel(); }

private:
    explicit HttpRequest(ConnectionFromClient&, NonnullRefPtr<HTTP::Job>, NonnullOwnPtr<Core::File>&&, i32);

    NonnullRefPtr<HTTP::Job> m_job;
};

}
