/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

class VersionUI final : public WebUI {
    WEB_UI(VersionUI);

private:
    virtual void register_interfaces() override;

    void load_version_info();
};

}
