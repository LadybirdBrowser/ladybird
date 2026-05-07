/*
 * Copyright (c) 2026, Jorge Pais <jorge27pais@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/HistoryStore.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class HistoryUI final : public WebUI {
    WEB_UI(HistoryUI);

private:
    virtual void register_interfaces() override;

    void load_history();
    void delete_history_entry(JsonValue const& data);
};

}
