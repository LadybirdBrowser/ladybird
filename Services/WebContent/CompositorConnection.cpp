/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebContent/CompositorConnection.h>

namespace WebContent {

CompositorConnection::CompositorConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport))
{
}

void CompositorConnection::die()
{
}

void CompositorConnection::did_connect()
{
}

}
