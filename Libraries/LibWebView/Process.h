/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibCore/Process.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Transport.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

class Process {
    AK_MAKE_NONCOPYABLE(Process);
    AK_MAKE_DEFAULT_MOVABLE(Process);

public:
    Process(ProcessType type, RefPtr<IPC::ConnectionBase> connection, Core::Process process);
    ~Process();

    template<typename ClientType>
    struct ProcessAndClient;

    template<typename ClientType, typename... ClientArguments>
    static ErrorOr<ProcessAndClient<ClientType>> spawn(ProcessType type, Core::ProcessSpawnOptions const& options, ClientArguments&&... client_arguments);

    ProcessType type() const { return m_type; }
    Optional<String> const& title() const { return m_title; }
    void set_title(Optional<String> title) { m_title = move(title); }

    template<typename ConnectionFromClient>
    Optional<ConnectionFromClient&> client()
    {
        if (auto strong_connection = m_connection.strong_ref())
            return as<ConnectionFromClient>(*strong_connection);
        return {};
    }

    pid_t pid() const { return m_process.pid(); }

    struct ProcessPaths {
        ByteString socket_path;
        ByteString pid_path;
    };
    static ErrorOr<ProcessPaths> paths_for_process(StringView process_name);
    static ErrorOr<Optional<pid_t>> get_process_pid(StringView process_name, StringView pid_path);
    static ErrorOr<int> create_ipc_socket(ByteString const& socket_path);

private:
    struct ProcessAndIPCTransport {
        Core::Process process;
        IPC::Transport transport;
    };
    static ErrorOr<ProcessAndIPCTransport> spawn_and_connect_to_process(Core::ProcessSpawnOptions const& options);

    Core::Process m_process;
    ProcessType m_type;
    Optional<String> m_title;
    WeakPtr<IPC::ConnectionBase> m_connection;
};

template<typename ClientType>
struct Process::ProcessAndClient {
    Process process;
    NonnullRefPtr<ClientType> client;
};

template<typename ClientType, typename... ClientArguments>
ErrorOr<Process::ProcessAndClient<ClientType>> Process::spawn(ProcessType type, Core::ProcessSpawnOptions const& options, ClientArguments&&... client_arguments)
{
    auto [core_process, transport] = TRY(spawn_and_connect_to_process(options));
    auto client = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ClientType { move(transport), forward<ClientArguments>(client_arguments)... }));

    return ProcessAndClient<ClientType> { Process { type, client, move(core_process) }, client };
}

}
