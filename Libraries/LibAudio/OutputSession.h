/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Badge.h>
#include <AK/NonnullRefPtr.h>
#include <AK/WeakPtr.h>
#include <LibAudio/Export.h>
#include <LibAudio/Forward.h>
#include <LibAudio/PlaybackGroupId.h>
#include <LibCore/Forward.h>

namespace Audio {

// The streams of one source of sound, such as a media element or an audio context, which the AudioServer plays as part
// of the session's group. Streams follow the session when its group changes, including those still being created.
class AUDIO_API OutputSession final : public AtomicRefCounted<OutputSession> {
public:
    // Everything but the destructor runs on the creating thread, which is the thread of the process's connection.
    static NonnullRefPtr<OutputSession> create(PlaybackGroupId);
    ~OutputSession();

    u64 id() const { return m_id; }
    void set_group(PlaybackGroupId);

    // Unless the connection already knows of the session, tells it of the session and its current group.
    void declare_on(Badge<ClientConnection>, ClientConnection&);

private:
    explicit OutputSession(PlaybackGroupId);

    u64 const m_id { 0 };
    PlaybackGroupId m_group;
    Core::EventLoop& m_control_loop;
    // A connection that replaces this one hears of the session when the session's first stream on it is created.
    WeakPtr<ClientConnection> m_declared_connection;
};

}
