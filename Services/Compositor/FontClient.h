/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/FontClientEndpoint.h>
#include <LibCompositing/FontServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>

namespace Compositor {

class FontClient final : public IPC::ConnectionToServer<FontClientEndpoint, FontServerEndpoint> {
    C_OBJECT(FontClient);

public:
    using InitTransport = Messages::FontServer::InitTransport;
    virtual ~FontClient() override = default;

private:
    explicit FontClient(NonnullOwnPtr<IPC::Transport>);
    virtual void die() override;
};

}
