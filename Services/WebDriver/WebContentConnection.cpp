/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Promise.h>
#include <WebDriver/Client.h>
#include <WebDriver/WebContentConnection.h>

namespace WebDriver {

WebContentConnection::WebContentConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<WebDriverClientEndpoint, WebDriverServerEndpoint>(*this, move(transport), 1)
{
}

void WebContentConnection::die()
{
    if (on_close)
        on_close();
}

void WebContentConnection::driver_execution_complete(int request_id, Web::WebDriver::Response response)
{
    m_request_id_allocator.deallocate(request_id);
    auto request_promise = m_pending_requests.take(request_id);
    if (!request_promise.has_value()) [[unlikely]] {
        dbgln("WebContentConnection::driver_execution_complete: No promise found with request ID of {}", request_id);
        return;
    }

    if (response.is_error()) {
        (*request_promise)->reject(response.release_error());
        return;
    }
    (*request_promise)->resolve(response.release_value());
}

int WebContentConnection::create_pending_request(NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> promise)
{
    auto request_id = m_request_id_allocator.allocate();
    auto result = m_pending_requests.set(request_id, move(promise));
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    return request_id;
}

}
