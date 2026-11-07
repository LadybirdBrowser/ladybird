/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/HubClientEndpoint.h>
#include <Hub/HubServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>

namespace Ladybird {

// The UI's end of its connection to the Hub.
class HubConnection final
    : public IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>
    , public HubClientEndpoint {
    C_OBJECT_ABSTRACT(HubConnection)

public:
    explicit HubConnection(NonnullOwnPtr<IPC::Transport>);
    virtual ~HubConnection() override;

    static HubConnection& the();

private:
    virtual void die() override;

    // ^HubClientEndpoint
    virtual void hub_is_ready() override;
};

}
