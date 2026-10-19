/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/ClientConnection.h>
#include <LibAudio/OutputSession.h>
#include <LibCore/EventLoop.h>

namespace Audio {

static u64 s_next_session_id { 1 };

NonnullRefPtr<OutputSession> OutputSession::create(PlaybackGroupId group)
{
    return adopt_ref(*new OutputSession(group));
}

OutputSession::OutputSession(PlaybackGroupId group)
    : m_id(s_next_session_id++)
    , m_group(group)
    , m_control_loop(Core::EventLoop::current())
{
}

OutputSession::~OutputSession()
{
    // The last reference may go on a stream's pump or audio thread, and the connection is only used on its own.
    auto say_goodbye = [connection = move(m_declared_connection), session_id = m_id] {
        if (auto strong_connection = connection.strong_ref())
            strong_connection->session_destroyed({}, session_id);
    };
    if (Core::EventLoop::is_running() && &Core::EventLoop::current() == &m_control_loop) {
        say_goodbye();
        return;
    }
    m_control_loop.deferred_invoke(move(say_goodbye));
}

void OutputSession::set_group(PlaybackGroupId group)
{
    if (m_group == group)
        return;
    m_group = group;
    if (auto connection = m_declared_connection.strong_ref())
        connection->session_rebound({}, m_id, group);
}

void OutputSession::declare_on(Badge<ClientConnection>, ClientConnection& connection)
{
    if (m_declared_connection.ptr() == &connection)
        return;
    m_declared_connection = connection.make_weak_ptr<ClientConnection>();
    connection.async_declare_session(m_id, m_group);
}

}
