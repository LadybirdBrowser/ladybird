/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>

namespace WebView {

class ProcessMonitor {
public:
    ProcessMonitor(Function<void(pid_t)> exit_handler);
    ~ProcessMonitor();

    void add_process(pid_t pid);

private:
    Function<void(pid_t)> m_on_process_exit;
    HashTable<pid_t> m_monitored_processes;
    [[maybe_unused]] int m_signal_handle { -1 };
};

}
