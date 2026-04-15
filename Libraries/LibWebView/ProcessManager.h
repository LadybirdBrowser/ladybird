/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <AK/Types.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessMonitor.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

WEBVIEW_API ProcessType process_type_from_name(StringView);
WEBVIEW_API StringView process_name_from_type(ProcessType type);

class WEBVIEW_API ProcessManager {
    AK_MAKE_NONCOPYABLE(ProcessManager);

public:
    ProcessManager();

    void add_process(Process&&);
    void for_each_process(Function<void(Process&)>);
    Optional<Process> remove_process(pid_t);
    Optional<Process&> find_process(pid_t);

#if defined(AK_OS_MACH)
    void set_process_mach_port(pid_t, Core::MachPort&&);
#endif

    void update_all_process_statistics();
    JsonValue serialize_json();

    Function<void(Process&)> on_process_added; // test-web
    Function<void(Process&&)> on_process_exited;

private:
    void verify_event_loop() const;

    Core::Platform::ProcessStatistics m_statistics;
    HashMap<pid_t, Process> m_processes;
    ProcessMonitor m_process_monitor;
    Core::EventLoop* m_creation_event_loop { &Core::EventLoop::current() };
};

}
