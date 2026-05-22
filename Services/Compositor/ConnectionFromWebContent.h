/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace Compositor {

class ConnectionFromWebContent final
    : public IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint> {
    C_OBJECT(ConnectionFromWebContent)

public:
    virtual void die() override;

    Function<void(ConnectionFromWebContent&)> on_death;

private:
    explicit ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport>, int client_id);

    virtual void ping() override;
};

}
