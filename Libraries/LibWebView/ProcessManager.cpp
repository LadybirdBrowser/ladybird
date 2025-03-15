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

ProcessType process_type_from_name(StringView name)
{
    if (name == "Browser"sv)
        return ProcessType::Browser;
    if (name == "WebContent"sv)
        return ProcessType::WebContent;
    if (name == "WebWorker"sv)
        return ProcessType::WebWorker;
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
    case ProcessType::Browser:
        return "Browser"sv;
    case ProcessType::WebContent:
        return "WebContent"sv;
    case ProcessType::WebWorker:
        return "WebWorker"sv;
    case ProcessType::RequestServer:
        return "RequestServer"sv;
    case ProcessType::ImageDecoder:
        return "ImageDecoder"sv;
    }
    VERIFY_NOT_REACHED();
}

ProcessManager::ProcessManager()
    : on_process_exited([](Process&&) { })
{
    m_signal_handle = Core::EventLoop::register_signal(SIGCHLD, [this](int) {
        auto result = Core::System::waitpid(-1, WNOHANG);
        while (!result.is_error() && result.value().pid > 0) {
            auto& [pid, status] = result.value();
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                if (auto process = remove_process(pid); process.has_value())
                    on_process_exited(process.release_value());
            }
            result = Core::System::waitpid(-1, WNOHANG);
        }
    });

    add_process(Process(WebView::ProcessType::Browser, nullptr, Core::Process::current()));

#ifdef AK_OS_MACH
    auto self_send_port = mach_task_self();
    auto res = mach_port_mod_refs(mach_task_self(), self_send_port, MACH_PORT_RIGHT_SEND, +1);
    VERIFY(res == KERN_SUCCESS);
    set_process_mach_port(getpid(), Core::MachPort::adopt_right(self_send_port, Core::MachPort::PortRight::Send));
#endif
}

ProcessManager::~ProcessManager()
{
    Core::EventLoop::unregister_signal(m_signal_handle);
}

Optional<Process&> ProcessManager::find_process(pid_t pid)
{
    return m_processes.get(pid);
}

void ProcessManager::add_process(WebView::Process&& process)
{
    Threading::MutexLocker locker { m_lock };

    auto pid = process.pid();
    auto result = m_processes.set(pid, move(process));
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    m_statistics.processes.append(make<Core::Platform::ProcessInfo>(pid));
}

#if defined(AK_OS_MACH)
void ProcessManager::set_process_mach_port(pid_t pid, Core::MachPort&& port)
{
    Threading::MutexLocker locker { m_lock };
    for (auto const& info : m_statistics.processes) {
        if (info->pid == pid) {
            info->child_task_port = move(port);
            return;
        }
    }
}
#endif

Optional<Process> ProcessManager::remove_process(pid_t pid)
{
    Threading::MutexLocker locker { m_lock };
    m_statistics.processes.remove_first_matching([&](auto const& info) {
        return (info->pid == pid);
    });
    return m_processes.take(pid);
}

void ProcessManager::update_all_process_statistics()
{
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
                    tr:nth-child(even) {
                        background: rgb(57, 57, 57);
                    }
                }

                @media (prefers-color-scheme: light) {
                    tr:nth-child(even) {
                        background: #f7f7f7;
                    }
                }

                html {
                    color-scheme: light dark;
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

    m_statistics.for_each_process([&](auto const& process) {
        builder.append("<tr>"sv);
        builder.append("<td>"sv);
        auto& process_handle = this->find_process(process.pid).value();
        builder.append(WebView::process_name_from_type(process_handle.type()));
        if (process_handle.title().has_value())
            builder.appendff(" - {}", escape_html_entities(*process_handle.title()));
        builder.append("</td>"sv);
        builder.append("<td>"sv);
        builder.append(String::number(process.pid));
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
