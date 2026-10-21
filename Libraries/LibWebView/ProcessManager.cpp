/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibIPC/Connection.h>
#include <LibWebView/ProcessManager.h>

namespace WebView {

ProcessType process_type_from_name(StringView name)
{
    if (name == "Browser"sv)
        return ProcessType::Browser;
    if (name == "Compositor"sv)
        return ProcessType::Compositor;
    if (name == "WebContent"sv)
        return ProcessType::WebContent;
    if (name == "WebWorker"sv)
        return ProcessType::WebWorker;
    if (name == "RequestServer"sv)
        return ProcessType::RequestServer;
    if (name == "ImageDecoder"sv)
        return ProcessType::ImageDecoder;
    if (name == "MediaServer"sv)
        return ProcessType::MediaServer;
    if (name == "AudioServer"sv)
        return ProcessType::AudioServer;
    if (name == "VideoDecoderServer"sv)
        return ProcessType::VideoDecoderServer;

    dbgln("Unknown process type: '{}'", name);
    VERIFY_NOT_REACHED();
}

StringView process_name_from_type(ProcessType type)
{
    switch (type) {
    case ProcessType::Browser:
        return "Browser"sv;
    case ProcessType::Compositor:
        return "Compositor"sv;
    case ProcessType::WebContent:
        return "WebContent"sv;
    case ProcessType::WebWorker:
        return "WebWorker"sv;
    case ProcessType::RequestServer:
        return "RequestServer"sv;
    case ProcessType::ImageDecoder:
        return "ImageDecoder"sv;
    case ProcessType::MediaServer:
        return "MediaServer"sv;
    case ProcessType::AudioServer:
        return "AudioServer"sv;
    case ProcessType::VideoDecoderServer:
        return "VideoDecoderServer"sv;
    }
    VERIFY_NOT_REACHED();
}

ProcessManager::ProcessManager()
    : on_process_added([](Process&) { })
    , on_process_exited([](Process&&, Optional<int>) { })
    , m_process_monitor(ProcessMonitor([this](pid_t pid, Optional<int> exit_status) {
        auto child_id = m_child_ids_by_pid.get(pid);
        if (!child_id.has_value())
            return;
        if (auto process = remove_process(*child_id); process.has_value()) {
            process->save_crash_report(exit_status);
            on_process_exited(process.release_value(), exit_status);
        }
    }))
{
    add_process(Process::for_browser());

#ifdef AK_OS_MACH
    mach_port_t task_name_port = MACH_PORT_NULL;
    auto res = task_get_special_port(mach_task_self(), TASK_NAME_PORT, &task_name_port);
    VERIFY(res == KERN_SUCCESS);
    set_process_task_name_port(getpid(), Core::MachPort::adopt_right(task_name_port, Core::MachPort::PortRight::Send));
#endif
}

Optional<Process&> ProcessManager::find_process(pid_t pid)
{
    verify_event_loop();
    auto child_id = m_child_ids_by_pid.get(pid);
    if (!child_id.has_value())
        return {};
    return m_processes.get(*child_id);
}

Optional<Process&> ProcessManager::find_child(u64 child_id)
{
    verify_event_loop();
    return m_processes.get(child_id);
}

ChildIdentity ProcessManager::add_process(WebView::Process&& process)
{
    verify_event_loop();
    auto pid = process.pid();
    u64 child_id = 0;
    if (process.type() != ProcessType::Browser) {
        VERIFY(m_next_child_id < NumericLimits<u64>::max());
        child_id = m_next_child_id++;
    }
    process.did_register({}, child_id);
    on_process_added(process);
    VERIFY(m_processes.set(child_id, move(process)) == AK::HashSetResult::InsertedNewEntry);
    VERIFY(m_child_ids_by_pid.set(pid, child_id) == AK::HashSetResult::InsertedNewEntry);
    m_statistics.processes.append(make<Core::Platform::ProcessInfo>(pid));
    m_process_monitor.add_process(pid);
    return { child_id, pid };
}

ErrorOr<void> ProcessManager::terminate_child(u64 child_id, Core::Process::TerminationMode mode)
{
    verify_event_loop();
    // NB: Child ID zero is the browser itself.
    auto process = child_id != 0 ? find_child(child_id) : OptionalNone {};
    if (!process.has_value())
        return Error::from_errno(ESRCH);
    // NB: The process is still in the table, so its exit has not been handled yet. On POSIX it has not been reaped,
    //     and on Windows its Process keeps the handle open, so its PID cannot belong to another process yet.
    return Core::Process::terminate_process(process->pid(), mode);
}

void ProcessManager::for_each_process(Function<void(Process&)> callback)
{
    verify_event_loop();
    for (auto& entry : m_processes)
        callback(entry.value);
}

void ProcessManager::for_each_process_statistics(Function<void(Process&, Core::Platform::ProcessInfo const&)> callback)
{
    verify_event_loop();
    m_statistics.for_each_process([&](auto const& process) {
        auto process_handle = find_process(process.pid);
        if (!process_handle.has_value())
            return;

        callback(*process_handle, process);
    });
}

#if defined(AK_OS_MACH)
void ProcessManager::set_process_task_name_port(pid_t pid, Core::MachPort&& port)
{
    verify_event_loop();
    for (auto const& info : m_statistics.processes) {
        if (info->pid == pid) {
            info->task_name_port = move(port);
            return;
        }
    }
}
#endif

Optional<Process> ProcessManager::remove_process(u64 child_id)
{
    verify_event_loop();
    cancel_forced_exit(child_id);
    auto process = m_processes.take(child_id);
    if (!process.has_value())
        return {};
    auto pid = process->pid();
    m_child_ids_by_pid.remove(pid);
    m_statistics.processes.remove_first_matching([&](auto const& info) {
        return (info->pid == pid);
    });
    return process;
}

void ProcessManager::remove_all_processes()
{
    verify_event_loop();

    // NB: Shutting down a connection runs its die() handler, which looks up its process, for example to end a worker
    //     that did not notice the connection closing. Shut the connections down while the table is intact.
    Vector<u64> child_ids;
    child_ids.ensure_capacity(m_processes.size());
    for (auto const& entry : m_processes)
        child_ids.unchecked_append(entry.key);
    for (auto child_id : child_ids) {
        auto process = m_processes.get(child_id);
        if (!process.has_value())
            continue;
        if (auto connection = process->client<IPC::ConnectionBase>())
            connection->shutdown();
    }

    // Destroying a process may run a die() handler too, so take the processes out first. Lookups then see an empty
    // table rather than one being destroyed.
    m_child_ids_by_pid.clear();
    auto processes = move(m_processes);
    processes.clear();
}

void ProcessManager::cancel_forced_exit(u64 child_id)
{
    verify_event_loop();
    if (auto timer = m_forced_exit_timers.take(child_id); timer.has_value())
        (*timer)->stop();
}

void ProcessManager::force_exit_after_timeout(u64 child_id, int timeout_ms)
{
    verify_event_loop();
    if (!m_processes.contains(child_id))
        return;
    if (m_forced_exit_timers.contains(child_id))
        return;

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timeout_ms), [this, child_id] {
        m_forced_exit_timers.remove(child_id);
        auto process = m_processes.get(child_id);
        if (!process.has_value())
            return;

        auto pid = process->pid();
        dbgln("Force-killing unresponsive {} process {}", process_name_from_type(process->type()), pid);
        auto result = terminate_child(child_id, Core::Process::TerminationMode::Forceful);
        if (result.is_error())
            dbgln("Failed to force-kill process {}: {}", pid, result.error());
    });
    timer->start();
    m_forced_exit_timers.set(child_id, move(timer));
}

void ProcessManager::update_all_process_statistics()
{
    verify_event_loop();
    (void)update_process_statistics(m_statistics);
}

void ProcessManager::verify_event_loop() const
{
    if (Core::EventLoop::is_running())
        VERIFY(&Core::EventLoop::current() == m_creation_event_loop);
}

Optional<Core::Platform::ProcessResourceUsage> ProcessManager::resource_usage(pid_t pid) const
{
    verify_event_loop();
    for (auto const& process : m_statistics.processes) {
        if (process->pid == pid)
            return Core::Platform::process_resource_usage(*process);
    }
    return {};
}

}
