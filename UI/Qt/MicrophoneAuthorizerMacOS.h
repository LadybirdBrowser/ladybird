/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWebView/MicrophoneAuthorizer.h>

namespace Ladybird {

namespace Detail {

struct PendingMicrophoneRequests;

}

// The system asks the user on behalf of the process responsible for the browser: the app, or the terminal that ran it.
// The AudioServer that opens the device answers to the same one.
class MicrophoneAuthorizerMacOS final : public WebView::MicrophoneAuthorizer {
public:
    MicrophoneAuthorizerMacOS();
    virtual ~MicrophoneAuthorizerMacOS() override;

    virtual WebView::MicrophoneAuthorization authorization() const override;
    virtual void request_authorization(Function<void(WebView::MicrophoneAuthorization)> on_complete) override;
    virtual void open_system_settings() override;

private:
    NonnullRefPtr<Detail::PendingMicrophoneRequests> m_pending_requests;
};

}
