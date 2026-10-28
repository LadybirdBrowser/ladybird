/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/ThreadSafeWeakable.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/Export.h>
#include <LibMedia/Producers/RemoteVideoProducer.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientEndpoint.h>
#include <LibMedia/VideoPresentation/VideoPresentationServerEndpoint.h>
#include <LibMedia/VideoSinkHandle.h>

namespace Media {

// The presentation-client end of the cross-process video presentation channel, hosted in the Compositor. It
// asks the producer-server to create edges, builds the consumer + DisplayingVideoSink for each, and hands the
// sink to the host (via the ready callback passed to create_edge) to tick and draw. Process-agnostic:
// host-specific work happens through that callback, never by referencing Compositor types here.
class MEDIA_API VideoPresentationClientConnection final
    : public IPC::ConnectionToServer<VideoPresentationClientEndpoint, VideoPresentationServerEndpoint>
    , public VideoPresentationClientEndpoint
    , public ThreadSafeWeakable<VideoPresentationClientConnection> {
    C_OBJECT_ABSTRACT(VideoPresentationClientConnection);

public:
    static constexpr size_t MAX_EDGE_COUNT = 64;

    explicit VideoPresentationClientConnection(NonnullOwnPtr<IPC::Transport>);
    virtual ~VideoPresentationClientConnection() override;

    // Asks the producer-server to create an edge for the given sink, then invokes on_ready with the built
    // DisplayingVideoSink once it arrives. The edge id correlating the request and reply is allocated and kept here.
    ErrorOr<void> create_edge(VideoSinkHandle, Function<void(NonnullRefPtr<DisplayingVideoSink>)> on_ready);
    void release_edge(VideoSinkHandle);

private:
    virtual void die() override;

    virtual void video_edge_ready(u64 edge_id, Media::MediaTimeReader time_reader) override;
    virtual void update_edge_time_reader(u64 edge_id, Media::MediaTimeReader time_reader) override;
    virtual void lend_frame(u64 edge_id, u64 lend_id, u32 seek_id, Media::VideoFrameMetadata metadata, Media::VideoFramePixels pixels) override;
    virtual void edge_status_changed(u64 edge_id, Media::PipelineStatus status, u32 seek_id) override;
    virtual void notify_clock_resumed(u64 edge_id) override;
    virtual void seek_sink(u64 edge_id, u32 requested_seek_id, InvalidateHeldData) override;

    struct EdgeState {
        VideoSinkHandle handle;
        Function<void(NonnullRefPtr<DisplayingVideoSink>)> on_ready;
        RefPtr<RemoteVideoProducer> consumer;
        RefPtr<DisplayingVideoSink> sink;
        Optional<MediaTimeReader> time_reader;
        u64 sender_id { 0 };
        u32 expected_requested_seek_id { 0 };
    };

    HashMap<u64, EdgeState> m_edge_states;
    u64 m_next_edge_id { 1 };
};

}
