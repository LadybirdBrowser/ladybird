/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/TestConnection.h>

namespace WebContent {

TestConnection::TestConnection(NonnullOwnPtr<IPC::Transport> transport, WebContent::ConnectionFromClient& client)
    : IPC::ConnectionFromClient<WebContentTestClientEndpoint, WebContentTestServerEndpoint>(*this, move(transport), 0)
    , m_client(client)
{
}

TestConnection::~TestConnection() = default;

// This transport has its own send queue, so what the main connection queued first has to reach its socket first.
void TestConnection::prepare_to_send()
{
    m_client.transport().flush();
}

void TestConnection::die()
{
}

void TestConnection::reset_session_history_for_testing(Web::PageId page_id)
{
    m_client.dispatch_pending_messages();

    if (auto page = m_client.page(page_id); page.has_value()) {
        as<Web::HTML::LocalTraversableNavigable>(*page->page().local_root_navigable()).reset_session_history_for_testing();
        auto active_entry = page->page().local_root_navigable()->active_session_history_entry();
        VERIFY(active_entry);
        async_did_reset_session_history_for_testing(page_id, Web::HTML::create_session_history_entry_descriptor(*active_entry));
    }
}

}
