/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <LibURL/URL.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Options.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

struct SiteSetting {
    SiteSetting();

    bool enabled_globally { false };
    OrderedHashTable<String> site_filters;
};

enum class DoNotTrack {
    No,
    Yes,
};

class SettingsObserver {
public:
    explicit SettingsObserver();
    virtual ~SettingsObserver();

    virtual void new_tab_page_url_changed() { }
    virtual void languages_changed() { }
    virtual void search_engine_changed() { }
    virtual void autocomplete_engine_changed() { }
    virtual void autoplay_settings_changed() { }
    virtual void do_not_track_changed() { }
    virtual void dns_settings_changed() { }
};

class Settings {
public:
    static Settings create(Badge<Application>);

    JsonValue serialize_json() const;

    void restore_defaults();

    URL::URL const& new_tab_page_url() const { return m_new_tab_page_url; }
    void set_new_tab_page_url(URL::URL);

    static Vector<String> parse_json_languages(JsonValue const&);
    Vector<String> const& languages() const { return m_languages; }
    void set_languages(Vector<String>);

    Optional<SearchEngine> const& search_engine() const { return m_search_engine; }
    void set_search_engine(Optional<StringView> search_engine_name);

    static Optional<SearchEngine> parse_custom_search_engine(JsonValue const&);
    void add_custom_search_engine(SearchEngine);
    void remove_custom_search_engine(SearchEngine const&);

    Optional<AutocompleteEngine> const& autocomplete_engine() const { return m_autocomplete_engine; }
    void set_autocomplete_engine(Optional<StringView> autocomplete_engine_name);

    SiteSetting const& autoplay_settings() const { return m_autoplay; }
    void set_autoplay_enabled_globally(bool);
    void add_autoplay_site_filter(String const&);
    void remove_autoplay_site_filter(String const&);
    void remove_all_autoplay_site_filters();

    DoNotTrack do_not_track() const { return m_do_not_track; }
    void set_do_not_track(DoNotTrack);

    static DNSSettings parse_dns_settings(JsonValue const&);
    DNSSettings const& dns_settings() const { return m_dns_settings; }
    void set_dns_settings(DNSSettings const&, bool override_by_command_line = false);

    static void add_observer(Badge<SettingsObserver>, SettingsObserver&);
    static void remove_observer(Badge<SettingsObserver>, SettingsObserver&);

private:
    explicit Settings(ByteString settings_path);

    void persist_settings();

    Optional<SearchEngine> find_search_engine_by_name(StringView name);

    ByteString m_settings_path;

    URL::URL m_new_tab_page_url;
    Vector<String> m_languages;
    Optional<SearchEngine> m_search_engine;
    Vector<SearchEngine> m_custom_search_engines;
    Optional<AutocompleteEngine> m_autocomplete_engine;
    SiteSetting m_autoplay;
    DoNotTrack m_do_not_track { DoNotTrack::No };
    DNSSettings m_dns_settings { SystemDNS() };
    bool m_dns_override_by_command_line { false };

    Vector<SettingsObserver&> m_observers;
};

}
