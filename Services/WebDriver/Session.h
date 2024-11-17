/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
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
#include <WebDriver/WebContentConnection.h>
#include <unistd.h>

namespace WebDriver {

struct LaunchBrowserCallbacks;

class Session : public RefCounted<Session> {
public:
    Session(unsigned session_id, NonnullRefPtr<Client> client, Web::WebDriver::LadybirdOptions options);
    ~Session();

    void initialize_from_capabilities(JsonObject&);

    unsigned session_id() const { return m_id; }

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

    String const& current_window_handle() const
    {
        return m_current_window_handle;
    }

    bool has_window_handle(StringView handle) const { return m_windows.contains(handle); }

    ErrorOr<void> start(LaunchBrowserCallbacks const&);

    Web::WebDriver::Response set_timeouts(JsonValue);
    Web::WebDriver::Response close_window();
    Web::WebDriver::Response switch_to_window(StringView);
    Web::WebDriver::Response get_window_handles() const;
    ErrorOr<void, Web::WebDriver::Error> ensure_current_window_handle_is_valid() const;

    template<typename Action>
    Web::WebDriver::Response perform_async_action(Action&& action)
    {
        Optional<Web::WebDriver::Response> response;
        auto& connection = web_content_connection();

        ScopeGuard guard { [&]() { connection.on_driver_execution_complete = nullptr; } };
        connection.on_driver_execution_complete = [&](auto result) { response = move(result); };

        TRY(action(connection));

        Core::EventLoop::current().spin_until([&]() {
            return response.has_value();
        });

        return response.release_value();
    }

private:
    using ServerPromise = Core::Promise<ErrorOr<void>>;
    ErrorOr<NonnullRefPtr<Core::LocalServer>> create_server(NonnullRefPtr<ServerPromise> promise);

    NonnullRefPtr<Client> m_client;
    Web::WebDriver::LadybirdOptions m_options;

    bool m_started { false };
    unsigned m_id { 0 };

    HashMap<String, Window> m_windows;
    String m_current_window_handle;

    Optional<ByteString> m_web_content_socket_path;
    Optional<Core::Process> m_browser_process;

    RefPtr<Core::LocalServer> m_web_content_server;

    Web::WebDriver::PageLoadStrategy m_page_load_strategy { Web::WebDriver::PageLoadStrategy::Normal };
    Web::WebDriver::UnhandledPromptBehavior m_unhandled_prompt_behavior { Web::WebDriver::UnhandledPromptBehavior::DismissAndNotify };
    Optional<JsonValue> m_timeouts_configuration;
    bool m_strict_file_interactiblity { false };
};

}
