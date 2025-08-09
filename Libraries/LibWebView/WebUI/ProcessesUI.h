/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Forward.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class WEBVIEW_API ProcessesUI : public WebUI {
    WEB_UI(ProcessesUI);

private:
    virtual void register_interfaces() override;

    void update_process_statistics();
};

}
