/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ProcessReaperProtocol.h>
#include <bsm/libbsm.h>
#include <errno.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>

// macOS cannot ask the kernel to kill a process when its parent dies, so a compromised helper could keep running with
// its network, cache and other capabilities after the Browser is gone. The Browser starts this process with the read
// end of a pipe as descriptor 3 and writes a record for every helper that it spawns to the pipe. When the Browser exits
// or dies, the pipe reaches end-of-file. The helpers get a moment to exit on their own, and then we kill the rest.
//
// This program uses nothing but system libraries, so that it has no libraries of ours to load.

static constexpr int browser_pipe_fd = 3;
static constexpr int grace_period_in_seconds = 2;

// The audit token of a process names that process instance, not just its PID: signals sent through it fail once the
// process is gone, even if its PID has been given to another process.
struct Helper {
    pid_t pid;
    audit_token_t audit_token;
};

struct Helpers {
    Helper* helpers { nullptr };
    size_t count { 0 };
    size_t capacity { 0 };

    void add(Helper const& helper)
    {
        if (count == capacity) {
            auto new_capacity = capacity == 0 ? 64 : capacity * 2;
            auto* new_helpers = static_cast<Helper*>(realloc(helpers, new_capacity * sizeof(Helper)));
            if (!new_helpers)
                abort();
            helpers = new_helpers;
            capacity = new_capacity;
        }
        helpers[count++] = helper;
    }

    void remove(pid_t pid)
    {
        for (size_t i = 0; i < count; ++i) {
            if (helpers[i].pid == pid) {
                helpers[i] = helpers[--count];
                return;
            }
        }
    }
};

static bool audit_token_for_process(pid_t pid, audit_token_t& audit_token)
{
    mach_port_t task_name = MACH_PORT_NULL;
    if (task_name_for_pid(mach_task_self(), pid, &task_name) != KERN_SUCCESS)
        return false;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    auto result = task_info(task_name, TASK_AUDIT_TOKEN, reinterpret_cast<task_info_t>(&audit_token), &count);
    mach_port_deallocate(mach_task_self(), task_name);
    return result == KERN_SUCCESS && audit_token_to_pid(audit_token) == pid;
}

static bool process_is_alive(audit_token_t& audit_token)
{
    char path[PROC_PIDPATHINFO_MAXSIZE];
    return proc_pidpath_audittoken(&audit_token, path, sizeof(path)) > 0;
}

static void watch(int queue, Helpers& helpers, WebView::ProcessReaperRecord const& record)
{
    struct kevent change {};
    EV_SET(&change, record.pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(queue, &change, 1, nullptr, 0, nullptr) != 0)
        return;

    // The helper may have exited and been reaped before we got here, and its PID may belong to another process now.
    // Only the process that started when the helper did is the helper. The audit token is still valid after the start
    // time was read, so both describe the same process instance. Since we already watch the PID, the helper cannot exit
    // unnoticed after this.
    Helper helper { .pid = record.pid, .audit_token = {} };
    uint64_t start_time = 0;
    if (!audit_token_for_process(record.pid, helper.audit_token)
        || !WebView::process_start_time_in_microseconds(record.pid, start_time)
        || start_time != record.start_time_in_microseconds
        || !process_is_alive(helper.audit_token)) {
        EV_SET(&change, record.pid, EVFILT_PROC, EV_DELETE, 0, 0, nullptr);
        (void)kevent(queue, &change, 1, nullptr, 0, nullptr);
        return;
    }
    helpers.add(helper);
}

int main()
{
    auto queue = kqueue();
    if (queue < 0)
        return 1;

    struct kevent change {};
    EV_SET(&change, browser_pipe_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (kevent(queue, &change, 1, nullptr, 0, nullptr) < 0)
        return 1;

    Helpers helpers;
    while (true) {
        struct kevent event {};
        auto count = kevent(queue, nullptr, 0, &event, 1, nullptr);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (event.filter == EVFILT_PROC) {
            helpers.remove(static_cast<pid_t>(event.ident));
            continue;
        }

        WebView::ProcessReaperRecord records[16];
        auto bytes = read(browser_pipe_fd, records, sizeof(records));
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0)
            break;
        // Writes of a single record to a pipe are atomic, so reads return whole records.
        for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(WebView::ProcessReaperRecord); ++i)
            watch(queue, helpers, records[i]);
    }

    // The Browser is gone. Stop listening to the pipe, whose end-of-file would otherwise keep waking us up.
    EV_SET(&change, browser_pipe_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    (void)kevent(queue, &change, 1, nullptr, 0, nullptr);

    timespec grace_period { .tv_sec = grace_period_in_seconds, .tv_nsec = 0 };
    while (helpers.count > 0) {
        struct kevent event {};
        auto count = kevent(queue, nullptr, 0, &event, 1, &grace_period);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        if (event.filter == EVFILT_PROC)
            helpers.remove(static_cast<pid_t>(event.ident));
    }

    // Signal through the audit tokens, so that a helper that exits right now cannot make us kill whichever process gets
    // its PID next.
    for (size_t i = 0; i < helpers.count; ++i)
        proc_signal_with_audittoken(&helpers.helpers[i].audit_token, SIGKILL);
    return 0;
}
