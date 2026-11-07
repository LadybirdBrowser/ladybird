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
static auto ncpu_online = sysconf(_SC_NPROCESSORS_ONLN);

// The memory a process keeps alive: its anonymous and shared memory pages, and what it has in swap. Pages of files are
// the page cache's, shared with every other process that maps the same libraries, so they are left out.
// NB: Shared memory counts in every process that maps it. Decoded images and backing stores live there, so leaving it
//     out would hide most of the memory a renderer spends on pixels.
// NB: Pages freed with MADV_FREE stay counted until the kernel reclaims them. Only /proc/pid/smaps_rollup tells them
//     apart, and reading it walks every page of the process, which takes tens of milliseconds for a large one.
static Optional<u64> own_memory_bytes(pid_t pid)
{
    auto file = Core::File::open(ByteString::formatted("/proc/{}/status", pid), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    Optional<u64> anonymous_kib;
    Optional<u64> shared_memory_kib;
    Optional<u64> swap_kib;
    StringView { contents.value() }.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView line) {
        auto parse_kib = [&](StringView name) -> Optional<u64> {
            if (!line.starts_with(name))
                return {};
            auto value = line.substring_view(name.length()).trim_whitespace();
            if (!value.ends_with(" kB"sv))
                return {};
            return value.substring_view(0, value.length() - 3).trim_whitespace().to_number<u64>();
        };
        if (auto value = parse_kib("RssAnon:"sv); value.has_value())
            anonymous_kib = value;
        else if (auto value = parse_kib("RssShmem:"sv); value.has_value())
            shared_memory_kib = value;
        else if (auto value = parse_kib("VmSwap:"sv); value.has_value())
            swap_kib = value;
    });
    if (!anonymous_kib.has_value() || !shared_memory_kib.has_value())
        return {};

    Checked<u64> bytes = *anonymous_kib;
    bytes += *shared_memory_kib;
    // NB: A kernel without swap support may leave the line out.
    bytes += swap_kib.value_or(0);
    bytes *= 1024;
    if (bytes.has_overflow())
        return {};
    return bytes.value();
}

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

        // We only care about fields 14 (utime) and 15 (stime)
        unsigned long utime = 0;
        unsigned long stime = 0;
        //                                                        1   2   3   4   5   6   7   8   9   10  11  12  13  14* 15*
        res = sscanf(reinterpret_cast<char const*>(line.data()), "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime);
        if (res != 2)
            return Error::from_string_literal("Failed to parse /proc/pid/stat");

        if (auto footprint = own_memory_bytes(process->pid); footprint.has_value())
            process->memory_usage_bytes = *footprint;

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
    auto memory = own_memory_bytes(process.pid);
    if (!memory.has_value())
        return {};
    Checked<u64> cpu_time = *user;
    cpu_time += *system;
    cpu_time *= 1'000'000;
    if (cpu_time.has_overflow())
        return {};
    return ProcessResourceUsage { cpu_time.value() / user_hz, *memory };
}

}
