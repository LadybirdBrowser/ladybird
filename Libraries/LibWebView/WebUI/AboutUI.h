/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

// about:about, which lists every page in WebUI::pages().
class AboutUI final : public WebUI {
    WEB_UI(AboutUI);

private:
    virtual void register_interfaces() override;

    void load_pages();
};

}
