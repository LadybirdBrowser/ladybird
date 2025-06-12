/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
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
    // FIXME: Handle exiting child processes on Windows
#ifndef AK_OS_WINDOWS
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
#endif

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
    // FIXME: Handle exiting child processes on Windows
#ifndef AK_OS_WINDOWS
    Core::EventLoop::unregister_signal(m_signal_handle);
#endif
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

JsonValue ProcessManager::serialize_json()
{
    Threading::MutexLocker locker { m_lock };
    JsonArray serialized;

    m_statistics.for_each_process([&](auto const& process) {
        auto& process_handle = find_process(process.pid).value();

        auto type = WebView::process_name_from_type(process_handle.type());
        auto const& title = process_handle.title();

        auto process_name = title.has_value()
            ? MUST(String::formatted("{} - {}", type, *title))
            : String::from_utf8_without_validation(type.bytes());

        JsonObject object;
        object.set("name"sv, move(process_name));
        object.set("pid"sv, process.pid);
        object.set("cpu"sv, process.cpu_percent);
        object.set("memory"sv, process.memory_usage_bytes);
        serialized.must_append(move(object));
    });

    return serialized;
}

}
