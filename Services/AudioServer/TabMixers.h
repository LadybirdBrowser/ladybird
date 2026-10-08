/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Types.h>
#include <LibIPC/TransportHandle.h>
#include <LibMedia/Audio/PlaybackStreamMixer.h>
#include <LibMedia/Audio/ServerConnection.h>

namespace AudioServer {

// One mixer per tab with a connected client process, alive from the tab's first connection to its last.
class TabMixers {
public:
    static TabMixers& the();

    ErrorOr<IPC::TransportHandle> connect_client(u64 tab_id);

private:
    struct Tab {
        NonnullRefPtr<Audio::PlaybackStreamMixer> mixer;
        HashMap<int, NonnullRefPtr<Audio::ServerConnection>> connections;
    };

    Tab& tab_for(u64 tab_id);
    void connection_died(u64 tab_id, int client_id);

    HashMap<u64, Tab> m_tabs;
    int m_next_client_id { 1 };
};

}
