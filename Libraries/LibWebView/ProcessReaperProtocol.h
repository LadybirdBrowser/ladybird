/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Shared by the Browser and the ProcessReaper helper, which uses nothing but system libraries.

#include <libproc.h>
#include <stdint.h>
#include <sys/proc_info.h>
#include <sys/types.h>

namespace WebView {

// A PID alone does not identify a process: once the helper has exited and the Browser has reaped it, the system may
// give its PID to another process. The start time tells the two apart.
struct ProcessReaperRecord {
    pid_t pid;
    uint32_t padding;
    uint64_t start_time_in_microseconds;
};
static_assert(sizeof(ProcessReaperRecord) == 16);

inline bool process_start_time_in_microseconds(pid_t pid, uint64_t& start_time)
{
    proc_bsdinfo info {};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return false;
    start_time = info.pbi_start_tvsec * 1'000'000 + info.pbi_start_tvusec;
    return true;
}

}
