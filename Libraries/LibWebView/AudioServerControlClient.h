/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AudioServer/AudioServerControlClientEndpoint.h>
#include <AudioServer/AudioServerControlEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Export.h>

namespace WebView {

// The control plane of a renderer's AudioServer, spoken only by the Browser, which launched it.
class WEBVIEW_API AudioServerControlClient final
    : public IPC::ConnectionToServer<AudioServerControlClientEndpoint, AudioServerControlEndpoint>
    , public AudioServerControlClientEndpoint {
    C_OBJECT_ABSTRACT(AudioServerControlClient);

public:
    using InitTransport = Messages::AudioServerControl::InitTransport;

    explicit AudioServerControlClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~AudioServerControlClient() override;

private:
    virtual void die() override;
};

}
