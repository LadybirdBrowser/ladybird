/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Process.h>
#include <LibWebView/Process.h>

namespace WebView {

Process::Process(ProcessType type, RefPtr<IPC::ConnectionBase> connection, Core::Process process)
    : m_process(move(process))
    , m_type(type)
    , m_connection(move(connection))
{
}

Process::~Process()
{
    if (m_connection)
        m_connection->shutdown();
}

}
