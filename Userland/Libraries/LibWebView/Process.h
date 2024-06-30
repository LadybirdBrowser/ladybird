/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibCore/Process.h>
#include <LibIPC/Connection.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

class Process {
    AK_MAKE_NONCOPYABLE(Process);
    AK_MAKE_DEFAULT_MOVABLE(Process);

public:
    Process(ProcessType type, RefPtr<IPC::ConnectionBase> connection, Core::Process process);
    ~Process();

    ProcessType type() const { return m_type; }
    Optional<String> const& title() const { return m_title; }
    void set_title(Optional<String> title) { m_title = move(title); }

    template<typename ConnectionFromClient>
    Optional<ConnectionFromClient&> client()
    {
        if (auto strong_connection = m_connection.strong_ref())
            return verify_cast<ConnectionFromClient>(*strong_connection);
        return {};
    }

    pid_t pid() const { return m_process.pid(); }

private:
    Core::Process m_process;
    ProcessType m_type;
    Optional<String> m_title;
    WeakPtr<IPC::ConnectionBase> m_connection;
};

}
