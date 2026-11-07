/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <ImageDecoder/ImageDecoderControlClientEndpoint.h>
#include <ImageDecoder/ImageDecoderControlEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/ChildIdentity.h>
#include <LibWebView/Export.h>

namespace WebView {

// The control plane of an ImageDecoder, spoken only by the Browser, which launched it. A decoder parses untrusted images,
// so this connection carries nothing back from it.
class WEBVIEW_API ImageDecoderControlClient final
    : public IPC::ConnectionToServer<ImageDecoderControlClientEndpoint, ImageDecoderControlEndpoint>
    , public ImageDecoderControlClientEndpoint {
    C_OBJECT_ABSTRACT(ImageDecoderControlClient);

public:
    explicit ImageDecoderControlClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~ImageDecoderControlClient() override;

    u64 child_id() const { return m_child_identity.child_id; }
    void set_child_identity(ChildIdentity identity) { m_child_identity = identity; }

private:
    virtual void die() override;

    ChildIdentity m_child_identity;
};

}
