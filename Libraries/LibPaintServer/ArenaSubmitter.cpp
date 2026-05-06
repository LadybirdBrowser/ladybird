/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibPaintServer/ArenaSubmitter.h>
#include <LibPaintServer/Debug.h>
#include <LibPaintServer/Policy.h>
#include <LibPaintServer/RenderClientOfPaintServer.h>
#include <LibPaintServer/Resource/SharedBlob.h>

namespace PaintServer {

ArenaSubmitter::~ArenaSubmitter() = default;

bool ArenaSubmitter::begin_streamed_draw_list(RenderClientOfPaintServer& render_client)
{
    if (u64 server_epoch = render_client.server_epoch(); server_epoch != m_last_seen_server_epoch) {
        m_last_seen_server_epoch = server_epoch;
        clear_registered_resources();
        m_arena_writer.reset(render_client);
    }
    if (!m_arena_writer.begin_packet()) {
        dbgevery(64, "dropped_submission"sv, "ArenaSubmitter: failed to begin arena frame packet");
        return false;
    }

    return true;
}

ErrorOr<void> ArenaSubmitter::write_streamed_draw_list_payload(size_t offset, ReadonlyBytes bytes)
{
    return m_arena_writer.write(sizeof(ArenaFramePacketPrefix) + offset, bytes);
}

Optional<ReadonlyBytes> ArenaSubmitter::read_streamed_draw_list_payload(size_t offset, size_t size) const
{
    return m_arena_writer.pending_slice(sizeof(ArenaFramePacketPrefix) + offset, size);
}

Optional<PaintServer::ReleaseToken> ArenaSubmitter::finish_streamed_draw_list(RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, FrameHeader const& header, PaintServer::SyncToken wait_sync_token, PaintServer::SyncToken render_wait_sync_token)
{
    ArenaFramePacketPrefix packet_prefix {
        .kind = ArenaPacketKind::Frame,
        .frame_header = header,
        .render_wait_sync_token = render_wait_sync_token,
    };

    auto finalize_result = [&]() -> ErrorOr<PaintServer::ReleaseToken> {
        TRY(m_arena_writer.write(0, ReadonlyBytes { reinterpret_cast<u8 const*>(&packet_prefix), sizeof(ArenaFramePacketPrefix) }));

        PaintServer::ReleaseToken const release_token = render_client.allocate_release_token();
        ReadonlyBytes expected_header_bytes { reinterpret_cast<u8 const*>(&packet_prefix), sizeof(ArenaFramePacketPrefix) };
        auto slice = m_arena_writer.finish_packet(render_client, release_token, expected_header_bytes);
        if (!slice.has_value()) {
            dbgln("ArenaSubmitter: dropped GPU draw list submission due to arena slice/header validation failure payload_generation={} payload_length={}",
                header.payload_generation,
                header.payload_length);
            return Error::from_string_literal("ArenaSubmitter failed to finalize frame");
        }

        render_client.submit_arena_packet(surface_id, slice.release_value(), wait_sync_token, release_token);
        return release_token;
    }();
    if (finalize_result.is_error()) {
        m_arena_writer.abort_packet();
        dbgln("ArenaSubmitter: failed to finalize arena frame {}", finalize_result.error());
        return {};
    }

    if (is_logging_enabled(LOG_INGRESS) && dbg_has_elapsed("arena_writer_dump"sv, 1000))
        m_arena_writer.dump();
    return finalize_result.release_value();
}

void ArenaSubmitter::abort_streamed_draw_list()
{
    m_arena_writer.abort_packet();
}

void ArenaSubmitter::reset_resources(RenderClientOfPaintServer& render_client)
{
    unregister_resources(render_client);
    clear_registered_resources();
}

void ArenaSubmitter::reset_all(RenderClientOfPaintServer& render_client)
{
    reset_resources(render_client);
    m_arena_writer.reset(render_client);
}

void ArenaSubmitter::clear_registered_resources()
{
    m_registered_resources.clear();
}

void ArenaSubmitter::did_server_ingest(PaintServer::ReleaseToken release_token)
{
    m_arena_writer.did_complete(release_token);
}

Optional<PaintServer::ReleaseToken> ArenaSubmitter::submit_draw_list(RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, FrameHeader const& header, ReadonlyBytes payload, PaintServer::SyncToken wait_sync_token, PaintServer::SyncToken render_wait_sync_token, Vector<Gfx::ResourceTransfer>&& resource_submissions)
{
    SyncToken const resource_sync_token = submit_resources(render_client, surface_id, move(resource_submissions));
    SyncToken const effective_wait_sync_token = max(wait_sync_token, resource_sync_token);

    if (!begin_streamed_draw_list(render_client))
        return {};

    u32 const computed_payload_length = static_cast<u32>(payload.size());
    if (computed_payload_length != header.payload_length) {
        abort_streamed_draw_list();
        dbgln("ArenaSubmitter: frame submission failed with bad payload length header={} computed={} delta={}",
            header.payload_length,
            computed_payload_length,
            computed_payload_length - header.payload_length);
        return {};
    }

    auto payload_write = write_streamed_draw_list_payload(0, payload);
    if (payload_write.is_error()) {
        abort_streamed_draw_list();
        dbgln("ArenaSubmitter: frame submission failed with error={}", payload_write.error());
        return {};
    }
    return finish_streamed_draw_list(render_client, surface_id, header, effective_wait_sync_token, render_wait_sync_token);
}

Optional<PaintServer::ReleaseToken> ArenaSubmitter::submit_canvas_draw_list(RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, ReadonlyBytes payload, PaintServer::SyncToken wait_sync_token, Vector<Gfx::ResourceTransfer>&& resource_submissions)
{
    SyncToken const resource_sync_token = submit_resources(render_client, surface_id, move(resource_submissions));
    SyncToken const effective_wait_sync_token = max(wait_sync_token, resource_sync_token);

    if (!begin_streamed_draw_list(render_client))
        return {};

    ArenaCanvasPacketPrefix packet_prefix {
        .kind = ArenaPacketKind::Canvas,
        .image_id = image_id,
        .size = size,
        .format = format,
        .payload_length = static_cast<u32>(payload.size()),
    };

    auto finalize_result = [&]() -> ErrorOr<PaintServer::ReleaseToken> {
        TRY(m_arena_writer.write(0, ReadonlyBytes { reinterpret_cast<u8 const*>(&packet_prefix), sizeof(ArenaCanvasPacketPrefix) }));
        TRY(m_arena_writer.write(sizeof(ArenaCanvasPacketPrefix), payload));

        PaintServer::ReleaseToken const release_token = render_client.allocate_release_token();
        ReadonlyBytes expected_header_bytes { reinterpret_cast<u8 const*>(&packet_prefix), sizeof(ArenaCanvasPacketPrefix) };
        auto slice = m_arena_writer.finish_packet(render_client, release_token, expected_header_bytes);
        if (!slice.has_value())
            return Error::from_string_literal("ArenaSubmitter failed to finalize canvas packet");

        render_client.submit_arena_packet(surface_id, slice.release_value(), effective_wait_sync_token, release_token);
        return release_token;
    }();
    if (finalize_result.is_error()) {
        m_arena_writer.abort_packet();
        dbgln("ArenaSubmitter: failed to finalize arena canvas packet {}", finalize_result.error());
        return {};
    }

    return finalize_result.release_value();
}

SyncToken ArenaSubmitter::submit_resources(RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, Vector<Gfx::ResourceTransfer>&& resource_submissions)
{
    SyncToken latest_resource_token = 0;
    for (auto& submission : resource_submissions) {
        if (submission.info.resource_id == 0)
            continue;

        switch (submission.info.resource_id.type()) {
        case Gfx::WireResourceType::Invalid:
            break;
        case Gfx::WireResourceType::SkTypeface:
        case Gfx::WireResourceType::LocalFont: {
            if (submission.bytes.is_empty())
                continue;

            ResourceID resource_id = submission.info.resource_id;
            auto& registered_resources = m_registered_resources.ensure(surface_id, [] { return HashTable<PaintServer::ResourceID> {}; });
            if (registered_resources.contains(resource_id))
                continue;

            Optional<PaintServer::ReleaseToken> resource_token;
            bool fallback_submitted = false;
            if (!fallback_to_shared_blob(submission))
                resource_token = submit_resource_packet(render_client, surface_id, submission);

            if (!resource_token.has_value()) {
                auto shared_blob_or_error = PaintServer::SharedBlob::create_from_bytes(submission.bytes);
                if (shared_blob_or_error.is_error())
                    continue;

                PaintServer::SharedBlob shared_blob = shared_blob_or_error.release_value();
                auto file_or_error = shared_blob.clone_file();
                if (file_or_error.is_error())
                    continue;

                resource_token = render_client.allocate_release_token();
                render_client.async_register_resource_from_shared_blob(surface_id, submission.info, file_or_error.release_value(), shared_blob.size(), resource_token.value());
                fallback_submitted = true;
            }

            if (!resource_token.has_value() && !fallback_submitted)
                continue;

            registered_resources.set(resource_id);
            if (resource_token.has_value())
                latest_resource_token = max(latest_resource_token, resource_token.value());
            break;
        }
        case Gfx::WireResourceType::Bitmap: {
            if (!submission.shared_image_payload)
                continue;

            ResourceID resource_id = submission.info.resource_id;
            auto& registered_resources = m_registered_resources.ensure(surface_id, [] { return HashTable<PaintServer::ResourceID> {}; });
            if (registered_resources.contains(resource_id))
                continue;

            ReleaseToken const resource_token = render_client.allocate_release_token();
            render_client.async_register_bitmap_resource_from_shared_image(surface_id, submission.info, *submission.shared_image_payload, resource_token);
            registered_resources.set(resource_id);
            latest_resource_token = max(latest_resource_token, resource_token);
            break;
        }
        }
    }

    return latest_resource_token;
}

Optional<PaintServer::ReleaseToken> ArenaSubmitter::submit_resource_packet(RenderClientOfPaintServer& render_client, PaintServer::SurfaceID surface_id, Gfx::ResourceTransfer const& submission)
{
    if (submission.bytes.size() > NumericLimits<u32>::max())
        return {};
    if (!m_arena_writer.begin_packet())
        return {};

    ArenaResourcePacket packet {
        .kind = ArenaPacketKind::Resource,
        .descriptor = submission.info,
        .data_size = static_cast<u32>(submission.bytes.size()),
    };

    auto write_result = [&]() -> ErrorOr<PaintServer::ReleaseToken> {
        TRY(m_arena_writer.write(0, ReadonlyBytes { reinterpret_cast<u8 const*>(&packet), sizeof(packet) }));
        TRY(m_arena_writer.write(sizeof(packet), submission.bytes));

        PaintServer::ReleaseToken const release_token = render_client.allocate_release_token();
        ReadonlyBytes expected_prefix { reinterpret_cast<u8 const*>(&packet), sizeof(packet) };
        auto slice = m_arena_writer.finish_packet(render_client, release_token, expected_prefix);
        if (!slice.has_value())
            return Error::from_string_literal("ArenaSubmitter failed to finalize resource packet");

        if (is_logging_enabled(LOG_INGRESS)) {
            dbgln("ArenaSubmitter: submit resource packet {} surface_id={} release_token={} data_size={} arena_id={} offset={} size={}",
                submission.info.resource_id,
                surface_id,
                release_token,
                submission.bytes.size(),
                slice->arena_id,
                slice->offset,
                slice->size);
        }

        render_client.submit_arena_packet(surface_id, slice.release_value(), 0, release_token);
        return release_token;
    }();

    if (write_result.is_error()) {
        m_arena_writer.abort_packet();
        return {};
    }

    return write_result.release_value();
}

void ArenaSubmitter::unregister_resources(RenderClientOfPaintServer& render_client)
{
    for (auto const& surface_it : m_registered_resources) {
        for (auto resource_id : surface_it.value)
            render_client.async_unregister_resource(surface_it.key, resource_id);
    }
}

bool ArenaSubmitter::fallback_to_shared_blob(Gfx::ResourceTransfer const& submission)
{
    return submission.bytes.size() + sizeof(ArenaResourcePacket) > MIN_SUBMIT_ARENA_CAPACITY;
}

void ArenaSubmitter::did_fail_resource(PaintServer::SurfaceID surface_id, PaintServer::ResourceID resource_id, RenderClientOfPaintServer& render_client)
{
    render_client.async_unregister_resource(surface_id, resource_id);

    if (auto resources = m_registered_resources.get(surface_id); resources.has_value()) {
        resources->remove(resource_id);
        if (resources->is_empty())
            m_registered_resources.remove(surface_id);
    }
}

}
