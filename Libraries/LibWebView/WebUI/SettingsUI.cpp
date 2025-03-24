/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/WebUI/SettingsUI.h>

namespace WebView {

void SettingsUI::register_interfaces()
{
    register_interface("loadCurrentSettings"sv, [this](auto const&) {
        load_current_settings();
    });
    register_interface("restoreDefaultSettings"sv, [this](auto const&) {
        restore_default_settings();
    });
    register_interface("setNewTabPageURL"sv, [this](auto const& data) {
        set_new_tab_page_url(data);
    });
    register_interface("loadAvailableSearchEngines"sv, [this](auto const&) {
        load_available_search_engines();
    });
    register_interface("setSearchEngine"sv, [this](auto const& data) {
        set_search_engine(data);
    });
}

void SettingsUI::load_current_settings()
{
    auto settings = WebView::Application::settings().serialize_json();
    async_send_message("loadSettings"sv, settings);
}

void SettingsUI::restore_default_settings()
{
    WebView::Application::settings().restore_defaults();
    load_current_settings();
}

void SettingsUI::set_new_tab_page_url(JsonValue const& new_tab_page_url)
{
    if (!new_tab_page_url.is_string())
        return;

    auto parsed_new_tab_page_url = URL::Parser::basic_parse(new_tab_page_url.as_string());
    if (!parsed_new_tab_page_url.has_value())
        return;

    WebView::Application::settings().set_new_tab_page_url(parsed_new_tab_page_url.release_value());
}

void SettingsUI::load_available_search_engines()
{
    JsonArray engines;
    for (auto const& engine : search_engines())
        engines.must_append(engine.name);

    async_send_message("loadSearchEngines"sv, move(engines));
}

void SettingsUI::set_search_engine(JsonValue const& search_engine)
{
    if (search_engine.is_null())
        WebView::Application::settings().set_search_engine({});
    else if (search_engine.is_string())
        WebView::Application::settings().set_search_engine(search_engine.as_string());
}

}
