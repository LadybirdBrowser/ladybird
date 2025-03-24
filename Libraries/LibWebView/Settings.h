/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

class SettingsObserver {
public:
    SettingsObserver();
    virtual ~SettingsObserver();

    virtual void new_tab_page_url_changed() { }
    virtual void search_engine_changed() { }
};

class Settings {
public:
    static Settings create(Badge<Application>);

    JsonValue serialize_json() const;

    void restore_defaults();

    URL::URL const& new_tab_page_url() const { return m_new_tab_page_url; }
    void set_new_tab_page_url(URL::URL);

    Optional<SearchEngine> const& search_engine() const { return m_search_engine; }
    void set_search_engine(Optional<StringView> search_engine_name);

    static void add_observer(Badge<SettingsObserver>, SettingsObserver&);
    static void remove_observer(Badge<SettingsObserver>, SettingsObserver&);

private:
    explicit Settings(ByteString settings_path);

    void persist_settings();

    ByteString m_settings_path;

    URL::URL m_new_tab_page_url;
    Optional<SearchEngine> m_search_engine;

    Vector<SettingsObserver&> m_observers;
};

}
