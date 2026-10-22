/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorControlClientEndpoint.h>
#include <Compositor/CompositorControlServerEndpoint.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CompositorClient final
    : public IPC::ConnectionToServer<CompositorControlClientEndpoint, CompositorControlServerEndpoint>
    , public CompositorControlClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorClient)

public:
    explicit CompositorClient(NonnullOwnPtr<IPC::Transport>);

    Function<void()> on_death;
    Function<void(Optional<Gfx::GpuDeviceIdentity>)> on_gpu_device_identity_reported;

private:
    virtual void die() override;

    virtual void gpu_device_identity_reported(Optional<Gfx::GpuDeviceIdentity>) override;
};

}
