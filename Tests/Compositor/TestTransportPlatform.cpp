/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromWebContent.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CompositorConnection.h>

TEST_CASE(non_windows_transport_initialization_disconnects_web_content)
{
    Core::EventLoop event_loop;
    auto paired_transport = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto client_transport = TRY_OR_FAIL(paired_transport.remote_handle.create_transport());
    auto client = adopt_ref(*new WebView::CompositorConnection(move(client_transport)));
    auto compositor_state = Compositor::CompositorState::create({}, false);
    auto connection = Compositor::ConnectionFromWebContent::construct(move(paired_transport.local), move(compositor_state), 1);

    bool disconnected = false;
    connection->set_on_death([&](auto&) {
        disconnected = true;
    });

    TRY_OR_FAIL(client->post_message(Messages::CompositorWebContentServer::InitTransport(42)));
    client->transport().flush();
    connection->dispatch_pending_messages();

    EXPECT(disconnected);
    EXPECT(!connection->is_open());
}
