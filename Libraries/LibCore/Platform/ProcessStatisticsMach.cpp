/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if !defined(AK_OS_MACH)
#    error "This file is only available on Mach platforms"
#endif

#include <AK/Time.h>
#include <LibCore/Platform/ProcessStatisticsMach.h>

namespace Core::Platform {

static auto user_hz = sysconf(_SC_CLK_TCK);

static constexpr bool task_is_unavailable(kern_return_t result)
{
    // A dead task port fails in the MIG client. If task termination races the request, task_info() instead sees an
    // inactive task and returns KERN_INVALID_ARGUMENT.
    return result == MACH_SEND_INVALID_DEST || result == KERN_INVALID_ARGUMENT;
}

ErrorOr<void> update_process_statistics(ProcessStatistics& statistics)
{
    host_cpu_load_info_data_t cpu_info {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    auto res = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu_info), &count);
    if (res != KERN_SUCCESS) {
        dbgln("Failed to get host statistics: {}", mach_error_string(res));
        return Core::mach_error_to_error(res);
    }

    u64 total_cpu_ticks = 0;
    total_cpu_ticks += cpu_info.cpu_ticks[CPU_STATE_USER];
    total_cpu_ticks += cpu_info.cpu_ticks[CPU_STATE_SYSTEM];
    total_cpu_ticks += cpu_info.cpu_ticks[CPU_STATE_NICE];
    total_cpu_ticks += cpu_info.cpu_ticks[CPU_STATE_IDLE];

    auto const total_cpu_ticks_diff = total_cpu_ticks - statistics.total_time_scheduled;
    auto const total_cpu_seconds_diff = total_cpu_ticks_diff / (static_cast<float>(user_hz));
    auto const total_cpu_micro_diff = total_cpu_seconds_diff * 1'000'000;
    statistics.total_time_scheduled = total_cpu_ticks;

    for (auto& process : statistics.processes) {
        // A newly spawned process may be added to the process manager before its task port is delivered to the browser
        // event loop. Skip it for this sample; the port will be available on the next update.
        if (!MACH_PORT_VALID(process->child_task_port.port())) {
            process->reset_cpu_time();
            continue;
        }

        mach_task_basic_info_data_t basic_info {};
        count = MACH_TASK_BASIC_INFO_COUNT;
        res = task_info(process->child_task_port.port(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic_info), &count);
        if (res != KERN_SUCCESS) {
            if (task_is_unavailable(res)) {
                process->reset_cpu_time();
                continue;
            }

            dbgln("Failed to get task info for pid {}: {}", process->pid, mach_error_string(res));
            return Core::mach_error_to_error(res);
        }

        process->memory_usage_bytes = basic_info.resident_size;

        task_thread_times_info_data_t time_info {};
        count = TASK_THREAD_TIMES_INFO_COUNT;
        res = task_info(process->child_task_port.port(), TASK_THREAD_TIMES_INFO, reinterpret_cast<task_info_t>(&time_info), &count);
        if (res != KERN_SUCCESS) {
            if (task_is_unavailable(res)) {
                process->reset_cpu_time();
                continue;
            }

            dbgln("Failed to get thread times info for pid {}: {}", process->pid, mach_error_string(res));
            return Core::mach_error_to_error(res);
        }

        timeval scratch_timeval = { static_cast<time_t>(time_info.user_time.seconds), static_cast<suseconds_t>(time_info.user_time.microseconds) };
        auto time_in_process = AK::Duration::from_timeval(scratch_timeval);
        scratch_timeval = { static_cast<time_t>(time_info.system_time.seconds), static_cast<suseconds_t>(time_info.system_time.microseconds) };
        time_in_process += AK::Duration::from_timeval(scratch_timeval);

        auto time_diff_process = time_in_process - AK::Duration::from_microseconds(process->time_spent_in_process);
        process->time_spent_in_process = time_in_process.to_microseconds();

        process->cpu_percent = 0.0f;
        if (process->has_cpu_time_baseline && time_diff_process > AK::Duration::zero() && total_cpu_micro_diff > 0.0f)
            process->cpu_percent = 100.0f * static_cast<float>(time_diff_process.to_microseconds()) / total_cpu_micro_diff;
        process->has_cpu_time_baseline = true;
    }

    return {};
}

}
