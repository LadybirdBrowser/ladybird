/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IDAllocator.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/RenderClient/RenderClientConnection.h>
#include <PaintServer/Server.h>
#include <math.h>

namespace PaintServer {

static IDAllocator s_client_ids;

static char const* packet_kind_name(ArenaPacketKind kind)
{
    switch (kind) {
    case ArenaPacketKind::Frame:
        return "Frame";
    case ArenaPacketKind::Resource:
        return "Resource";
    case ArenaPacketKind::Canvas:
        return "Canvas";
    }
    return "Unknown";
}

static Optional<u32> checked_positive_integral_to_u32(f32 value)
{
    if (!isfinite(value) || value <= 0.0f)
        return {};

    if (value > static_cast<f32>(NumericLimits<u32>::max()) || value != floorf(value))
        return {};

    return static_cast<u32>(value);
}

RenderClientConnection::RenderClientConnection(Server& server, NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ToRenderClientFromPaintServerEndpoint, ToPaintServerFromRenderClientEndpoint>(*this, move(transport), s_client_ids.allocate())
    , m_ingested_completions([this](SurfaceID surface_id, ReleaseToken release_token) {
        async_did_complete(surface_id, release_token, CompletionStatus::Ingested);
    })
    , m_render_completions([this](SurfaceID, ReleaseToken) {
        flush_pending_image_destroys();
    })
    , m_server(server)
{
    async_server_epoch_changed(m_server.server_epoch());
}

bool RenderClientConnection::are_sync_tokens_valid(SyncToken wait_sync_token, ReleaseToken release_token) const
{
    return wait_sync_token <= release_token
        && release_token > m_last_submitted_release_token;
}

void RenderClientConnection::reject_submit_at_ingress(SurfaceID surface_id, ReleaseToken release_token)
{
    m_last_submitted_release_token = max(m_last_submitted_release_token, release_token);
    enqueue_release_token(surface_id, release_token);
    m_ingested_completions.complete_release_token(release_token);
    m_render_completions.complete_release_token(release_token);
    clear_log_frame(release_token);
}

void RenderClientConnection::enqueue_release_token(SurfaceID surface_id, ReleaseToken release_token)
{
    m_ingested_completions.enqueue_completion(surface_id, release_token);
    m_render_completions.enqueue_completion(surface_id, release_token);
}

void RenderClientConnection::maybe_request_frame_repaint_after_resource_resolution(SurfaceID surface_id)
{
    auto& frame_slots = m_surface_frames.ensure(surface_id, [] { return SurfaceFrameSlots {}; });

    if (frame_slots.repaint_request_pending)
        return;

    frame_slots.repaint_request_pending = true;
    async_request_frame_repaint(surface_id);
}

void RenderClientConnection::complete_resource_registration_at_ingress(SurfaceID surface_id, ResourceID resource_id, ReleaseToken release_token, bool success)
{
    if (!success)
        async_did_fail_resource_registration(surface_id, resource_id, release_token);
    m_ingested_completions.complete_release_token(release_token);
    m_render_completions.complete_release_token(release_token);
    if (success)
        maybe_request_frame_repaint_after_resource_resolution(surface_id);
    try_dispatch_frames();
}

void RenderClientConnection::did_create_content_image(RequestID request_id, bool success, ImageID image_id, Optional<Gfx::SharedImagePayload> content_image)
{
    async_did_create_content_image(request_id, success, image_id, content_image);
}

void RenderClientConnection::did_import_content_image(RequestID request_id, bool success, ImageID image_id)
{
    async_did_import_content_image(request_id, success, image_id);
}

void RenderClientConnection::clear_compositor_resources_for_surface(SurfaceID surface_id)
{
    m_compositor_font_cache.clear_surface(surface_id);
}

void RenderClientConnection::clear_compositor_resources()
{
    m_compositor_font_cache.clear();
}

void RenderClientConnection::die()
{
    if (!m_server.has_render_client(connection_id()))
        return;

    if (is_logging_enabled())
        dbgln("PaintServer::RenderClient::Connection: die connection_id={} peer_pid={} server_epoch={}", connection_id(), m_client_pid, m_server.server_epoch());

    m_resource_manager.reset();
    m_ingested_completions.reset();
    m_render_completions.reset();
    m_pending_image_destroys.clear();
    m_surface_frames.clear();
    m_last_submitted_release_token = 0;

    s_client_ids.deallocate(static_cast<int>(connection_id()));
    m_server.remove_render_client(*this);
}

void RenderClientConnection::hello(RequestID request_id, ClientKind client_kind, int client_pid, bool is_headless)
{
    (void)client_kind;
    if (client_pid > 0)
        m_client_pid = client_pid;
    m_is_headless = is_headless;

    async_did_hello(request_id, getpid(), m_server.server_epoch(), 0);
}

void RenderClientConnection::create_content_image(RequestID request_id, GPUEpoch server_epoch, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format)
{
    if (server_epoch != m_server.server_epoch() || image_id == 0) {
        async_did_create_content_image(request_id, false, image_id, {});
        return;
    }
    m_server.create_content_image_for_client(connection_id(), request_id, image_id, size, format);
}

void RenderClientConnection::import_content_image(RequestID request_id, GPUEpoch server_epoch, ImageID image_id, Gfx::SharedImagePayload content_image)
{
    if (server_epoch != m_server.server_epoch() || image_id == 0) {
        async_did_import_content_image(request_id, false, image_id);
        return;
    }

    m_server.import_content_image_for_client(connection_id(), request_id, image_id, move(content_image));
}

void RenderClientConnection::complete_content_upload(GPUEpoch server_epoch, ImageID image_id, bool success)
{
    if (server_epoch != m_server.server_epoch())
        return;

    if (image_id == 0)
        return;

    m_server.complete_content_upload_for_client(connection_id(), image_id, success);
}

void RenderClientConnection::destroy_content_image(GPUEpoch server_epoch, ImageID image_id, SyncToken wait_sync_token)
{
    if (server_epoch != m_server.server_epoch())
        return;

    // Per-client content images retire against this connection's render-completion stream.
    // A future cross-client shared-image path would need a backend-visible shared fence.
    if (wait_sync_token <= m_render_completions.last_completed_release_token()) {
        m_server.destroy_content_image_for_client(connection_id(), image_id);
        return;
    }
    for (auto& pending : m_pending_image_destroys) {
        if (pending.image_id == image_id) {
            pending.wait_sync_token = AK::max(wait_sync_token, pending.wait_sync_token);
            return;
        }
    }
    m_pending_image_destroys.append(PendingImageDestroy {
        .image_id = image_id,
        .wait_sync_token = wait_sync_token,
    });
}

void RenderClientConnection::flush_pending_image_destroys()
{
    if (m_pending_image_destroys.is_empty())
        return;

    SyncToken const last_completed = m_render_completions.last_completed_release_token();
    size_t index = 0;
    while (index < m_pending_image_destroys.size()) {
        auto const pending = m_pending_image_destroys[index];
        if (pending.image_id == 0 || pending.wait_sync_token > last_completed) {
            ++index;
            continue;
        }
        m_server.destroy_content_image_for_client(connection_id(), pending.image_id);
        m_pending_image_destroys.remove(index);
    }
}

void RenderClientConnection::arm_next_frame_logging()
{
    PaintServer::set_log_next_frame();
}

bool RenderClientConnection::handle_frame_packet_at_ingress(SurfaceID surface_id, ReadonlyBytes packet_bytes, SyncToken wait_sync_token, ReleaseToken release_token)
{
    if (!are_sync_tokens_valid(wait_sync_token, release_token)) {
        if (is_logging_enabled())
            dbgln("Connection: rejecting submit_arena_packet frame with invalid sync tokens: wait_sync_token={} release_token={}", wait_sync_token, release_token);
        return false;
    }

    if (packet_bytes.size() < sizeof(ArenaFramePacketPrefix)) {
        dbgln("Connection: rejecting submit_arena_packet frame with truncated prefix: surface_id={} size={}", surface_id, packet_bytes.size());
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    auto const& frame_packet = *reinterpret_cast<ArenaFramePacketPrefix const*>(packet_bytes.data());
    auto const& header = frame_packet.frame_header;
    auto payload_bytes = packet_bytes.slice(sizeof(ArenaFramePacketPrefix));

    if (header.payload_length != payload_bytes.size()) {
        dbgln("Connection: rejecting submit_arena_packet frame with payload length mismatch: surface_id={} header_payload_length={} actual_payload_length={}", surface_id, header.payload_length, payload_bytes.size());
        fail_offscreen_render(surface_id, header);
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    if (m_is_headless && header.output_type != FrameOutputType::Offscreen) {
        m_last_submitted_release_token = release_token;
        enqueue_release_token(surface_id, release_token);
        m_ingested_completions.complete_release_token(release_token);
        m_render_completions.complete_release_token(release_token);
        return true;
    }

    if (!m_server.has_presentation_surface(surface_id)) {
        dbgln("Connection: rejecting submit_arena_packet frame for unknown surface_id={}", surface_id);
        fail_offscreen_render(surface_id, header);
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    set_log_next_frame(release_token);

    if (is_logging_enabled(LOG_INGRESS)) {
        dbgln("Connection: decoded frame packet surface_id={} release_token={} wait_sync_token={} packet_size={} payload_generation={} header_payload_length={} actual_payload_length={} viewport={}x{} submission_timestamp_ms={}",
            surface_id,
            release_token,
            wait_sync_token,
            packet_bytes.size(),
            header.payload_generation,
            header.payload_length,
            payload_bytes.size(),
            header.viewport_size.width(),
            header.viewport_size.height(),
            header.webcontent_submission_timestamp_ms);
    }

    if (header.viewport_size.is_empty()) {
        dbgln("Connection: rejecting submit_arena_packet frame with empty viewport size: surface_id={} payload_size={}", surface_id, payload_bytes.size());
        fail_offscreen_render(surface_id, header);
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    auto decoded_width = checked_positive_integral_to_u32(header.viewport_size.width());
    if (!decoded_width.has_value()) {
        dbgln("Connection: rejecting submit_arena_packet frame with invalid viewport width: surface_id={} payload_size={}", surface_id, payload_bytes.size());
        fail_offscreen_render(surface_id, header);
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    auto decoded_height = checked_positive_integral_to_u32(header.viewport_size.height());
    if (!decoded_height.has_value()) {
        dbgln("Connection: rejecting submit_arena_packet frame with invalid viewport height: surface_id={} payload_size={}", surface_id, payload_bytes.size());
        fail_offscreen_render(surface_id, header);
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    if (header.output_type == FrameOutputType::Presentation
        && !m_server.presentation_surface_dimensions_match(surface_id, { static_cast<int>(decoded_width.value()), static_cast<int>(decoded_height.value()) })) {
        dbgln("Connection: rejecting submit_arena_packet frame with mismatched presentation surface dimensions: surface_id={} decoded_width={} decoded_height={}", surface_id, decoded_width.value(), decoded_height.value());
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    if (!stage_frame_at_ingress(surface_id, header, payload_bytes, wait_sync_token, frame_packet.render_wait_sync_token, release_token)) {
        dbgln("Connection: rejecting submit_arena_packet frame with failed payload staging: surface_id={} release_token={}", surface_id, release_token);
        fail_offscreen_render(surface_id, header);
        reject_submit_at_ingress(surface_id, release_token);
        return true;
    }

    m_last_submitted_release_token = release_token;
    enqueue_release_token(surface_id, release_token);
    m_ingested_completions.complete_release_token(release_token);

    try_dispatch_frames();
    return true;
}

void RenderClientConnection::handle_resource_packet_at_ingress(SurfaceID surface_id, ReadonlyBytes packet_bytes, ReleaseToken release_token)
{
    if (packet_bytes.size() < sizeof(ArenaResourcePacket)) {
        dbgln("Connection: rejecting shared-arena resource packet with truncated prefix: surface_id={} release_token={} size={}", surface_id, release_token, packet_bytes.size());
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    auto const& packet = *reinterpret_cast<ArenaResourcePacket const*>(packet_bytes.data());
    if (packet.descriptor.resource_id == 0) {
        dbgln("Connection: rejecting shared-arena resource packet with invalid resource id: surface_id={} release_token={}", surface_id, release_token);
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    auto resource_bytes = packet_bytes.slice(sizeof(ArenaResourcePacket));

    if (is_logging_enabled(LOG_INGRESS)) {
        dbgln("Connection: decoded resource packet {} surface_id={} release_token={} packet_size={} declared_data_size={} actual_data_size={}",
            packet.descriptor.resource_id,
            surface_id,
            release_token,
            packet_bytes.size(),
            packet.data_size,
            resource_bytes.size());
    }

    if (packet.data_size != resource_bytes.size()) {
        dbgln("Connection: rejecting shared-arena resource packet with payload mismatch: surface_id={} release_token={} declared_size={} actual_size={}", surface_id, release_token, packet.data_size, resource_bytes.size());
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    auto copied_resource_bytes_or_error = ByteBuffer::copy(resource_bytes);
    if (copied_resource_bytes_or_error.is_error()) {
        dbgln("Connection: rejecting shared-arena resource packet because copying resource bytes failed: surface_id={} release_token={}", surface_id, release_token);
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    m_last_submitted_release_token = release_token;
    enqueue_release_token(surface_id, release_token);
    RefPtr<RenderClientConnection> protected_this { this };
    m_server.register_resource(connection_id(), surface_id, packet.descriptor, copied_resource_bytes_or_error.release_value(), [protected_this = move(protected_this), surface_id, release_token, resource_id = packet.descriptor.resource_id](bool success) {
        if (!success)
            protected_this->async_did_fail_resource_registration(surface_id, resource_id, release_token);
        protected_this->m_ingested_completions.complete_release_token(release_token);
        protected_this->m_render_completions.complete_release_token(release_token);
        if (success)
            protected_this->maybe_request_frame_repaint_after_resource_resolution(surface_id);
        protected_this->try_dispatch_frames();
    });
}

void RenderClientConnection::handle_canvas_packet_at_ingress(SurfaceID surface_id, ReadonlyBytes packet_bytes, SyncToken wait_sync_token, ReleaseToken release_token)
{
    if (!are_sync_tokens_valid(wait_sync_token, release_token)) {
        if (is_logging_enabled())
            dbgln("Connection: rejecting submit_arena_packet canvas with invalid sync tokens: wait_sync_token={} release_token={}", wait_sync_token, release_token);
        return;
    }

    if (packet_bytes.size() < sizeof(ArenaCanvasPacketPrefix)) {
        dbgln("Connection: rejecting shared-arena canvas packet with truncated prefix: surface_id={} release_token={} size={}", surface_id, release_token, packet_bytes.size());
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    auto const& packet = *reinterpret_cast<ArenaCanvasPacketPrefix const*>(packet_bytes.data());
    auto draw_list_payload = packet_bytes.slice(sizeof(ArenaCanvasPacketPrefix));

    if (packet.image_id == 0 || packet.size.is_empty()) {
        dbgln("Connection: rejecting shared-arena canvas packet with invalid target: surface_id={} release_token={} image_id={} size={}x{}", surface_id, release_token, packet.image_id, packet.size.width(), packet.size.height());
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    if (packet.payload_length != draw_list_payload.size()) {
        dbgln("Connection: rejecting shared-arena canvas packet with payload mismatch: surface_id={} release_token={} declared_size={} actual_size={}", surface_id, release_token, packet.payload_length, draw_list_payload.size());
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    auto copied_payload = ByteBuffer::copy(draw_list_payload);
    if (copied_payload.is_error()) {
        dbgln("Connection: rejecting shared-arena canvas packet because copying draw-list bytes failed: surface_id={} release_token={}", surface_id, release_token);
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    m_last_submitted_release_token = release_token;
    enqueue_release_token(surface_id, release_token);
    m_ingested_completions.complete_release_token(release_token);

    m_server.submit_canvas_draw_list_for_client_async(*this, surface_id, packet.image_id, packet.size, packet.format, copied_payload.release_value(), release_token);
}

void RenderClientConnection::submit_arena_packet(SurfaceID surface_id, ArenaID arena_id, size_t offset, size_t size, SyncToken wait_sync_token, ReleaseToken release_token)
{
    m_referenced_surface_ids.set(surface_id);

    if (release_token <= m_last_submitted_release_token) {
        dbgln("Connection: rejecting submit_arena_packet with non-monotonic release token: connection_id={} surface_id={} release_token={} last_submitted_release_token={} client_pid={}",
            connection_id(),
            surface_id,
            release_token,
            m_last_submitted_release_token,
            m_client_pid);
        return;
    }

    auto packet_bytes = m_resource_manager.arena_slice(arena_id, offset, size);
    if (!packet_bytes.has_value()) {
        dbgln("Connection: rejecting submit_arena_packet with invalid arena slice: arena_id={} offset={} size={}", arena_id, offset, size);
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    if (packet_bytes->is_empty()) {
        dbgln("Connection: rejecting submit_arena_packet with truncated header: surface_id={} release_token={} size={}", surface_id, release_token, packet_bytes->size());
        reject_submit_at_ingress(surface_id, release_token);
        return;
    }

    auto packet_kind = static_cast<ArenaPacketKind>(packet_bytes->data()[0]);

    if (is_logging_enabled(LOG_INGRESS)) {
        dbgln("Connection: submit_arena_packet decoded kind={} surface_id={} release_token={} arena_id={} offset={} size={}",
            packet_kind_name(packet_kind),
            surface_id,
            release_token,
            arena_id,
            offset,
            size);
    }

    switch (packet_kind) {
    case ArenaPacketKind::Frame:
        (void)handle_frame_packet_at_ingress(surface_id, *packet_bytes, wait_sync_token, release_token);
        break;
    case ArenaPacketKind::Resource:
        handle_resource_packet_at_ingress(surface_id, *packet_bytes, release_token);
        break;
    case ArenaPacketKind::Canvas:
        handle_canvas_packet_at_ingress(surface_id, *packet_bytes, wait_sync_token, release_token);
        break;
    default:
        dbgln("Connection: rejecting submit_arena_packet with unknown kind={} surface_id={} release_token={}", to_underlying(packet_kind), surface_id, release_token);
        reject_submit_at_ingress(surface_id, release_token);
        break;
    }
}

bool RenderClientConnection::wait_sync_token_has_been_ingested(SyncToken wait_sync_token) const
{
    return wait_sync_token <= m_ingested_completions.last_completed_release_token();
}

bool RenderClientConnection::wait_sync_token_has_been_rendered(SyncToken wait_sync_token) const
{
    return wait_sync_token <= m_render_completions.last_completed_release_token();
}

void RenderClientConnection::try_dispatch_frames()
{
    Vector<SurfaceID> surface_ids;
    surface_ids.ensure_capacity(m_surface_frames.size());
    for (auto const& it : m_surface_frames)
        surface_ids.append(it.key);

    for (SurfaceID surface_id : surface_ids)
        (void)try_dispatch_surface_frame(surface_id);
}

void RenderClientConnection::presentation_buffer_available(SurfaceID surface_id)
{
    auto it = m_surface_frames.find(surface_id);
    if (it == m_surface_frames.end())
        return;

    auto& frame_slots = it->value;
    if (frame_slots.render_frame)
        return;

    (void)try_dispatch_surface_frame(surface_id);
}

RenderClientConnection::FrameSubmitResult RenderClientConnection::submit_frame(SurfaceID surface_id, SurfaceFrameSlots& frame_slots, NonnullRefPtr<IngressFrame> frame)
{
    auto const submit_result = m_server.submit_frame_for_client_async(*this, surface_id, frame);
    if (submit_result == Server::SubmitFrameResult::Submitted) {
        frame_slots.render_frame = move(frame);
        return FrameSubmitResult::Submitted;
    }

    if (submit_result == Server::SubmitFrameResult::Blocked)
        return FrameSubmitResult::Blocked;

    fail_offscreen_render(surface_id, frame->header);
    did_complete_render(surface_id, frame->release_token, false);
    return FrameSubmitResult::Failed;
}

bool RenderClientConnection::try_dispatch_surface_frame(SurfaceID surface_id)
{
    // Post-completion redispatch can observe that the surface entry was already removed.
    auto it = m_surface_frames.find(surface_id);
    if (it == m_surface_frames.end() || !m_server.has_presentation_surface(surface_id))
        return false;
    auto& frame_slots = it->value;
    if (frame_slots.render_frame)
        return false;

    bool const offscreen_ready = !frame_slots.offscreen_frames.is_empty()
        && wait_sync_token_has_been_ingested(frame_slots.offscreen_frames.first()->wait_sync_token)
        && wait_sync_token_has_been_rendered(frame_slots.offscreen_frames.first()->render_wait_sync_token);
    bool const presentation_ready = frame_slots.presentation_frame
        && wait_sync_token_has_been_ingested(frame_slots.presentation_frame->wait_sync_token)
        && wait_sync_token_has_been_rendered(frame_slots.presentation_frame->render_wait_sync_token);
    if (!offscreen_ready && !presentation_ready)
        return false;

    if (presentation_ready && (!offscreen_ready || frame_slots.presentation_frame->release_token <= frame_slots.offscreen_frames.first()->release_token)) {
        auto frame = frame_slots.presentation_frame.release_nonnull();
        auto result = submit_frame(surface_id, frame_slots, frame);
        if (result == FrameSubmitResult::Blocked)
            frame_slots.presentation_frame = move(frame);
        return result == FrameSubmitResult::Submitted;
    }

    auto frame = frame_slots.offscreen_frames.take_first();
    return submit_frame(surface_id, frame_slots, frame) == FrameSubmitResult::Submitted;
}

void RenderClientConnection::did_complete_render(SurfaceID surface_id, ReleaseToken release_token, bool rendered)
{
    bool completed_submitted_frame = false;
    auto it = m_surface_frames.find(surface_id);
    if (it != m_surface_frames.end()) {
        auto& frame_slots = it->value;
        if (frame_slots.render_frame && frame_slots.render_frame->release_token == release_token) {
            frame_slots.render_frame = nullptr;
            completed_submitted_frame = true;
        }
        if (is_logging_enabled(LOG_INGRESS)) {
            dbgln("Connection: did_complete_render surface_id={} release_token={} rendered={} has_presentation={} offscreen_queue={} has_render={} repaint_request_pending={}",
                surface_id,
                release_token,
                rendered,
                frame_slots.presentation_frame ? 1 : 0,
                frame_slots.offscreen_frames.size(),
                frame_slots.render_frame ? 1 : 0,
                frame_slots.repaint_request_pending);
        }

        if (!frame_slots.presentation_frame && frame_slots.offscreen_frames.is_empty() && !frame_slots.render_frame && !frame_slots.repaint_request_pending)
            m_surface_frames.remove(it);
    }

    if (rendered)
        async_did_complete(surface_id, release_token, CompletionStatus::Rendered);

    m_render_completions.complete_release_token(release_token);
    bool const dispatched_waiting_frame = try_dispatch_surface_frame(surface_id);

    if (rendered && !completed_submitted_frame && !dispatched_waiting_frame)
        maybe_request_frame_repaint_after_resource_resolution(surface_id);
}

void RenderClientConnection::clear_submit_state_for_surface(SurfaceID surface_id)
{
    auto frame_slots = m_surface_frames.take(surface_id);
    if (!frame_slots.has_value())
        return;

    fail_pending_frames_for_surface(surface_id, frame_slots.value());
}

bool RenderClientConnection::stage_frame_at_ingress(SurfaceID surface_id, FrameHeader const& header, ReadonlyBytes payload, SyncToken wait_sync_token, SyncToken render_wait_sync_token, ReleaseToken release_token)
{
    auto frame = adopt_ref(*new IngressFrame);
    if (frame->payload.try_resize(payload.size()).is_error())
        return false;

    payload.copy_to(frame->payload.bytes());
    frame->header = header;
    frame->wait_sync_token = wait_sync_token;
    frame->render_wait_sync_token = render_wait_sync_token;
    frame->release_token = release_token;

    auto& frame_slots = m_surface_frames.ensure(surface_id, [] { return SurfaceFrameSlots {}; });

    frame_slots.repaint_request_pending = false;
    if (header.output_type == FrameOutputType::Offscreen) {
        frame_slots.offscreen_frames.append(move(frame));
        return true;
    }

    if (frame_slots.presentation_frame)
        m_render_completions.complete_release_token(frame_slots.presentation_frame->release_token);
    frame_slots.presentation_frame = move(frame);
    return true;
}

void RenderClientConnection::fail_offscreen_render(SurfaceID surface_id, FrameHeader const& header)
{
    if (header.output_type != FrameOutputType::Offscreen)
        return;
    async_did_complete_offscreen_render(surface_id, header.offscreen_target.request_id, 0, {}, {});
}

void RenderClientConnection::fail_pending_frames_for_surface(SurfaceID surface_id, SurfaceFrameSlots& frame_slots)
{
    if (frame_slots.presentation_frame)
        m_render_completions.complete_release_token(frame_slots.presentation_frame->release_token);

    for (auto& frame : frame_slots.offscreen_frames) {
        fail_offscreen_render(surface_id, frame->header);
        m_render_completions.complete_release_token(frame->release_token);
    }
}

void RenderClientConnection::register_resource_from_shared_blob(SurfaceID surface_id, Gfx::ResourceInfo descriptor, IPC::File blob_handle, size_t blob_size, ReleaseToken release_token)
{
    m_referenced_surface_ids.set(surface_id);

    if (release_token <= m_last_submitted_release_token) {
        dbgln("Connection: rejecting register_resource_from_shared_blob with non-monotonic release token: connection_id={} surface_id={} release_token={} last_submitted_release_token={} client_pid={}",
            connection_id(),
            surface_id,
            release_token,
            m_last_submitted_release_token,
            m_client_pid);
        return;
    }

    m_last_submitted_release_token = release_token;
    enqueue_release_token(surface_id, release_token);
    m_resource_manager.register_resource_from_shared_blob(*this, surface_id, descriptor, move(blob_handle), blob_size, release_token);
}

void RenderClientConnection::register_bitmap_resource_from_shared_image(SurfaceID surface_id, Gfx::ResourceInfo descriptor, Gfx::SharedImagePayload image_payload, ReleaseToken release_token)
{
    m_referenced_surface_ids.set(surface_id);

    if (release_token <= m_last_submitted_release_token) {
        dbgln("Connection: rejecting register_bitmap_resource_from_shared_image with non-monotonic release token: connection_id={} surface_id={} release_token={} last_submitted_release_token={} client_pid={}",
            connection_id(),
            surface_id,
            release_token,
            m_last_submitted_release_token,
            m_client_pid);
        return;
    }

    m_last_submitted_release_token = release_token;
    enqueue_release_token(surface_id, release_token);

    RefPtr<RenderClientConnection> protected_this { this };
    (void)m_server.post_to_compositor_thread([protected_this = move(protected_this), surface_id, descriptor, image_payload = move(image_payload), release_token]() mutable {
        auto result = protected_this->resource_manager().register_bitmap_resource(protected_this->connection_id(), descriptor, move(image_payload));
        bool success = !result.is_error();

        protected_this->m_server.post_to_main_thread([protected_this = move(protected_this), surface_id, resource_id = descriptor.resource_id, release_token, success]() mutable {
            protected_this->complete_resource_registration_at_ingress(surface_id, resource_id, release_token, success);
        });
    });
}

void RenderClientConnection::unregister_resource(SurfaceID surface_id, ResourceID resource_id)
{
    m_referenced_surface_ids.set(surface_id);
    m_server.unregister_resource(connection_id(), surface_id, resource_id);
}

}
