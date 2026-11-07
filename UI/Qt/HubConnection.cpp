/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/HubConnection.h>

namespace Ladybird {

static HubConnection* s_the;

HubConnection::HubConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>(*this, move(transport))
{
    s_the = this;
}

HubConnection::~HubConnection()
{
    s_the = nullptr;
}

HubConnection& HubConnection::the()
{
    return *s_the;
}

void HubConnection::die()
{
}

void HubConnection::hub_is_ready()
{
}

}
