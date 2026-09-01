/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Compositor/CompositorFontClientEndpoint.h>
#include <Compositor/CompositorFontServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>

namespace Compositor {

class FontClient final : public IPC::ConnectionToServer<CompositorFontClientEndpoint, CompositorFontServerEndpoint> {
    C_OBJECT(FontClient);

public:
    using InitTransport = Messages::CompositorFontServer::InitTransport;
    virtual ~FontClient() override = default;

private:
    explicit FontClient(NonnullOwnPtr<IPC::Transport>);
    virtual void die() override;
};

}
