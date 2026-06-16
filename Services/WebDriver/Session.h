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
#include <AK/Queue.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Vector.h>
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
    using WebDriverPromise = Core::Promise<JsonValue, Web::WebDriver::Error>;

    struct NewSession {
        NonnullRefPtr<Session> session;
        JsonValue capabilities;
    };
    using NewSessionPromise = Core::Promise<NewSession, Web::WebDriver::Error>;

    static ErrorOr<NonnullRefPtr<NewSessionPromise>> create(NonnullRefPtr<Client> client, JsonValue capabilities, Web::WebDriver::SessionFlags flags);
    ~Session();

    enum class AllowInvalidWindowHandle {
        No,
        Yes,
    };
    static ErrorOr<NonnullRefPtr<Session>, Web::WebDriver::Error> find_session(StringView session_id, Web::WebDriver::SessionFlags = Web::WebDriver::SessionFlags::Default, AllowInvalidWindowHandle = AllowInvalidWindowHandle::No);
    static size_t session_count(Web::WebDriver::SessionFlags);
    static bool has_pending_http_session_creation();
    static void close_all();

    NonnullRefPtr<WebDriverPromise> enqueue_http_request(Function<NonnullRefPtr<WebDriverPromise>()>);

    struct Window {
        String handle;
    };

    void close();

    String session_id() const { return m_session_id; }
    Web::WebDriver::SessionFlags session_flags() const { return m_session_flags; }
    String const& current_window_handle() const { return m_current_window_handle; }
    bool test_hooks_enabled() const { return m_options.enable_test_hooks; }

    bool has_window_handle(StringView handle) const { return m_windows.contains(handle); }
    using WindowHandleBecameAvailableCallbackID = u64;
    // Registrations are dispatched at most once. Callers must remove them when handling a timeout.
    WindowHandleBecameAvailableCallbackID add_window_handle_became_available_callback(String const& handle, Function<void()> callback, Function<void()> on_session_close);
    void remove_window_handle_became_available_callback(String const& handle, WindowHandleBecameAvailableCallbackID);

    Web::WebDriver::Response get_timeouts() const;
    Web::WebDriver::Response set_timeouts(JsonValue);
    NonnullRefPtr<WebDriverPromise> close_window();
    NonnullRefPtr<WebDriverPromise> switch_to_window(StringView);
    Web::WebDriver::Response get_window_handles() const;

    enum class HandleUserPrompts {
        No,
        Yes,
    };
    NonnullRefPtr<WebDriverPromise> navigate_to(URL::URL);
    NonnullRefPtr<WebDriverPromise> refresh();
    NonnullRefPtr<WebDriverPromise> wait_for_navigation_completion();
    NonnullRefPtr<WebDriverPromise> traverse_history(i32 delta, HandleUserPrompts);
    NonnullRefPtr<WebDriverPromise> session_history();
    NonnullRefPtr<WebDriverPromise> load_url(URL::URL);
    NonnullRefPtr<WebDriverPromise> run_content_command(StringView name, JsonValue payload = {}, Vector<String> arguments = {});
    ErrorOr<void, Web::WebDriver::Error> ensure_current_window_handle_is_valid() const;

private:
    Session(NonnullRefPtr<Client> client, JsonObject const& capabilities, String session_id, Web::WebDriver::SessionFlags flags);

    using ServerPromise = Core::Promise<Empty>;

    ErrorOr<NonnullRefPtr<ServerPromise>> start(LaunchBrowserCallback const&);
    ErrorOr<void> accept_browser_transport(NonnullOwnPtr<IPC::Transport>);
    NonnullRefPtr<WebDriverPromise> perform_browser_command(Function<void(u64 command_id)> send_command);
    Optional<u64> page_load_timeout() const;
    NonnullRefPtr<WebDriverPromise> reset_current_browsing_context();
    ErrorOr<void> create_server();
    void remove_window(StringView window_handle);
    void dispatch_window_handle_became_available_callbacks(String const& window_handle);
    void reject_pending_browser_commands();
    void arm_browser_startup_timeout(NonnullRefPtr<ServerPromise>);
    void cancel_browser_startup_timeout();
    void reject_start_promise(AK::Error);
    void process_next_http_request();
    void dequeue_current_http_request();

    NonnullRefPtr<Client> m_client;
    Web::WebDriver::LadybirdOptions m_options;

    String m_session_id;
    Web::WebDriver::SessionFlags m_session_flags { Web::WebDriver::SessionFlags::Default };

    HashMap<String, Window> m_windows;
    String m_current_window_handle;

    RefPtr<BrowserConnection> m_browser_connection;
    bool m_closing { false };

    u64 m_next_browser_command_id { 1 };
    HashMap<u64, NonnullRefPtr<WebDriverPromise>> m_pending_browser_commands;
    RefPtr<ServerPromise> m_start_promise;
    RefPtr<Core::Timer> m_start_timer;

    struct PendingHttpRequest {
        Function<NonnullRefPtr<WebDriverPromise>()> handler;
        NonnullRefPtr<WebDriverPromise> promise;
    };

    // https://w3c.github.io/webdriver/#dfn-request-queue
    // An HTTP session has an associated request queue which is a queue of requests that are currently awaiting
    // processing.
    Queue<PendingHttpRequest> m_http_request_queue;

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

    struct WindowHandleBecameAvailableCallback {
        WindowHandleBecameAvailableCallbackID id;
        Function<void()> callback;
        Function<void()> on_session_close;
    };
    HashMap<String, Vector<WindowHandleBecameAvailableCallback>> m_window_handle_became_available_callbacks;
    WindowHandleBecameAvailableCallbackID m_next_window_handle_became_available_callback_id { 1 };
};

NonnullRefPtr<Session::WebDriverPromise> continue_with_promise(NonnullRefPtr<Session::WebDriverPromise>, Function<NonnullRefPtr<Session::WebDriverPromise>()>);

}
