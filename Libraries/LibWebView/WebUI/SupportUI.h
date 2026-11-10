/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

// about:support, which shows what the system probe found and what each platform feature was decided to be.
class SupportUI final : public WebUI {
    WEB_UI(SupportUI);

public:
    virtual ~SupportUI() override;

    // Tells every open page to show the decisions anew.
    static void platform_features_changed();

private:
    virtual void register_interfaces() override;

    void load_platform_features();
};

}
