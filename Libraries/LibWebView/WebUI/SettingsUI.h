/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

class SettingsUI : public WebUI {
    WEB_UI(SettingsUI);

private:
    virtual void register_interfaces() override;

    void load_current_settings();
    void restore_default_settings();

    void set_new_tab_page_url(JsonValue const&);

    void load_available_engines();
    void set_search_engine(JsonValue const&);
    void set_autocomplete_engine(JsonValue const&);

    void load_forcibly_enabled_site_settings();
    void set_site_setting_enabled_globally(JsonValue const&);
    void add_site_setting_filter(JsonValue const&);
    void remove_site_setting_filter(JsonValue const&);
    void remove_all_site_setting_filters(JsonValue const&);

    void set_do_not_track(JsonValue const&);
};

}
