/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#if !defined(AK_OS_MACOS)
#    include <LibCore/LocalServer.h>
#else
#    include <LibIPC/MachBootstrapListener.h>
#    include <LibIPC/TransportBootstrapMach.h>
#endif
#include <LibCore/Process.h>
#include <LibCore/Promise.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Response.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <WebDriver/BrowserConnection.h>
#include <WebDriver/Client.h>

namespace WebDriver {

class Session : public RefCounted<Session> {
public:
    static ErrorOr<NonnullRefPtr<Session>> create(NonnullRefPtr<Client> client, JsonObject& capabilities, Web::WebDriver::SessionFlags flags);
    ~Session();

    enum class AllowInvalidWindowHandle {
        No,
        Yes,
    };
    static ErrorOr<NonnullRefPtr<Session>, Web::WebDriver::Error> find_session(StringView session_id, Web::WebDriver::SessionFlags = Web::WebDriver::SessionFlags::Default, AllowInvalidWindowHandle = AllowInvalidWindowHandle::No);
    static size_t session_count(Web::WebDriver::SessionFlags);
    static void close_all();

    struct Window {
        String handle;
    };

    void close();

    String session_id() const { return m_session_id; }
    Web::WebDriver::SessionFlags session_flags() const { return m_session_flags; }
    String const& current_window_handle() const { return m_current_window_handle; }
    bool test_hooks_enabled() const { return m_options.enable_test_hooks; }

    bool has_window_handle(StringView handle) const { return m_windows.contains(handle); }

    Web::WebDriver::Response get_timeouts() const;
    Web::WebDriver::Response set_timeouts(JsonValue);
    Web::WebDriver::Response close_window();
    Web::WebDriver::Response switch_to_window(StringView);
    Web::WebDriver::Response get_window_handles() const;

    enum class HandleUserPrompts {
        No,
        Yes,
    };
    Web::WebDriver::Response navigate_to(URL::URL);
    Web::WebDriver::Response refresh();
    Web::WebDriver::Response wait_for_navigation_completion();
    Web::WebDriver::Response traverse_history(i32 delta, HandleUserPrompts);
    Web::WebDriver::Response session_history();
    Web::WebDriver::Response load_url(URL::URL);
    Web::WebDriver::Response run_content_command(StringView name, JsonValue payload = {}, Vector<String> arguments = {});
    ErrorOr<void, Web::WebDriver::Error> ensure_current_window_handle_is_valid() const;

private:
    Session(NonnullRefPtr<Client> client, JsonObject const& capabilities, String session_id, Web::WebDriver::SessionFlags flags);

    using ServerPromise = Core::Promise<ErrorOr<void>>;

    ErrorOr<void> start(LaunchBrowserCallback const&);
    ErrorOr<void> accept_browser_transport(NonnullOwnPtr<IPC::Transport>, NonnullRefPtr<ServerPromise> promise);
    Web::WebDriver::Response perform_browser_command(Function<void(u64 command_id)> send_command);
    Optional<u64> page_load_timeout() const;
    void reset_current_browsing_context();
    ErrorOr<void> create_server(NonnullRefPtr<ServerPromise> promise);
    void remove_window(StringView window_handle);

    NonnullRefPtr<Client> m_client;
    Web::WebDriver::LadybirdOptions m_options;

    String m_session_id;
    Web::WebDriver::SessionFlags m_session_flags { Web::WebDriver::SessionFlags::Default };

    HashMap<String, Window> m_windows;
    String m_current_window_handle;

    RefPtr<BrowserConnection> m_browser_connection;
    bool m_closing { false };

    u64 m_next_browser_command_id { 1 };
    HashMap<u64, Function<void(Web::WebDriver::Response)>> m_pending_browser_commands;

    ByteString m_browser_endpoint;
    Optional<Core::Process> m_browser_process;
    Core::EventLoop& m_event_loop;

#if defined(AK_OS_MACOS)
    OwnPtr<IPC::MachBootstrapListener> m_browser_mach_port_server;
    IPC::TransportBootstrapMachServer m_transport_bootstrap_server;
#else
    RefPtr<Core::LocalServer> m_browser_server;
#endif

    Web::WebDriver::PageLoadStrategy m_page_load_strategy { Web::WebDriver::PageLoadStrategy::Normal };
    Web::WebDriver::TimeoutsConfiguration m_timeouts;
    Optional<JsonValue> m_timeouts_configuration;
    bool m_strict_file_interactiblity { false };
};

}
