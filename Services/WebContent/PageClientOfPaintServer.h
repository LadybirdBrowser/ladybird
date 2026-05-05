/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibPaintServer/Types.h>
#include <LibURL/Origin.h>

namespace PaintServer {

class ArenaSubmitter;

}

namespace Web::Painting {

struct DisplayListStream;

}

namespace WebContent {

class PageClient;

class PageClientOfPaintServer {
    AK_MAKE_NONCOPYABLE(PageClientOfPaintServer);
    AK_MAKE_NONMOVABLE(PageClientOfPaintServer);

public:
    explicit PageClientOfPaintServer(PageClient&);
    ~PageClientOfPaintServer();

    void arm_next_frame_logging();
    void set_surface_id(PaintServer::SurfaceID);
    void did_submit_gpu_operation(PaintServer::ReleaseToken);
    void did_ingest_gpu_operation(PaintServer::ReleaseToken);
    void did_render_gpu_operation(PaintServer::ReleaseToken, bool success);
    void did_fail_resource_registration(PaintServer::ResourceID);
    void request_frame_repaint();
    void did_change_top_level_origin(URL::Origin const&);

private:
    Optional<Web::Painting::DisplayListStream> create_stream();
    PaintServer::ArenaSubmitter& ensure_submitter();
    bool try_reset_submitter();

    PageClient& m_page_client;
    Optional<PaintServer::SurfaceID> m_gpu_surface_id;
    PaintServer::ReleaseToken m_last_submitted_gpu_release_token { 0 };
    PaintServer::ReleaseToken m_last_ingested_gpu_release_token { 0 };
    OwnPtr<PaintServer::ArenaSubmitter> m_gpu_arena_packet_submitter;
    HashMap<PaintServer::ReleaseToken, Function<void(bool)>> m_pending_canvas_render_callbacks;
    bool m_pending_gpu_resource_reset { false };
    bool m_log_next_submitted_frame { false };
    Optional<URL::Origin> m_last_top_level_origin;
};

}
