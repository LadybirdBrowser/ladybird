/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/ProcessResourceUsage.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibWebView/ChildIdentity.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessMonitor.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

WEBVIEW_API Optional<ProcessType> process_type_from_name(StringView);
WEBVIEW_API StringView process_name_from_type(ProcessType type);

class WEBVIEW_API ProcessManager {
    AK_MAKE_NONCOPYABLE(ProcessManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    ProcessManager();
    ~ProcessManager();

    // Gives the process its child ID and starts watching for its exit.
    ChildIdentity add_process(Process&&);
    void for_each_process(Function<void(Process&)>);
    void for_each_process_statistics(Function<void(Process&, Core::Platform::ProcessInfo const&)>);
    Optional<Process> remove_process(u64 child_id);
    void remove_all_processes();
    Optional<Process&> find_process(pid_t);
    Optional<Process&> find_child(u64 child_id);
    ErrorOr<void> terminate_child(u64 child_id, Core::Process::TerminationMode);
    void cancel_forced_exit(u64 child_id);
    void force_exit_after_timeout(u64 child_id, int timeout_ms);

#if defined(AK_OS_MACH)
    void set_process_task_name_port(pid_t, Core::MachPort&&);
#endif

    void update_all_process_statistics();

    // The memory a WebContent process may use on this machine before it is ended. Zero means that there is no limit.
    static u64 default_web_content_memory_limit();
    // Ends each helper that uses more memory than the limit, so that one runaway process cannot take the machine
    // down. A helper that comes near the limit is reported first, so that it can be asked to release memory. A limit
    // of zero turns the check off.
    void set_web_content_memory_limit(u64 limit_in_bytes);
    Optional<Core::Platform::ProcessResourceUsage> resource_usage(pid_t) const;

    Function<void(Process&)> on_process_added; // test-web
    Function<void(Process&&, Optional<int> exit_status)> on_process_exited;
    // Called when a helper comes near the memory limit, and again from time to time while it stays there.
    Function<void(Process&, u64 memory_usage_in_bytes)> on_process_near_memory_limit;
    // Called when a helper was ended for using more memory than the limit, before its exit is handled.
    Function<void(Process&, u64 memory_usage_in_bytes)> on_process_exceeded_memory_limit;

private:
    void verify_event_loop() const;
    void check_web_content_memory_use();

    Core::Platform::ProcessStatistics m_statistics;
    HashMap<u64, Process> m_processes;
    // NB: Exit notifications and statistics name processes by PID.
    HashMap<pid_t, u64> m_child_ids_by_pid;
    HashMap<u64, RefPtr<Core::Timer>> m_forced_exit_timers;
    u64 m_web_content_memory_limit { 0 };
    RefPtr<Core::Timer> m_web_content_memory_limit_timer;
    // NB: A helper ended for its memory use stays in the table until its exit is handled.
    HashTable<u64> m_children_ended_for_memory_use;
    struct MemoryLimitWarning {
        // The helper is not asked to release memory again before this time.
        MonotonicTime next_time;
        // How long the helper had to wait this time, which is doubled for the next time.
        AK::Duration interval;
    };
    HashMap<u64, MemoryLimitWarning> m_memory_limit_warnings;
    u64 m_next_child_id { 1 };
    ProcessMonitor m_process_monitor;
    Core::EventLoop* m_creation_event_loop { &Core::EventLoop::current() };
};

}
