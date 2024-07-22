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
#include <LibCore/Socket.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Forward.h>
#include <LibIPC/MultiServer.h>
#include <LibWebView/UIProcessClientEndpoint.h>
#include <LibWebView/UIProcessServerEndpoint.h>

namespace WebView {

class UIProcessConnectionFromClient final
    : public IPC::ConnectionFromClient<UIProcessClientEndpoint, UIProcessServerEndpoint> {
    C_OBJECT(UIProcessConnectionFromClient);

public:
    virtual ~UIProcessConnectionFromClient() override = default;

    virtual void die() override;

    Function<void(Vector<ByteString> const& urls)> on_new_tab;
    Function<void(Vector<ByteString> const& urls)> on_new_window;

private:
    UIProcessConnectionFromClient(NonnullOwnPtr<Core::LocalSocket>, int client_id);

    virtual void create_new_tab(Vector<ByteString> const& urls) override;
    virtual void create_new_window(Vector<ByteString> const& urls) override;
};

class ChromeProcess {
    AK_MAKE_NONCOPYABLE(ChromeProcess);
    AK_MAKE_DEFAULT_MOVABLE(ChromeProcess);

public:
    enum class ProcessDisposition : u8 {
        ContinueMainProcess,
        ExitProcess,
    };

    static ErrorOr<ChromeProcess> create();
    ~ChromeProcess();

    ErrorOr<ProcessDisposition> connect(Vector<ByteString> const& raw_urls, bool new_window);

    Function<void(Vector<ByteString> const& raw_urls)> on_new_tab;
    Function<void(Vector<ByteString> const& raw_urls)> on_new_window;

private:
    ChromeProcess() = default;

    ErrorOr<void> connect_as_client(ByteString const& socket_path, Vector<ByteString> const& raw_urls, bool new_window);
    ErrorOr<void> connect_as_server(ByteString const& socket_path);

    OwnPtr<IPC::MultiServer<UIProcessConnectionFromClient>> m_server_connection;
    OwnPtr<Core::File> m_pid_file;
    ByteString m_pid_path;
    ByteString m_socket_path;
};

}
