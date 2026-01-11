/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/ProcessMonitor.h>
#if !defined(AK_OS_WINDOWS)
#    include <LibCore/System.h>
#endif

namespace WebView {

ProcessMonitor::ProcessMonitor(Function<void(pid_t)> exit_handler)
    : m_on_process_exit(move(exit_handler))
{
#if !defined(AK_OS_WINDOWS)
    m_signal_handle = Core::EventLoop::register_signal(SIGCHLD, [this](int) {
        auto result = Core::System::waitpid(-1, WNOHANG);
        while (!result.is_error() && result.value().pid > 0) {
            auto& [pid, status] = result.value();
            if (m_monitored_processes.contains(pid)) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    m_monitored_processes.remove(pid);
                    m_on_process_exit(pid);
                }
            }
            result = Core::System::waitpid(-1, WNOHANG);
        }
    });
#endif
}

ProcessMonitor::~ProcessMonitor()
{
#if defined(AK_OS_WINDOWS)
    for (pid_t pid : m_monitored_processes) {
        Core::EventLoop::unregister_process(pid);
    }
#else
    Core::EventLoop::unregister_signal(m_signal_handle);
#endif
}

void ProcessMonitor::add_process(pid_t pid)
{
    m_monitored_processes.set(pid, AK::HashSetExistingEntryBehavior::Keep);
#if defined(AK_OS_WINDOWS)
    Core::EventLoop::register_process(pid, [this](pid_t pid) {
        m_on_process_exit(pid);
    });
#endif
}

}
