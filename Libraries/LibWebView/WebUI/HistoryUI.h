/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

class HistoryUI final : public WebUI {
    WEB_UI(HistoryUI);

private:
    virtual void register_interfaces() override;

    void load_history_entries(JsonValue const&);
    void remove_history_entry(JsonValue const&);
    void forget_history_site(JsonValue const&);
};

}
