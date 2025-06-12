/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/Types.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibThreading/Mutex.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

ProcessType process_type_from_name(StringView);
StringView process_name_from_type(ProcessType type);

class ProcessManager {
    AK_MAKE_NONCOPYABLE(ProcessManager);

public:
    ProcessManager();
    ~ProcessManager();

    void add_process(Process&&);
    Optional<Process> remove_process(pid_t);
    Optional<Process&> find_process(pid_t);

#if defined(AK_OS_MACH)
    void set_process_mach_port(pid_t, Core::MachPort&&);
#endif

    void update_all_process_statistics();
    JsonValue serialize_json();

    Function<void(Process&&)> on_process_exited;

private:
    Core::Platform::ProcessStatistics m_statistics;
    HashMap<pid_t, Process> m_processes;
    int m_signal_handle { -1 };
    Threading::Mutex m_lock;
};

}
