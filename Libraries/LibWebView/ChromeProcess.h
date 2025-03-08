/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Forward.h>
#include <LibIPC/MultiServer.h>
#include <LibWebView/Options.h>
#include <LibWebView/UIProcessClientEndpoint.h>
#include <LibWebView/UIProcessServerEndpoint.h>

namespace WebView {

class UIProcessConnectionFromClient final
    : public IPC::ConnectionFromClient<UIProcessClientEndpoint, UIProcessServerEndpoint> {
    C_OBJECT(UIProcessConnectionFromClient);

public:
    virtual ~UIProcessConnectionFromClient() override = default;

    virtual void die() override;

    Function<void(Vector<URL::URL> const&)> on_new_tab;
    Function<void(Vector<URL::URL> const&)> on_new_window;

private:
    UIProcessConnectionFromClient(IPC::Transport, int client_id);

    virtual void create_new_tab(Vector<ByteString> urls) override;
    virtual void create_new_window(Vector<ByteString> urls) override;
};

class ChromeProcess {
    AK_MAKE_NONCOPYABLE(ChromeProcess);
    AK_MAKE_DEFAULT_MOVABLE(ChromeProcess);

public:
    enum class ProcessDisposition : u8 {
        ContinueMainProcess,
        ExitProcess,
    };

    ChromeProcess() = default;
    ~ChromeProcess();

    ErrorOr<ProcessDisposition> connect(Vector<ByteString> const& raw_urls, NewWindow new_window);

    Function<void(Vector<URL::URL> const&)> on_new_tab;
    Function<void(Vector<URL::URL> const&)> on_new_window;

private:
    ErrorOr<void> connect_as_client(ByteString const& socket_path, Vector<ByteString> const& raw_urls, NewWindow new_window);
    ErrorOr<void> connect_as_server(ByteString const& socket_path);

    OwnPtr<IPC::MultiServer<UIProcessConnectionFromClient>> m_server_connection;
    OwnPtr<Core::File> m_pid_file;
    ByteString m_pid_path;
    ByteString m_socket_path;
};

}
