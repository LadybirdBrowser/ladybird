/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibMedia/Export.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientEndpoint.h>
#include <LibMedia/VideoPresentation/VideoPresentationServerEndpoint.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibSync/Weakable.h>

namespace Media {

// The producer-server end of the cross-process video presentation channel, hosted in the process that owns the
// producers (WebContent today, MediaServer later). It builds a RemoteVideoSink pump per edge on request, ships
// the edge's shared-memory handles to the presentation client, and routes the consumer's demands to the pump.
class MEDIA_API VideoPresentationServerConnection final
    : public IPC::ConnectionFromClient<VideoPresentationClientEndpoint, VideoPresentationServerEndpoint>
    , public Sync::Weakable<VideoPresentationServerConnection> {
    C_OBJECT(VideoPresentationServerConnection);

public:
    virtual ~VideoPresentationServerConnection() override;

private:
    explicit VideoPresentationServerConnection(NonnullOwnPtr<IPC::Transport>);

    virtual void die() override;

    virtual void create_video_edge(VideoSinkHandle video_sink_handle, u64 edge_id) override;
    virtual void release_video_edge(u64 edge_id) override;
    virtual void request_start(u64 edge_id) override;
    virtual void request_seek(u64 edge_id, AK::Duration timestamp) override;
    virtual void release_slot(u64 edge_id, VideoFramePoolID pool_id, u32 slot_index) override;
    virtual void notify_space_available(u64 edge_id) override;
    virtual void notify_sink_status(u64 edge_id, PipelineStatus status, u32 requested_seek_id) override;
    virtual void notify_sink_resize(u64 edge_id, u32 width, u32 height) override;
    virtual void set_sink_ticking(u64 edge_id, bool ticking) override;

    struct EdgeState {
        NonnullRefPtr<RemoteVideoSink> pump;
        VideoSinkHandle handle;
        u32 actual_requested_seek_id { 0 };
    };

    HashMap<u64, EdgeState> m_edge_states;
};

}
