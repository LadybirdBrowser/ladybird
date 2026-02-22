/*
 * Copyright (c) 2026, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Windows.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibCore/System.h>
#include <psapi.h>

namespace Core::Platform {

ErrorOr<void> update_process_statistics(ProcessStatistics& statistics)
{
    FILETIME kernel_time;
    FILETIME user_time;
    u64 total_time;
    static u32 cpu_count = Core::System::hardware_concurrency();
    BOOL result = GetSystemTimes(NULL, &kernel_time, &user_time);
    if (!result)
        return Error::from_windows_error();

    total_time = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
    u64 total_time_diff = total_time - statistics.total_time_scheduled;
    statistics.total_time_scheduled = total_time;

    for (auto const& process : statistics.processes) {
        FILETIME process_creation_time;
        FILETIME process_exit_time;
        FILETIME process_kernel_time;
        FILETIME process_user_time;

        HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process->pid);
        if (!process_handle)
            continue;

        result = GetProcessTimes(process_handle, &process_creation_time, &process_exit_time, &process_kernel_time, &process_user_time);
        if (!result)
            continue;

        u64 time_in_process = filetime_to_u64(process_kernel_time) + filetime_to_u64(process_user_time);
        u64 time_in_process_diff = time_in_process - process->time_spent_in_process;
        process->time_spent_in_process = time_in_process;

        if (total_time_diff > 0) {
            process->cpu_percent = ((f32)time_in_process_diff / (f32)total_time_diff / (f32)cpu_count) * 100.0f;
        }

        PROCESS_MEMORY_COUNTERS counters;
        result = GetProcessMemoryInfo(process_handle, &counters, sizeof(counters));
        if (!result)
            continue;
        process->memory_usage_bytes = counters.WorkingSetSize;
    }

    return {};
}

}
