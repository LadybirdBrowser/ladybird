/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Export.h>
#include <VideoDecoderServer/VideoDecoderServerControlClientEndpoint.h>
#include <VideoDecoderServer/VideoDecoderServerControlEndpoint.h>

namespace WebView {

// The control plane of a MediaServer's VideoDecoderServer, spoken only by the Browser, which launched it.
class WEBVIEW_API VideoDecoderServerControlClient final
    : public IPC::ConnectionToServer<VideoDecoderServerControlClientEndpoint, VideoDecoderServerControlEndpoint>
    , public VideoDecoderServerControlClientEndpoint {
    C_OBJECT_ABSTRACT(VideoDecoderServerControlClient);

public:
    explicit VideoDecoderServerControlClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~VideoDecoderServerControlClient() override;

private:
    virtual void die() override;
};

}
