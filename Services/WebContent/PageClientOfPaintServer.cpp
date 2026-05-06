/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/NumericLimits.h>
#include <AK/RefCounted.h>
#include <LibPaintServer/ArenaSubmitter.h>
#include <LibPaintServer/Debug.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/DisplayListSubmitter.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageClientOfPaintServer.h>

namespace WebContent {

class DisplayListStreamer final : public RefCounted<DisplayListStreamer> {
public:
    DisplayListStreamer(PaintServer::ArenaSubmitter& submitter, PaintServer::RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, PaintServer::SyncToken wait_sync_token, bool clear_snapshot_logging_when_done);
    ~DisplayListStreamer();

    ErrorOr<void> append_payload(size_t offset, ReadonlyBytes bytes);
    void clear_snapshot_logging_if_needed();

    PaintServer::ArenaSubmitter& submitter;
    PaintServer::RenderClientOfPaintServer& render_client;
    ByteBuffer payload;
    PaintServer::SurfaceID surface_id { 0 };
    PaintServer::SyncToken wait_sync_token { 0 };
    bool clear_snapshot_logging_when_done { false };
    bool finished { false };
};

PageClientOfPaintServer::PageClientOfPaintServer(PageClient& page_client)
    : m_page_client(page_client)
{
    Web::Painting::DisplayListSubmitter::set_sink(m_page_client.id(), [this]() -> Optional<Web::Painting::DisplayListStream> {
        return create_stream();
    });
}

Optional<Web::Painting::DisplayListStream> PageClientOfPaintServer::create_stream()
{
    if (!m_gpu_surface_id.has_value()) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            PaintServer::dbgevery(120, "WCSubmit"sv, "PageClientOfPaintServer: create_stream skipped reason=no_surface page_id={}", m_page_client.id());
        return {};
    }
    auto* gpu_render_client = m_page_client.client().paint_server_render_client();
    if (!gpu_render_client) {
        if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS))
            PaintServer::dbgevery(120, "WCSubmit"sv, "PageClientOfPaintServer: create_stream skipped reason=no_render_client page_id={} surface_id={}", m_page_client.id(), *m_gpu_surface_id);
        return {};
    }
    PaintServer::ArenaSubmitter& submitter = ensure_submitter();
    if (m_pending_gpu_resource_reset) {
        submitter.reset_all(*gpu_render_client);
        m_pending_gpu_resource_reset = false;
    }
    bool clear_snapshot_logging_when_done = false;
    if (m_log_next_submitted_frame) {
        PaintServer::set_log_next_frame();
        gpu_render_client->arm_next_frame_logging();
        m_log_next_submitted_frame = false;
        clear_snapshot_logging_when_done = true;
    }
    if (PaintServer::is_logging_enabled(PaintServer::LOG_INGRESS)) {
        PaintServer::dbgevery(120, "WCSubmit"sv, "PageClientOfPaintServer: create_stream page_id={} surface_id={} wait_sync_token={}",
            m_page_client.id(),
            *m_gpu_surface_id,
            m_last_ingested_gpu_release_token);
    }
    auto state = make_ref_counted<DisplayListStreamer>(submitter, *gpu_render_client, *m_gpu_surface_id, static_cast<PaintServer::SyncToken>(m_last_ingested_gpu_release_token), clear_snapshot_logging_when_done);
    return Web::Painting::DisplayListStream {
        .append_payload = [state](size_t offset, ReadonlyBytes bytes) -> ErrorOr<void> {
            return state->append_payload(offset, bytes);
        },
        .finish = [state](PaintServer::FrameHeader const& header, PaintServer::SyncToken render_wait_sync_token, Vector<Gfx::ResourceTransfer>&& resource_submissions) -> Optional<PaintServer::ReleaseToken> {
            state->finished = true;
            auto release_token = state->submitter.submit_draw_list(
                state->render_client,
                state->surface_id,
                header,
                state->payload.bytes(),
                state->wait_sync_token,
                render_wait_sync_token,
                move(resource_submissions));
            state->clear_snapshot_logging_if_needed();
            return release_token;
        },
        .submit_canvas_draw_list = [this, state](PaintServer::ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, ReadonlyBytes payload, Vector<Gfx::ResourceTransfer>&& resource_submissions, Function<void(bool)> callback) -> Optional<PaintServer::ReleaseToken> {
            auto release_token = state->submitter.submit_canvas_draw_list(
                state->render_client,
                state->surface_id,
                image_id,
                size,
                format,
                payload,
                state->wait_sync_token,
                move(resource_submissions));
            if (release_token.has_value() && callback)
                m_pending_canvas_render_callbacks.set(release_token.value(), move(callback));
            state->clear_snapshot_logging_if_needed();
            return release_token;
        },
        .abort = [state] {
            state->finished = true;
            state->clear_snapshot_logging_if_needed(); },
    };
}

PageClientOfPaintServer::~PageClientOfPaintServer()
{
    Web::Painting::DisplayListSubmitter::clear_sink(m_page_client.id());
}

void PageClientOfPaintServer::arm_next_frame_logging()
{
    m_log_next_submitted_frame = true;
}

void PageClientOfPaintServer::set_surface_id(PaintServer::SurfaceID surface_id)
{
    Optional<PaintServer::SurfaceID> const previous_surface_id = m_gpu_surface_id;

    if (surface_id == 0) {
        m_gpu_surface_id.clear();
        m_last_submitted_gpu_release_token = 0;
        m_last_ingested_gpu_release_token = 0;
        if (previous_surface_id.has_value()) {
            Web::Painting::DisplayListSubmitter::reset_resources_for_page(m_page_client.id());
            m_pending_gpu_resource_reset = true;
        }
        return;
    }

    if (previous_surface_id.has_value() && previous_surface_id.value() == surface_id)
        return;

    m_gpu_surface_id = surface_id;
    m_last_submitted_gpu_release_token = 0;
    m_last_ingested_gpu_release_token = 0;
    Web::Painting::DisplayListSubmitter::reset_resources_for_page(m_page_client.id());
    m_pending_gpu_resource_reset = true;
    if (try_reset_submitter())
        m_pending_gpu_resource_reset = false;
}

void PageClientOfPaintServer::did_submit_gpu_operation(PaintServer::ReleaseToken release_token)
{
    if (release_token <= m_last_submitted_gpu_release_token)
        return;

    m_last_submitted_gpu_release_token = release_token;
}

void PageClientOfPaintServer::did_ingest_gpu_operation(PaintServer::ReleaseToken release_token)
{
    if (release_token <= m_last_ingested_gpu_release_token)
        return;

    m_last_ingested_gpu_release_token = release_token;

    if (m_gpu_arena_packet_submitter)
        m_gpu_arena_packet_submitter->did_server_ingest(release_token);
}

void PageClientOfPaintServer::did_render_gpu_operation(PaintServer::ReleaseToken release_token, bool success)
{
    auto callback = m_pending_canvas_render_callbacks.take(release_token);
    if (!callback.has_value())
        return;

    callback.release_value()(success);
}

void PageClientOfPaintServer::did_fail_resource_registration(PaintServer::ResourceID resource_id)
{
    Web::Painting::DisplayListSubmitter::invalidate_resource_for_page(m_page_client.id(), resource_id);
    auto* gpu_render_client = m_page_client.client().paint_server_render_client();
    if (!gpu_render_client || !m_gpu_arena_packet_submitter || !m_gpu_surface_id.has_value())
        return;

    m_gpu_arena_packet_submitter->did_fail_resource(*m_gpu_surface_id, resource_id, *gpu_render_client);
}

void PageClientOfPaintServer::request_frame_repaint()
{
    auto traversable = m_page_client.page().top_level_traversable();
    traversable->set_needs_repaint();
    Web::HTML::main_thread_event_loop().schedule();
}

void PageClientOfPaintServer::did_change_top_level_origin(URL::Origin const& new_origin)
{
    if (m_last_top_level_origin.has_value() && *m_last_top_level_origin == new_origin)
        return;

    m_last_top_level_origin = new_origin;
    Web::Painting::DisplayListSubmitter::reset_resources_for_page(m_page_client.id());
    m_pending_gpu_resource_reset = true;
    if (try_reset_submitter())
        m_pending_gpu_resource_reset = false;
}

PaintServer::ArenaSubmitter& PageClientOfPaintServer::ensure_submitter()
{
    if (!m_gpu_arena_packet_submitter) {
        VERIFY(m_gpu_surface_id.has_value());
        m_gpu_arena_packet_submitter = make<PaintServer::ArenaSubmitter>();
    }
    return *m_gpu_arena_packet_submitter;
}

bool PageClientOfPaintServer::try_reset_submitter()
{
    if (!m_gpu_arena_packet_submitter)
        return false;

    auto* gpu_render_client = m_page_client.client().paint_server_render_client();
    if (!gpu_render_client)
        return false;

    m_gpu_arena_packet_submitter->reset_all(*gpu_render_client);
    return true;
}

DisplayListStreamer::DisplayListStreamer(PaintServer::ArenaSubmitter& submitter, PaintServer::RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, PaintServer::SyncToken wait_sync_token, bool clear_snapshot_logging_when_done)
    : submitter(submitter)
    , render_client(render_client)
    , surface_id(surface_id)
    , wait_sync_token(wait_sync_token)
    , clear_snapshot_logging_when_done(clear_snapshot_logging_when_done)
{
}

DisplayListStreamer::~DisplayListStreamer()
{
    clear_snapshot_logging_if_needed();
}

ErrorOr<void> DisplayListStreamer::append_payload(size_t offset, ReadonlyBytes bytes)
{
    if (bytes.is_empty())
        return {};

    if (offset > NumericLimits<size_t>::max() - bytes.size())
        return Error::from_string_literal("DisplayListStreamer payload offset overflow");

    size_t const required_size = offset + bytes.size();
    if (required_size > payload.size())
        TRY(payload.try_resize(required_size, ByteBuffer::ZeroFillNewElements::Yes));
    bytes.copy_to(payload.bytes().slice(offset, bytes.size()));
    return {};
}

void DisplayListStreamer::clear_snapshot_logging_if_needed()
{
    if (!exchange(clear_snapshot_logging_when_done, false))
        return;
    PaintServer::clear_log_frame();
}

}
