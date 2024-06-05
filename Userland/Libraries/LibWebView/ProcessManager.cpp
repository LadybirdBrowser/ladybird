/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumberFormat.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibWebView/ProcessManager.h>

namespace WebView {

static sig_atomic_t s_received_sigchld = 0;

ProcessType process_type_from_name(StringView name)
{
    if (name == "Chrome"sv)
        return ProcessType::Chrome;
    if (name == "WebContent"sv)
        return ProcessType::WebContent;
    if (name == "WebWorker"sv)
        return ProcessType::WebWorker;
    if (name == "SQLServer"sv)
        return ProcessType::SQLServer;
    if (name == "RequestServer"sv)
        return ProcessType::RequestServer;
    if (name == "ImageDecoder"sv)
        return ProcessType::ImageDecoder;

    dbgln("Unknown process type: '{}'", name);
    VERIFY_NOT_REACHED();
}

StringView process_name_from_type(ProcessType type)
{
    switch (type) {
    case ProcessType::Chrome:
        return "Chrome"sv;
    case ProcessType::WebContent:
        return "WebContent"sv;
    case ProcessType::WebWorker:
        return "WebWorker"sv;
    case ProcessType::SQLServer:
        return "SQLServer"sv;
    case ProcessType::RequestServer:
        return "RequestServer"sv;
    case ProcessType::ImageDecoder:
        return "ImageDecoder"sv;
    }
    VERIFY_NOT_REACHED();
}

ProcessManager::ProcessManager()
{
}

ProcessManager::~ProcessManager()
{
}

ProcessManager& ProcessManager::the()
{
    static ProcessManager s_the;
    return s_the;
}

void ProcessManager::initialize()
{
    // FIXME: Should we change this to call EventLoop::register_signal?
    //        Note that only EventLoopImplementationUnix has a working register_signal

    struct sigaction action { };
    action.sa_flags = SA_RESTART;
    action.sa_sigaction = [](int, auto*, auto) {
        s_received_sigchld = 1;
    };

    MUST(Core::System::sigaction(SIGCHLD, &action, nullptr));

    the().add_process(WebView::ProcessType::Chrome, getpid());
#ifdef AK_OS_MACH
    auto self_send_port = mach_task_self();
    auto res = mach_port_mod_refs(mach_task_self(), self_send_port, MACH_PORT_RIGHT_SEND, +1);
    VERIFY(res == KERN_SUCCESS);
    the().add_process(getpid(), Core::MachPort::adopt_right(self_send_port, Core::MachPort::PortRight::Send));
#endif
}

ProcessInfo* ProcessManager::find_process(pid_t pid)
{
    if (auto existing_process = m_statistics.processes.find_if([&](auto& info) { return info->pid == pid; }); !existing_process.is_end())
        return verify_cast<ProcessInfo>(existing_process->ptr());

    return nullptr;
}

void ProcessManager::add_process(ProcessType type, pid_t pid)
{
    Threading::MutexLocker locker { m_lock };
    if (auto* existing_process = find_process(pid)) {
        existing_process->type = type;
        return;
    }
    m_statistics.processes.append(make<ProcessInfo>(type, pid));
}

#if defined(AK_OS_MACH)
void ProcessManager::add_process(pid_t pid, Core::MachPort&& port)
{
    Threading::MutexLocker locker { m_lock };
    if (auto* existing_process = find_process(pid)) {
        existing_process->child_task_port = move(port);
        return;
    }
    m_statistics.processes.append(make<ProcessInfo>(pid, move(port)));
}
#endif

void ProcessManager::remove_process(pid_t pid)
{
    Threading::MutexLocker locker { m_lock };
    m_statistics.processes.remove_first_matching([&](auto const& info) {
        if (info->pid == pid) {
            return true;
        }
        return false;
    });
}

void ProcessManager::update_all_processes()
{
    if (s_received_sigchld) {
        s_received_sigchld = 0;
        auto result = Core::System::waitpid(-1, WNOHANG);
        while (!result.is_error() && result.value().pid > 0) {
            auto& [pid, status] = result.value();
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                remove_process(pid);
            }
            result = Core::System::waitpid(-1, WNOHANG);
        }
    }

    Threading::MutexLocker locker { m_lock };
    (void)update_process_statistics(m_statistics);
}

String ProcessManager::generate_html()
{
    Threading::MutexLocker locker { m_lock };
    StringBuilder builder;

    builder.append(R"(
        <html>
        <head>
        <title>Task Manager</title>
        <style>
                @media (prefers-color-scheme: dark) {
                    /* FIXME: We should be able to remove the HTML style when "color-scheme" is supported */
                    html {
                        background-color: rgb(30, 30, 30);
                        color: white;
                    }

                    tr:nth-child(even) {
                        background: rgb(57, 57, 57);
                    }
                }

                @media (prefers-color-scheme: light) {
                    tr:nth-child(even) {
                        background: #f7f7f7;
                    }
                }

                table {
                    width: 100%;
                    border-collapse: collapse;
                }
                th {
                    text-align: left;
                    border-bottom: 1px solid #aaa;
                }
                td, th {
                    padding: 4px;
                    border: 1px solid #aaa;
                }
        </style>
        </head>
        <body>
        <table>
                <thead>
                <tr>
                        <th>Name</th>
                        <th>PID</th>
                        <th>Memory Usage</th>
                        <th>CPU %</th>
                </tr>
                </thead>
                <tbody>
    )"sv);

    m_statistics.for_each_process<ProcessInfo>([&](auto const& process) {
        builder.append("<tr>"sv);
        builder.append("<td>"sv);
        builder.append(WebView::process_name_from_type(process.type));
        if (process.title.has_value())
            builder.appendff(" - {}", escape_html_entities(*process.title));
        builder.append("</td>"sv);
        builder.append("<td>"sv);
        builder.append(MUST(String::number(process.pid)));
        builder.append("</td>"sv);
        builder.append("<td>"sv);
        builder.append(human_readable_size(process.memory_usage_bytes));
        builder.append("</td>"sv);
        builder.append("<td>"sv);
        builder.append(MUST(String::formatted("{:.1f}", process.cpu_percent)));
        builder.append("</td>"sv);
        builder.append("</tr>"sv);
    });

    builder.append(R"(
                </tbody>
                </table>
                </body>
                </html>
    )"sv);

    return builder.to_string_without_validation();
}

}
