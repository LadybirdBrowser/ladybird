/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/MediaSessionCoordinator.h>

namespace WebView {

namespace Detail {

struct NowPlayingBridge;

}

class WEBVIEW_API MediaSessionPlatformBackendMacOS final : public MediaSessionPlatformBackend {
public:
    MediaSessionPlatformBackendMacOS();
    virtual ~MediaSessionPlatformBackendMacOS() override;

    virtual void update(Optional<NowPlayingState> const&) override;

private:
    OwnPtr<Detail::NowPlayingBridge> m_bridge;
};

}
