/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <LibGfx/Resource/FontResource.h>
#include <LibPaintServer/ArenaWriter.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class RenderClientOfPaintServer;

class ArenaSubmitter {
    AK_MAKE_NONCOPYABLE(ArenaSubmitter);
    AK_MAKE_NONMOVABLE(ArenaSubmitter);

public:
    ArenaSubmitter() = default;
    ~ArenaSubmitter();

    Optional<PaintServer::ReleaseToken> submit_draw_list(RenderClientOfPaintServer&, PaintServer::SurfaceID, FrameHeader const&, ReadonlyBytes payload, PaintServer::SyncToken wait_sync_token, PaintServer::SyncToken render_wait_sync_token, Vector<Gfx::ResourceTransfer>&& resource_submissions);
    Optional<PaintServer::ReleaseToken> submit_canvas_draw_list(RenderClientOfPaintServer&, PaintServer::SurfaceID, ImageID, Gfx::IntSize, Gfx::BitmapFormat, ReadonlyBytes payload, PaintServer::SyncToken wait_sync_token, Vector<Gfx::ResourceTransfer>&& resource_submissions);
    bool begin_streamed_draw_list(RenderClientOfPaintServer&);
    ErrorOr<void> write_streamed_draw_list_payload(size_t offset, ReadonlyBytes bytes);
    Optional<ReadonlyBytes> read_streamed_draw_list_payload(size_t offset, size_t size) const;
    Optional<PaintServer::ReleaseToken> finish_streamed_draw_list(RenderClientOfPaintServer&, PaintServer::SurfaceID, FrameHeader const&, PaintServer::SyncToken wait_sync_token, PaintServer::SyncToken render_wait_sync_token);
    void abort_streamed_draw_list();
    void reset_resources(RenderClientOfPaintServer&);
    void reset_all(RenderClientOfPaintServer&);
    void did_server_ingest(PaintServer::ReleaseToken);
    size_t arena_count() const { return m_arena_writer.arena_count(); }

private:
    SyncToken submit_resources(RenderClientOfPaintServer&, PaintServer::SurfaceID, Vector<Gfx::ResourceTransfer>&& resource_submissions);
    Optional<PaintServer::ReleaseToken> submit_resource_packet(RenderClientOfPaintServer&, PaintServer::SurfaceID, Gfx::ResourceTransfer const&);
    void clear_registered_resources();
    void unregister_resources(RenderClientOfPaintServer&);
    static bool fallback_to_shared_blob(Gfx::ResourceTransfer const&);

public:
    void did_fail_resource(PaintServer::SurfaceID, PaintServer::ResourceID, RenderClientOfPaintServer&);

    ArenaWriter m_arena_writer;
    HashMap<PaintServer::SurfaceID, HashTable<PaintServer::ResourceID>> m_registered_resources;
    u64 m_last_seen_server_epoch { 0 };
};

}
