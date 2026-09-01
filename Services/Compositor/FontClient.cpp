/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/FontClient.h>
#include <LibCore/Process.h>

namespace Compositor {

FontClient::FontClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorFontClientEndpoint, CompositorFontServerEndpoint>(*this, move(transport))
{
}

void FontClient::die()
{
    Core::Process::terminate_immediately(0);
}

}
