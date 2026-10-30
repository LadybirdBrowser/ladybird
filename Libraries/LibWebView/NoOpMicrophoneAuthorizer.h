/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/MicrophoneAuthorizer.h>

namespace WebView {

// The authorizer of a platform whose system has no say over the microphone, which leaves it to what the user allows
// each site.
class NoOpMicrophoneAuthorizer final : public MicrophoneAuthorizer {
public:
    virtual MicrophoneAuthorization authorization() const override { return MicrophoneAuthorization::Authorized; }
    virtual void request_authorization(Function<void(MicrophoneAuthorization)> on_complete) override { on_complete(MicrophoneAuthorization::Authorized); }
    virtual void open_system_settings() override { }
};

}
