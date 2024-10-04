/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/File.h>
#include <LibCore/ProcessStatisticsReader.h>
#include <pwd.h>

namespace Core {

HashMap<uid_t, ByteString> ProcessStatisticsReader::s_usernames;

ErrorOr<AllProcessesStatistics> ProcessStatisticsReader::get_all(SeekableStream& proc_all_file, bool include_usernames)
{
    TRY(proc_all_file.seek(0, SeekMode::SetPosition));

    AllProcessesStatistics all_processes_statistics;

    auto file_contents = TRY(proc_all_file.read_until_eof());
    auto json_obj = TRY(JsonValue::from_string(file_contents)).as_object();
    json_obj.get_array("processes"sv)->for_each([&](auto& value) {
        JsonObject const& process_object = value.as_object();
        Core::ProcessStatistics process;

        // kernel data first
        process.pid = process_object.get_u32("pid"sv).value_or(0);
        process.pgid = process_object.get_u32("pgid"sv).value_or(0);
        process.pgp = process_object.get_u32("pgp"sv).value_or(0);
        process.sid = process_object.get_u32("sid"sv).value_or(0);
        process.uid = process_object.get_u32("uid"sv).value_or(0);
        process.gid = process_object.get_u32("gid"sv).value_or(0);
        process.ppid = process_object.get_u32("ppid"sv).value_or(0);
        process.kernel = process_object.get_bool("kernel"sv).value_or(false);
        process.name = process_object.get_byte_string("name"sv).value_or("");
        process.executable = process_object.get_byte_string("executable"sv).value_or("");
        process.tty = process_object.get_byte_string("tty"sv).value_or("");
        process.pledge = process_object.get_byte_string("pledge"sv).value_or("");
        process.veil = process_object.get_byte_string("veil"sv).value_or("");
        process.creation_time = UnixDateTime::from_nanoseconds_since_epoch(process_object.get_i64("creation_time"sv).value_or(0));
        process.amount_virtual = process_object.get_u32("amount_virtual"sv).value_or(0);
        process.amount_resident = process_object.get_u32("amount_resident"sv).value_or(0);
        process.amount_shared = process_object.get_u32("amount_shared"sv).value_or(0);
        process.amount_dirty_private = process_object.get_u32("amount_dirty_private"sv).value_or(0);
        process.amount_clean_inode = process_object.get_u32("amount_clean_inode"sv).value_or(0);
        process.amount_purgeable_volatile = process_object.get_u32("amount_purgeable_volatile"sv).value_or(0);
        process.amount_purgeable_nonvolatile = process_object.get_u32("amount_purgeable_nonvolatile"sv).value_or(0);

        auto& thread_array = process_object.get_array("threads"sv).value();
        process.threads.ensure_capacity(thread_array.size());
        thread_array.for_each([&](auto& value) {
            auto& thread_object = value.as_object();
            Core::ThreadStatistics thread;
            thread.tid = thread_object.get_u32("tid"sv).value_or(0);
            thread.times_scheduled = thread_object.get_u32("times_scheduled"sv).value_or(0);
            thread.name = thread_object.get_byte_string("name"sv).value_or("");
            thread.state = thread_object.get_byte_string("state"sv).value_or("");
            thread.time_user = thread_object.get_u64("time_user"sv).value_or(0);
            thread.time_kernel = thread_object.get_u64("time_kernel"sv).value_or(0);
            thread.cpu = thread_object.get_u32("cpu"sv).value_or(0);
            thread.priority = thread_object.get_u32("priority"sv).value_or(0);
            thread.syscall_count = thread_object.get_u32("syscall_count"sv).value_or(0);
            thread.inode_faults = thread_object.get_u32("inode_faults"sv).value_or(0);
            thread.zero_faults = thread_object.get_u32("zero_faults"sv).value_or(0);
            thread.cow_faults = thread_object.get_u32("cow_faults"sv).value_or(0);
            thread.unix_socket_read_bytes = thread_object.get_u64("unix_socket_read_bytes"sv).value_or(0);
            thread.unix_socket_write_bytes = thread_object.get_u64("unix_socket_write_bytes"sv).value_or(0);
            thread.ipv4_socket_read_bytes = thread_object.get_u64("ipv4_socket_read_bytes"sv).value_or(0);
            thread.ipv4_socket_write_bytes = thread_object.get_u64("ipv4_socket_write_bytes"sv).value_or(0);
            thread.file_read_bytes = thread_object.get_u64("file_read_bytes"sv).value_or(0);
            thread.file_write_bytes = thread_object.get_u64("file_write_bytes"sv).value_or(0);
            process.threads.append(move(thread));
        });

        // and synthetic data last
        if (include_usernames) {
            process.username = username_from_uid(process.uid);
        }
        all_processes_statistics.processes.append(move(process));
    });

    all_processes_statistics.total_time_scheduled = json_obj.get_u64("total_time"sv).value_or(0);
    all_processes_statistics.total_time_scheduled_kernel = json_obj.get_u64("total_time_kernel"sv).value_or(0);
    return all_processes_statistics;
}

ErrorOr<AllProcessesStatistics> ProcessStatisticsReader::get_all(bool include_usernames)
{
    auto proc_all_file = TRY(Core::File::open("/sys/kernel/processes"sv, Core::File::OpenMode::Read));
    return get_all(*proc_all_file, include_usernames);
}

ByteString ProcessStatisticsReader::username_from_uid(uid_t uid)
{
    if (s_usernames.is_empty()) {
        setpwent();
        while (auto* passwd = getpwent())
            s_usernames.set(passwd->pw_uid, passwd->pw_name);
        endpwent();
    }

    auto it = s_usernames.find(uid);
    if (it != s_usernames.end())
        return (*it).value;
    return ByteString::number(uid);
}
}
