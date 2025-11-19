/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Promise.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Response.h>
#include <WebDriver/Client.h>
#include <WebDriver/WebContentConnection.h>

namespace WebDriver {

class Session : public RefCounted<Session> {
public:
    struct NewSession {
        NonnullRefPtr<Session> session;
        JsonValue capabilities;
    };

    static ErrorOr<NonnullRefPtr<Core::Promise<NewSession, Web::WebDriver::Error>>> create(NonnullRefPtr<Client> client, JsonValue capabilities, Web::WebDriver::SessionFlags flags);
    ~Session();

    enum class AllowInvalidWindowHandle {
        No,
        Yes,
    };
    static ErrorOr<NonnullRefPtr<Session>, Web::WebDriver::Error> find_session(StringView session_id, Web::WebDriver::SessionFlags = Web::WebDriver::SessionFlags::Default, AllowInvalidWindowHandle = AllowInvalidWindowHandle::No);
    static size_t session_count(Web::WebDriver::SessionFlags);

    struct Window {
        String handle;
        NonnullRefPtr<WebContentConnection> web_content_connection;
    };

    WebContentConnection& web_content_connection() const
    {
        auto current_window = m_windows.get(m_current_window_handle);
        VERIFY(current_window.has_value());

        return current_window->web_content_connection;
    }

    NonnullRefPtr<Core::Promise<Empty, Web::WebDriver::Error>> close();

    String session_id() const { return m_session_id; }
    Web::WebDriver::SessionFlags session_flags() const { return m_session_flags; }
    String const& current_window_handle() const { return m_current_window_handle; }

    bool has_window_handle(StringView handle) const { return m_windows.contains(handle); }
    void add_window_handle_became_available_callback(String const& handle, Function<void()> callback);
    void remove_window_handle_became_available_callback(String const& handle);

    void set_timeouts(JsonValue, NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> top_level_promise);
    void close_window(NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> top_level_promise);
    void switch_to_window(StringView, NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> top_level_promise);
    Web::WebDriver::Response get_window_handles() const;
    ErrorOr<void, Web::WebDriver::Error> ensure_current_window_handle_is_valid() const;

    template<typename Action>
    void perform_async_action(NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> promise, Action&& action)
    {
        auto& connection = web_content_connection();
        auto request_id = connection.create_pending_request(move(promise));
        action(connection, request_id);
    }

private:
    Session(NonnullRefPtr<Client> client, JsonObject const& capabilities, String session_id, Web::WebDriver::SessionFlags flags);

    using ServerPromise = Core::Promise<Empty>;
    ErrorOr<NonnullRefPtr<ServerPromise>> start(LaunchBrowserCallback const&);
    ErrorOr<NonnullRefPtr<Core::LocalServer>> create_server(NonnullRefPtr<ServerPromise> promise);

    NonnullRefPtr<Client> m_client;
    Web::WebDriver::LadybirdOptions m_options;

    String m_session_id;
    Web::WebDriver::SessionFlags m_session_flags { Web::WebDriver::SessionFlags::Default };

    HashMap<String, Window> m_windows;
    String m_current_window_handle;

    Optional<ByteString> m_web_content_socket_path;
    Optional<Core::Process> m_browser_process;

    RefPtr<Core::LocalServer> m_web_content_server;

    Web::WebDriver::PageLoadStrategy m_page_load_strategy { Web::WebDriver::PageLoadStrategy::Normal };
    Optional<JsonValue> m_timeouts_configuration;
    bool m_strict_file_interactability { false };

    RefPtr<Core::Promise<Empty, Web::WebDriver::Error>> m_close_promise;
    HashMap<String, Function<void()>> m_window_handle_became_available_callbacks;
};

}
