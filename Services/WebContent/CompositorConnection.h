/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>

namespace WebContent {

class CompositorConnection final
    : public IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>
    , public CompositorWebContentClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorConnection)

public:
    explicit CompositorConnection(NonnullOwnPtr<IPC::Transport>);

private:
    virtual void die() override;

    virtual void did_connect() override;
};

}
