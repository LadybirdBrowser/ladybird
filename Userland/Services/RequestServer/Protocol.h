/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibCore/Proxy.h>
#include <LibHTTP/HeaderMap.h>
#include <LibURL/URL.h>
#include <RequestServer/Forward.h>

namespace RequestServer {

class Protocol {
public:
    virtual ~Protocol() = default;

    ByteString const& name() const { return m_name; }
    virtual OwnPtr<Request> start_request(i32, ConnectionFromClient&, ByteString const& method, URL::URL const&, HTTP::HeaderMap const& headers, ReadonlyBytes body, Core::ProxyData proxy_data = {}) = 0;

    static Protocol* find_by_name(ByteString const&);

protected:
    explicit Protocol(ByteString const& name);
    struct Pipe {
        int read_fd { -1 };
        int write_fd { -1 };
    };
    static ErrorOr<Pipe> get_pipe_for_request();

    static void install(NonnullOwnPtr<Protocol>);

private:
    ByteString m_name;
};

}
