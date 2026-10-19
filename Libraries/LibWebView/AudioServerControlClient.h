/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AudioServer/AudioServerControlClientEndpoint.h>
#include <AudioServer/AudioServerControlEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Export.h>

namespace WebView {

// The control plane of a renderer's AudioServer, spoken only by the Browser, which launched it.
class WEBVIEW_API AudioServerControlClient final
    : public IPC::ConnectionToServer<AudioServerControlClientEndpoint, AudioServerControlEndpoint>
    , public AudioServerControlClientEndpoint {
    C_OBJECT_ABSTRACT(AudioServerControlClient);

public:
    explicit AudioServerControlClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~AudioServerControlClient() override;

    // A client of the tab asks to capture from the device; the holder answers with capture_decided.
    Function<void(u64 tab_id, u64 request_id, ByteString const& device_id)> on_capture_requested;
    Function<void(u64 tab_id, Audio::PlaybackGroupId, Audio::Audibility)> on_playback_group_audibility_changed;
    // Whatever the AudioServer heard goes with it.
    Function<void()> on_death;

private:
    virtual void die() override;
    virtual void capture_requested(u64 tab_id, u64 request_id, ByteString device_id) override;
    virtual void playback_group_audibility_changed(u64 tab_id, Audio::PlaybackGroupId group, Audio::Audibility audibility) override;
};

}
