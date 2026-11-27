/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumberFormat.h>
#include <AK/NumericLimits.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibIPC/Connection.h>
#include <LibWebView/ProcessManager.h>

namespace WebView {

Optional<ProcessType> process_type_from_name(StringView name)
{
    if (name == "ladybird"sv)
        return ProcessType::UI;
    if (name == "ladybird-hub"sv)
        return ProcessType::Hub;
    if (name == "ladybird-gpu"sv)
        return ProcessType::Compositor;
    if (name == "ladybird-render"sv)
        return ProcessType::WebContent;
    if (name == "ladybird-net"sv)
        return ProcessType::RequestServer;
    if (name == "ladybird-image"sv)
        return ProcessType::ImageDecoder;
    if (name == "ladybird-media"sv)
        return ProcessType::MediaServer;
    if (name == "ladybird-audio"sv)
        return ProcessType::AudioServer;
    if (name == "ladybird-video"sv)
        return ProcessType::VideoDecoderServer;
    if (name == "ladybird-webrtc"sv)
        return ProcessType::WebRTCClient;

    return {};
}

// NB: Processes are named after their executables, so that these names match the ones that ps and top show. The one
//     exception is the Browser type, which has no executable of its own. The Hub reports its crashes as the Browser,
//     which is named ladybird like the UI.
StringView process_name_from_type(ProcessType type)
{
    switch (type) {
    case ProcessType::Browser:
    case ProcessType::UI:
        return "ladybird"sv;
    case ProcessType::Hub:
        return "ladybird-hub"sv;
    case ProcessType::Compositor:
        return "ladybird-gpu"sv;
    case ProcessType::WebContent:
        return "ladybird-render"sv;
    case ProcessType::RequestServer:
        return "ladybird-net"sv;
    case ProcessType::ImageDecoder:
        return "ladybird-image"sv;
    case ProcessType::MediaServer:
        return "ladybird-media"sv;
    case ProcessType::AudioServer:
        return "ladybird-audio"sv;
    case ProcessType::VideoDecoderServer:
        return "ladybird-video"sv;
    case ProcessType::WebRTCClient:
        return "ladybird-webrtc"sv;
    }
    VERIFY_NOT_REACHED();
}

ProcessManager::ProcessManager()
    : on_process_added([](Process&) { })
    , on_process_exited([](Process&&, Optional<int>) { })
    , on_process_near_memory_limit([](Process&, u64) { })
    , on_process_exceeded_memory_limit([](Process&, u64) { })
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

ProcessManager::~ProcessManager() = default;

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
    m_children_ended_for_memory_use.remove(child_id);
    m_memory_limit_warnings.remove(child_id);
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

u64 ProcessManager::default_web_content_memory_limit()
{
#if defined(HAS_ADDRESS_SANITIZER)
    // NB: A sanitizer multiplies the memory a helper uses, so the limit would end helpers that behave correctly.
    return 0;
#else
    // A page that needs this much memory is almost always a runaway one. A machine with more memory can let a page
    // go further before the rest of the system suffers, and a machine with little memory suffers long before one
    // helper uses all of it.
    auto physical_memory = Core::System::physical_memory_bytes();
    auto limit = physical_memory > 16 * GiB ? 16 * GiB : 8 * GiB;
    return min(limit, physical_memory / 2);
#endif
}

void ProcessManager::set_web_content_memory_limit(u64 limit_in_bytes)
{
    verify_event_loop();
    m_web_content_memory_limit = limit_in_bytes;
    if (limit_in_bytes == 0) {
        if (m_web_content_memory_limit_timer)
            m_web_content_memory_limit_timer->stop();
        return;
    }

    // NB: A runaway helper can grow by gigabytes per second, so a long interval would let it pass the limit by far.
    static constexpr auto check_interval = AK::Duration::from_seconds(2);
    if (!m_web_content_memory_limit_timer) {
        m_web_content_memory_limit_timer = Core::Timer::create_repeating(check_interval, [this] {
            check_web_content_memory_use();
        });
    }
    m_web_content_memory_limit_timer->start();
}

void ProcessManager::check_web_content_memory_use()
{
    verify_event_loop();

    // A helper this near the limit is asked to release memory, so that a page that only holds much garbage or many
    // caches does not have to be ended. The limit itself is not relaxed for it: a runaway helper does not shrink,
    // and waiting for it would only let it grow further.
    auto warning_threshold = m_web_content_memory_limit / 4 * 3;
    // NB: Releasing memory is costly for a helper, and one that stays near the limit most likely has nothing left to
    //     release. It is asked again, but each time after twice the wait, until it has been well below the limit for
    //     a while.
    static constexpr auto first_warning_interval = AK::Duration::from_seconds(10);
    static constexpr auto longest_warning_interval = AK::Duration::from_seconds(640);

    struct HelperAndMemoryUse {
        u64 child_id { 0 };
        u64 memory_usage_in_bytes { 0 };
    };
    Vector<HelperAndMemoryUse> helpers_near_limit;
    Vector<HelperAndMemoryUse> helpers_over_limit;
    auto now = MonotonicTime::now_coarse();
    for (auto const& info : m_statistics.processes) {
        // NB: Child ID zero is the browser itself.
        auto child_id = m_child_ids_by_pid.get(info->pid);
        if (!child_id.has_value() || *child_id == 0 || m_children_ended_for_memory_use.contains(*child_id))
            continue;
        // NB: Only WebContent has a limit for now. The other helpers serve every tab, so ending one of them would
        //     disturb all tabs, and none of them can tell the user why it was ended.
        if (auto process = find_child(*child_id); !process.has_value() || process->type() != ProcessType::WebContent)
            continue;
        auto usage = Core::Platform::process_resource_usage(*info);
        if (!usage.has_value())
            continue;
        if (usage->attributed_memory_bytes > m_web_content_memory_limit) {
            helpers_over_limit.append({ *child_id, usage->attributed_memory_bytes });
            continue;
        }
        auto warning = m_memory_limit_warnings.get(*child_id);
        if (usage->attributed_memory_bytes <= warning_threshold) {
            // NB: A helper that only dipped below the threshold keeps its wait, so that one that goes back and forth
            //     across the threshold is not asked each time it crosses.
            if (warning.has_value() && now >= warning->next_time + warning->interval)
                m_memory_limit_warnings.remove(*child_id);
            continue;
        }
        if (warning.has_value() && now < warning->next_time)
            continue;
        auto interval = warning.has_value() ? min(warning->interval + warning->interval, longest_warning_interval) : first_warning_interval;
        m_memory_limit_warnings.set(*child_id, { now + interval, interval });
        helpers_near_limit.append({ *child_id, usage->attributed_memory_bytes });
    }

    for (auto const& helper : helpers_near_limit) {
        auto process = find_child(helper.child_id);
        if (!process.has_value())
            continue;
        dbgln("{} process {} is near its memory limit: it uses {}, and the limit is {}", process_name_from_type(process->type()), process->pid(), human_readable_size(helper.memory_usage_in_bytes), human_readable_size(m_web_content_memory_limit));
        on_process_near_memory_limit(*process, helper.memory_usage_in_bytes);
    }

    // The callback may change the process table, so each helper is looked up again.
    for (auto const& helper : helpers_over_limit) {
        auto process = find_child(helper.child_id);
        if (!process.has_value())
            continue;
        warnln("Ending {} process {}: it uses {} of memory, and the limit is {}", process_name_from_type(process->type()), process->pid(), human_readable_size(helper.memory_usage_in_bytes), human_readable_size(m_web_content_memory_limit));
        // NB: The exit is handled in a later task, so the process is still in the table for the callback. A helper
        //     that could not be ended is not reported, or a later crash of it would be blamed on its memory use. It
        //     is not remembered either, so the next check tries again.
        if (auto result = terminate_child(helper.child_id, Core::Process::TerminationMode::Forceful); result.is_error()) {
            warnln("Failed to end process over its memory limit: {}", result.error());
            continue;
        }
        m_children_ended_for_memory_use.set(helper.child_id);
        on_process_exceeded_memory_limit(*process, helper.memory_usage_in_bytes);
    }
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
