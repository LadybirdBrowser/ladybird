/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/WeakPtr.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Transport.h>
#include <LibSandbox/ConnectBroker.h>
#include <LibURL/URL.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

struct ProcessOutputCapture {
    OwnPtr<Core::File> stdout_file;
    OwnPtr<Core::File> stderr_file;
};

class WEBVIEW_API Process {
    AK_MAKE_NONCOPYABLE(Process);
    AK_MAKE_DEFAULT_MOVABLE(Process);

public:
    Process(ProcessType type, RefPtr<IPC::ConnectionBase> connection, Core::Process process);
    static Process for_browser();
    ~Process();

    template<typename ClientType>
    struct ProcessAndClient;

    template<typename ClientType, typename... ClientArguments>
    static ErrorOr<ProcessAndClient<ClientType>> spawn(ProcessType type, Core::ProcessSpawnOptions const& options, bool capture_output, ClientArguments&&... client_arguments);

    struct ProcessAndTransport;

    // For a helper that serves another process: The caller hands the transport over instead of connecting to it.
    static ErrorOr<ProcessAndTransport> spawn_with_transport(ProcessType type, Core::ProcessSpawnOptions const& options, bool capture_output);

    ProcessType type() const { return m_type; }

    Optional<Utf16String> const& title() const { return m_title; }
    void set_title(Optional<Utf16String> title) { m_title = move(title); }

    Optional<URL::URL> const& site_url() const { return m_site_url; }
    void set_site_url(Optional<URL::URL> site_url) { m_site_url = move(site_url); }

    Optional<u64> owner_child_id() const { return m_owner_child_id; }
    void set_owner_child_id(u64 child_id) { m_owner_child_id = child_id; }

    template<typename ConnectionFromClient>
    RefPtr<ConnectionFromClient> client()
    {
        if (auto strong_connection = m_connection.strong_ref())
            return as<ConnectionFromClient>(*strong_connection);
        return {};
    }

    void set_crash_report(NonnullOwnPtr<CrashReport> report) { m_crash_report = move(report); }

#if defined(AK_OS_LINUX)
    // The broker serves this process alone, so it lives exactly as long as the process does.
    void set_connect_broker(NonnullOwnPtr<Sandbox::ConnectBroker> broker) { m_connect_broker = move(broker); }
#endif
    void save_crash_report(Optional<int> exit_status);
    ByteString const& saved_crash_report_name() const { return m_saved_crash_report_name; }

    pid_t pid() const { return m_pid; }
    u64 child_id() const { return m_child_id; }
    void did_register(Badge<ProcessManager>, u64 child_id);

    ProcessOutputCapture& output_capture() { return m_output_capture; }
    ProcessOutputCapture const& output_capture() const { return m_output_capture; }

    struct ProcessPaths {
        ByteString socket_path;
        ByteString pid_path;
    };
    static ErrorOr<ProcessPaths> paths_for_process(StringView process_name, StringView runtime_directory);

    // Helper processes receive only debugging, sanitizer, locale and basic environment variables; others may hold
    // secrets.
    static Vector<ByteString> helper_process_environment(ProcessType);
    static ErrorOr<Optional<pid_t>> get_process_pid(StringView process_name, StringView pid_path);
    static ErrorOr<int> create_ipc_socket(ByteString const& socket_path);

private:
    struct ProcessAndIPCTransport {
        Core::Process process;
        NonnullOwnPtr<IPC::Transport> transport;
        ProcessOutputCapture output_capture;
    };
    static ErrorOr<ProcessAndIPCTransport> spawn_and_connect_to_process(ProcessType, Core::ProcessSpawnOptions const& options, bool capture_output);

    OwnPtr<CrashReport> m_crash_report;
    ByteString m_saved_crash_report_name;

#if defined(AK_OS_LINUX)

    OwnPtr<Sandbox::ConnectBroker> m_connect_broker;
#endif
    Process(ProcessType, pid_t);

    pid_t m_pid { -1 };
    // NB: The browser's own record has no process of its own to hold. On Windows, holding the process keeps its handle,
    //     and with it its PID, until this record is gone.
    Optional<Core::Process> m_process;
    u64 m_child_id { 0 };
    ProcessType m_type;
    Optional<Utf16String> m_title;
    Optional<URL::URL> m_site_url;
    Optional<u64> m_owner_child_id;
    WeakPtr<IPC::ConnectionBase> m_connection;
    ProcessOutputCapture m_output_capture;
};

template<typename ClientType>
struct Process::ProcessAndClient {
    Process process;
    NonnullRefPtr<ClientType> client;
};

struct Process::ProcessAndTransport {
    Process process;
    NonnullOwnPtr<IPC::Transport> transport;
};

template<typename ClientType, typename... ClientArguments>
ErrorOr<Process::ProcessAndClient<ClientType>> Process::spawn(ProcessType type, Core::ProcessSpawnOptions const& options, bool capture_output, ClientArguments&&... client_arguments)
{
    auto [core_process, transport, output_capture] = TRY(spawn_and_connect_to_process(type, options, capture_output));

    // NB: Create the record first, so that a failure below ends the helper rather than leaving it running untracked.
    Process process { type, nullptr, move(core_process) };
    process.m_output_capture = move(output_capture);
    auto client = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ClientType { move(transport), forward<ClientArguments>(client_arguments)... }));
    process.m_connection = *client;
    return ProcessAndClient<ClientType> { move(process), client };
}

}
