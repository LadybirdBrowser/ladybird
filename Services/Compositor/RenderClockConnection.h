/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/RenderClockClientEndpoint.h>
#include <Compositor/RenderClockServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Compositor {

// The Compositor end of a WebContent process's render clock channel, which delivers display ticks to the process's
// render clock thread without passing through its main thread. The ConnectionFromWebContent the channel was offered on
// owns it, and hears its requests.
class RenderClockConnection final
    : public IPC::ConnectionFromClient<RenderClockClientEndpoint, RenderClockServerEndpoint> {
    C_OBJECT(RenderClockConnection);

public:
    virtual ~RenderClockConnection() override = default;

    Function<void(Web::CompositorContextId, double maximum_frames_per_second)> on_request_clock_tick;

private:
    RenderClockConnection(NonnullOwnPtr<IPC::Transport>, int client_id);

    virtual void die() override;
    virtual void request_clock_tick(Web::CompositorContextId, double maximum_frames_per_second) override;
};

}
