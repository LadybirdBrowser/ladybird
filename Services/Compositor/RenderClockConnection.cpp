/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/RenderClockConnection.h>

namespace Compositor {

RenderClockConnection::RenderClockConnection(NonnullOwnPtr<IPC::Transport> transport, int client_id)
    : IPC::ConnectionFromClient<RenderClockClientEndpoint, RenderClockServerEndpoint>(*this, move(transport), client_id)
{
}

void RenderClockConnection::die()
{
    on_raw_message = nullptr;
}

ErrorOr<OwnPtr<IPC::MessageBuffer>> RenderClockConnection::handle_raw(IPC::RawMessage& message)
{
    if (on_raw_message)
        on_raw_message(message);
    return nullptr;
}

}
