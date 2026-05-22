/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromWebContent.h>

namespace Compositor {

ConnectionFromWebContent::ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport> transport, int client_id)
    : IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport), client_id)
{
}

void ConnectionFromWebContent::die()
{
    if (on_death)
        on_death(*this);
}

void ConnectionFromWebContent::ping()
{
}

}
