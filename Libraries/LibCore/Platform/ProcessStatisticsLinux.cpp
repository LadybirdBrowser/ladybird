/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <AK/String.h>
#include <LibCore/File.h>
#include <LibCore/Platform/ProcessResourceUsage.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <unistd.h>

namespace Core::Platform {

static auto user_hz = sysconf(_SC_CLK_TCK);
static auto page_size = sysconf(_SC_PAGESIZE);
static auto ncpu_online = sysconf(_SC_NPROCESSORS_ONLN);

ErrorOr<void> update_process_statistics(ProcessStatistics& statistics)
{
    // Read the total time scheduled from /proc/stat, and each process's usage from /proc/pid/stat
    // Calculate the CPU percentage for each process based on the total time scheduled and the time spent in the process

    static NeverDestroyed<NonnullOwnPtr<Core::File>> proc_stat { TRY(Core::File::open("/proc/stat"sv, Core::File::OpenMode::Read)) };
    TRY((*proc_stat)->seek(0, SeekMode::SetPosition));

    char buf[1024] = {};
    auto buffer = Bytes { buf, sizeof(buf) };
    auto line = TRY((*proc_stat)->read_some(buffer));

    int user_time = 0;
    int system_time = 0;
    int idle_time = 0;
    int irq_time = 0;
    int softirq_time = 0;

    // user, nice (ignored), system, idle, iowait (ignored), irq, softirq, steal (ignored), guest (ignored), guest_nice (ignored)
    int res = sscanf(reinterpret_cast<char const*>(line.data()), "cpu %d %*d %d %d %*d %d %d", &user_time, &system_time, &idle_time, &irq_time, &softirq_time);
    if (res != 5)
        return Error::from_string_literal("Failed to parse /proc/stat");

    u64 const total_time_scheduled = user_time + system_time + idle_time + irq_time + softirq_time;
    float const total_time_scheduled_diff = total_time_scheduled - statistics.total_time_scheduled;
    statistics.total_time_scheduled = total_time_scheduled;

    for (auto& process : statistics.processes) {
        auto proc_pid_stat_or_error = Core::File::open(MUST(String::formatted("/proc/{}/stat", process->pid)), Core::File::OpenMode::Read);
        if (proc_pid_stat_or_error.is_error()) {
            // FIXME: Remove stale process from process list?
            continue;
        }
        auto proc_pid_stat = proc_pid_stat_or_error.release_value();
        line = TRY(proc_pid_stat->read_some(buffer));

        // We only care about fields 14 (utime), 15 (stime), and 24 (rss)
        unsigned long utime = 0;
        unsigned long stime = 0;
        long rss = 0;
        //                                                        1   2   3   4   5   6   7   8   9   10  11  12  13  14* 15* 16  17  18  19  20  21  22  23  24*
        res = sscanf(reinterpret_cast<char const*>(line.data()), "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld", &utime, &stime, &rss);
        if (res != 3)
            return Error::from_string_literal("Failed to parse /proc/pid/stat");

        process->memory_usage_bytes = rss * page_size;

        u64 const time_process = utime + stime;
        float const time_scheduled_diff = time_process - process->time_spent_in_process;
        process->time_spent_in_process = time_process;

        process->cpu_percent = 0.0;
        if (total_time_scheduled_diff > 0) {
            process->cpu_percent = time_scheduled_diff / (total_time_scheduled_diff / ncpu_online) * 100.0f;
        }
    }

    return {};
}

Optional<ProcessResourceUsage> process_resource_usage(ProcessInfo const& process)
{
    auto stat_file = Core::File::open(MUST(String::formatted("/proc/{}/stat", process.pid)), Core::File::OpenMode::Read);
    if (stat_file.is_error())
        return {};
    auto contents = stat_file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto text = StringView(contents.value());
    auto closing_parenthesis = text.find_last(')');
    if (!closing_parenthesis.has_value())
        return {};
    auto fields = text.substring_view(*closing_parenthesis + 2).split_view(' ');
    if (fields.size() < 13)
        return {};
    auto user = fields[11].to_number<u64>();
    auto system = fields[12].to_number<u64>();
    if (!user.has_value() || !system.has_value() || user_hz <= 0)
        return {};
    auto memory_file = Core::File::open(MUST(String::formatted("/proc/{}/statm", process.pid)), Core::File::OpenMode::Read);
    if (memory_file.is_error())
        return {};
    auto memory_contents = memory_file.value()->read_until_eof();
    if (memory_contents.is_error())
        return {};
    auto memory_fields = StringView(memory_contents.value()).split_view(' ');
    if (memory_fields.size() < 3)
        return {};
    auto resident = memory_fields[1].to_number<u64>();
    auto shared = memory_fields[2].to_number<u64>();
    if (!resident.has_value() || !shared.has_value() || *resident < *shared || page_size <= 0)
        return {};
    // NB: statm's resident minus shared is a cheap private resident estimate. It omits
    //     file-backed private pages and does not include swapped-out memory.
    Checked<u64> cpu_time = *user;
    cpu_time += *system;
    cpu_time *= 1'000'000;
    Checked<u64> memory = *resident - *shared;
    memory *= page_size;
    if (cpu_time.has_overflow() || memory.has_overflow())
        return {};
    return ProcessResourceUsage { cpu_time.value() / user_hz, memory.value() };
}

}
