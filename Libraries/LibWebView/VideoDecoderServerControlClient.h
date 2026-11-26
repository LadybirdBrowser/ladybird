/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/ChildIdentity.h>
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

    u64 child_id() const { return m_child_identity.child_id; }
    void set_child_identity(ChildIdentity identity) { m_child_identity = identity; }

private:
    virtual void die() override;

    ChildIdentity m_child_identity;
};

}
