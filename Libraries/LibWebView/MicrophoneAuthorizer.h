/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/Export.h>

namespace WebView {

// Whether the operating system lets the browser itself use the microphone, whatever the user allows each site.
enum class MicrophoneAuthorization : u8 {
    // The system asks the user the first time the browser asks it.
    NotDetermined,
    Authorized,
    Denied,
};

// The system's consent for the microphone, which the UI process gets before any site may capture.
class WEBVIEW_API MicrophoneAuthorizer {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~MicrophoneAuthorizer() = default;

    virtual MicrophoneAuthorization authorization() const = 0;

    // on_complete runs on the main thread, possibly before this returns.
    virtual void request_authorization(Function<void(MicrophoneAuthorization)> on_complete) = 0;

    // Shows the user the setting that gives the browser the microphone back.
    virtual void open_system_settings() = 0;
};

}
