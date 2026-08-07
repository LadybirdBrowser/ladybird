/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/Utilities.h>

namespace WebView {

static constexpr auto SESSION_STORE_FILENAME = "Session.json"sv;
static constexpr auto SESSION_STORE_TEMPORARY_FILENAME = "Session.json.tmp"sv;
static constexpr auto VERSION_KEY = "version"sv;
static constexpr auto WINDOWS_KEY = "windows"sv;
static constexpr auto TABS_KEY = "tabs"sv;
static constexpr auto URL_KEY = "url"sv;
static constexpr auto ACTIVE_TAB_INDEX_KEY = "activeTabIndex"sv;
static constexpr auto ACTIVE_WINDOW_INDEX_KEY = "activeWindowIndex"sv;
static constexpr auto X_KEY = "x"sv;
static constexpr auto Y_KEY = "y"sv;
static constexpr auto WIDTH_KEY = "width"sv;
static constexpr auto HEIGHT_KEY = "height"sv;
static constexpr auto MAXIMIZED_KEY = "maximized"sv;
static constexpr u32 CURRENT_SESSION_STORE_VERSION = 2;

ByteString session_store_path(Profile const& profile)
{
    return LexicalPath::join(profile.paths().data, SESSION_STORE_FILENAME).string();
}

static ByteString temporary_session_store_path(Profile const& profile)
{
    return LexicalPath::join(profile.paths().data, SESSION_STORE_TEMPORARY_FILENAME).string();
}

static Optional<SessionTab> parse_tab(JsonValue const& tab_value)
{
    if (!tab_value.is_object())
        return {};

    auto url = tab_value.as_object().get_string(URL_KEY);
    if (!url.has_value())
        return {};

    auto parsed_url = URL::Parser::basic_parse(*url);
    if (!parsed_url.has_value() || parsed_url->scheme().is_empty())
        return {};

    return SessionTab { .url = parsed_url.release_value() };
}

static Optional<SessionWindow> parse_window(JsonValue const& window_value)
{
    if (!window_value.is_object())
        return {};

    auto tabs = window_value.as_object().get_array(TABS_KEY);
    if (!tabs.has_value())
        return {};

    SessionWindow window;
    window.tabs.ensure_capacity(tabs->size());

    tabs->for_each([&](JsonValue const& tab_value) {
        if (auto tab = parse_tab(tab_value); tab.has_value())
            window.tabs.append(tab.release_value());
    });

    if (window.tabs.is_empty())
        return {};

    if (auto active_tab_index = window_value.as_object().get_integer<size_t>(ACTIVE_TAB_INDEX_KEY); active_tab_index.has_value())
        window.active_tab_index = min(*active_tab_index, window.tabs.size() - 1);
    if (auto x = window_value.as_object().get_integer<i32>(X_KEY); x.has_value())
        window.x = *x;
    if (auto y = window_value.as_object().get_integer<i32>(Y_KEY); y.has_value())
        window.y = *y;
    if (auto width = window_value.as_object().get_integer<i32>(WIDTH_KEY); width.has_value())
        window.width = *width;
    if (auto height = window_value.as_object().get_integer<i32>(HEIGHT_KEY); height.has_value())
        window.height = *height;
    if (auto maximized = window_value.as_object().get_bool(MAXIMIZED_KEY); maximized.has_value())
        window.maximized = *maximized;

    return window;
}

ErrorOr<Optional<Session>> load_session(Profile const& profile)
{
    auto path = session_store_path(profile);
    if (!FileSystem::exists(path))
        return Optional<Session> {};

    auto json = read_json_file(path);
    if (json.is_error())
        return Optional<Session> {};

    auto version = json.value().get_integer<u32>(VERSION_KEY);
    if (!version.has_value() || *version != CURRENT_SESSION_STORE_VERSION)
        return Optional<Session> {};

    auto windows = json.value().get_array(WINDOWS_KEY);
    if (!windows.has_value())
        return Optional<Session> {};

    Session session;
    session.windows.ensure_capacity(windows->size());

    windows->for_each([&](JsonValue const& window_value) {
        if (auto window = parse_window(window_value); window.has_value())
            session.windows.append(window.release_value());
    });

    if (session.is_empty())
        return Optional<Session> {};

    if (auto active_window_index = json.value().get_integer<size_t>(ACTIVE_WINDOW_INDEX_KEY); active_window_index.has_value())
        session.active_window_index = min(*active_window_index, session.windows.size() - 1);

    return session;
}

static JsonValue serialize_session(Session const& session)
{
    JsonObject root;
    root.set(VERSION_KEY, CURRENT_SESSION_STORE_VERSION);
    root.set(ACTIVE_WINDOW_INDEX_KEY, session.active_window_index);

    JsonArray windows;
    windows.ensure_capacity(session.windows.size());

    for (auto const& window : session.windows) {
        JsonObject window_object;
        window_object.set(ACTIVE_TAB_INDEX_KEY, window.active_tab_index);
        if (window.x.has_value())
            window_object.set(X_KEY, *window.x);
        if (window.y.has_value())
            window_object.set(Y_KEY, *window.y);
        if (window.width.has_value())
            window_object.set(WIDTH_KEY, *window.width);
        if (window.height.has_value())
            window_object.set(HEIGHT_KEY, *window.height);
        window_object.set(MAXIMIZED_KEY, window.maximized);

        JsonArray tabs;
        tabs.ensure_capacity(window.tabs.size());

        for (auto const& tab : window.tabs) {
            JsonObject tab_object;
            tab_object.set(URL_KEY, tab.url.serialize());
            tabs.must_append(move(tab_object));
        }

        window_object.set(TABS_KEY, move(tabs));
        windows.must_append(move(window_object));
    }

    root.set(WINDOWS_KEY, move(windows));
    return root;
}

ErrorOr<void> save_session(Profile const& profile, Session const& session)
{
    if (session.is_empty())
        return clear_session(profile);

    auto path = session_store_path(profile);
    auto temporary_path = temporary_session_store_path(profile);

    TRY(Core::Directory::create(profile.paths().data, Core::Directory::CreateDirectories::Yes, 0700));

    {
        auto file = TRY(Core::File::open(temporary_path, Core::File::OpenMode::Write));
        TRY(file->write_until_depleted(serialize_session(session).serialized()));
    }

    TRY(Core::System::rename(temporary_path, path));
    return {};
}

ErrorOr<void> clear_session(Profile const& profile)
{
    auto path = session_store_path(profile);
    if (!FileSystem::exists(path))
        return {};
    TRY(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    return {};
}

}
