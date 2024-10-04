/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <unistd.h>

namespace Core {

struct ThreadStatistics {
    pid_t tid;
    unsigned times_scheduled;
    u64 time_user;
    u64 time_kernel;
    unsigned syscall_count;
    unsigned inode_faults;
    unsigned zero_faults;
    unsigned cow_faults;
    u64 unix_socket_read_bytes;
    u64 unix_socket_write_bytes;
    u64 ipv4_socket_read_bytes;
    u64 ipv4_socket_write_bytes;
    u64 file_read_bytes;
    u64 file_write_bytes;
    ByteString state;
    u32 cpu;
    u32 priority;
    ByteString name;
};

struct ProcessStatistics {
    // Keep this in sync with /sys/kernel/processes.
    // From the kernel side:
    pid_t pid;
    pid_t pgid;
    pid_t pgp;
    pid_t sid;
    uid_t uid;
    gid_t gid;
    pid_t ppid;
    bool kernel;
    ByteString name;
    ByteString executable;
    ByteString tty;
    ByteString pledge;
    ByteString veil;
    UnixDateTime creation_time;
    size_t amount_virtual;
    size_t amount_resident;
    size_t amount_shared;
    size_t amount_dirty_private;
    size_t amount_clean_inode;
    size_t amount_purgeable_volatile;
    size_t amount_purgeable_nonvolatile;

    Vector<Core::ThreadStatistics> threads;

    // synthetic
    ByteString username;
};

struct AllProcessesStatistics {
    Vector<ProcessStatistics> processes;
    u64 total_time_scheduled;
    u64 total_time_scheduled_kernel;
};

class ProcessStatisticsReader {
public:
    static ErrorOr<AllProcessesStatistics> get_all(SeekableStream&, bool include_usernames = true);
    static ErrorOr<AllProcessesStatistics> get_all(bool include_usernames = true);

private:
    static ByteString username_from_uid(uid_t);
    static HashMap<uid_t, ByteString> s_usernames;
};

}
