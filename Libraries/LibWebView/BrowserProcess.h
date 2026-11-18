/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/LocalServer.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Options.h>
#include <LibWebView/UIProcessClientEndpoint.h>
#include <LibWebView/UIProcessServerEndpoint.h>

namespace WebView {

class WEBVIEW_API UIProcessConnectionFromClient final
    : public IPC::ConnectionFromClient<UIProcessClientEndpoint, UIProcessServerEndpoint> {
    C_OBJECT(UIProcessConnectionFromClient);

public:
    virtual ~UIProcessConnectionFromClient() override = default;

    virtual void die() override;

    Function<void(Vector<URL::URL> const&)> on_new_tab;
    Function<void(Vector<URL::URL> const&)> on_new_window;

private:
    UIProcessConnectionFromClient(NonnullOwnPtr<IPC::Transport>, int client_id);

    virtual void create_new_tab(Vector<ByteString> urls) override;
    virtual void create_new_window(Vector<ByteString> urls) override;
};

class WEBVIEW_API BrowserProcess {
    AK_MAKE_NONCOPYABLE(BrowserProcess);
    AK_MAKE_DEFAULT_MOVABLE(BrowserProcess);

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class ProcessDisposition : u8 {
        ContinueMainProcess,
        ExitProcess,
        // A browser is running for the profile, but it does not take what this process was asked to open.
        BrowserIsUnresponsive,
    };

    // How handing what this process was asked to open to the browser that is already running went.
    enum class Handover : u8 {
        Done,
        BrowserIsGone,
        BrowserIsUnresponsive,
    };

    // What a process ends with when it found ProcessDisposition::BrowserIsUnresponsive.
    static constexpr int unresponsive_browser_exit_status = 86;

    BrowserProcess() = default;
    ~BrowserProcess();

    // NB: ui_pid is the process that shows this browser to the user, if that is not this process. A browser whose UI
    //     is gone cannot be seen or closed by the user, so a later launch ends it without asking if it is unresponsive.
    //     An unresponsive browser that started before replace_browser_from_before is ended as well. That is the time
    //     at which the user was asked about a browser, so one that started after it is not the one they agreed to end.
    ErrorOr<ProcessDisposition> connect(Vector<ByteString> const& raw_urls, NewWindow new_window, StringView runtime_directory, StringView profile_identity, Optional<pid_t> ui_pid, Optional<UnixDateTime> replace_browser_from_before);

    Function<void(Vector<URL::URL> const&)> on_new_tab;
    Function<void(Vector<URL::URL> const&)> on_new_window;

private:
    void accept_transport(NonnullOwnPtr<IPC::Transport>);
    struct HandoverResult {
        Handover handover { Handover::BrowserIsGone };
        // The process that was found to be the browser, which is not taken from the PID file alone.
        Optional<pid_t> browser_pid {};
    };
#if defined(AK_OS_MACOS)
    ErrorOr<HandoverResult> connect_as_client(pid_t pid, Vector<ByteString> const& raw_urls, NewWindow new_window, StringView profile_identity, AK::Duration handover_timeout);
    ErrorOr<void> connect_as_server();
#else
    ErrorOr<HandoverResult> connect_as_client(ByteString const& socket_path, Vector<ByteString> const& raw_urls, NewWindow new_window, AK::Duration handover_timeout);
    ErrorOr<void> connect_as_server(ByteString const& socket_path);
#endif

    RefPtr<Core::LocalServer> m_local_server;
    OwnPtr<Core::File> m_pid_file;
    ByteString m_startup_lock_path;
    ByteString m_pid_path;
    ByteString m_ui_pid_path;
    ByteString m_socket_path;
    int m_next_client_id { 0 };
};

}
