/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionFromClient.h>
#include <WebContent/Forward.h>
#include <WebContent/WebContentTestClientEndpoint.h>
#include <WebContent/WebContentTestServerEndpoint.h>

namespace WebContent {

// The test-only half of the WebContent protocol. The UI process connects this endpoint over a second transport, and
// only when it runs in test mode, so outside tests these messages have nowhere to go.
class TestConnection final
    : public IPC::ConnectionFromClient<WebContentTestClientEndpoint, WebContentTestServerEndpoint> {
    C_OBJECT(TestConnection);

public:
    ~TestConnection() override;

    void prepare_to_send();

    virtual void die() override;

private:
    TestConnection(NonnullOwnPtr<IPC::Transport>, WebContent::ConnectionFromClient&);

    virtual void reset_session_history_for_testing(u64 page_id) override;

    WebContent::ConnectionFromClient& m_client;
};

}
