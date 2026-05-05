/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibPaintServer/Debug.h>
#include <LibPaintServer/Policy.h>
#include <PaintServer/BrokerConnection.h>
#include <PaintServer/Painter.h>
#include <PaintServer/RenderClient/RenderClientConnection.h>
#include <PaintServer/RenderClient/ResourceManager.h>
#include <PaintServer/Server.h>

namespace PaintServer {

Server::Server(NonnullOwnPtr<Painter> surface_backend, u64 server_epoch)
    : m_surface_backend(move(surface_backend))
    , m_server_epoch(server_epoch)
    , m_main_event_loop(Core::EventLoop::current_weak())
    , m_compositor_worker("Compositor"sv)
{
    m_compositor_worker.start();
}

Server::~Server()
{
    m_compositor_worker.shutdown([this] {
        m_surface_backend->shutdown();
    });
}

void Server::create_broker_connection(NonnullOwnPtr<IPC::Transport> transport)
{
    auto connection = BrokerConnection::construct(*this, move(transport));
    set_broker_connection(*connection);
}

void Server::set_broker_connection(BrokerConnection& broker_connection)
{
    m_broker_connection = broker_connection;
}

void Server::clear_broker_connection(BrokerConnection& broker_connection)
{
    if (m_broker_connection.ptr() != &broker_connection)
        return;

    m_broker_connection = nullptr;

    Vector<ConnectionID> connection_ids;
    for (auto const& it : m_render_client_connections)
        connection_ids.append(it.key);

    for (ConnectionID connection_id : connection_ids) {
        auto connection = m_render_client_connections.get(connection_id);
        if (connection.has_value())
            remove_render_client(*connection.value());
    }

    m_render_client_connections.clear();

    m_compositor_worker.shutdown([this] {
        m_surface_backend->shutdown();
    });

    Core::EventLoop::current().quit(0);
}

void Server::post_to_main_thread(Function<void()> callback)
{
    if (!m_main_event_loop)
        return;
    auto event_loop = m_main_event_loop->take();
    if (!event_loop)
        return;
    event_loop->deferred_invoke(move(callback));
}

bool Server::post_to_compositor_thread(Function<void()> callback)
{
    bool posted = m_compositor_worker.post_task(move(callback));
    if (!posted && is_logging_enabled())
        dbgln("Server: failed to post task to compositor thread");
    return posted;
}

void Server::post_submit_result_to_main_thread(AsyncSubmitRequest request, Painter::SubmitResult result)
{
    post_to_main_thread([this, request = move(request), result = move(result)]() mutable {
        if (result.operation_result == Painter::OperationResult::Failed) {
            m_presentation_buffers.release_submit_reservation(request.release_token, request.reserved_presentation_buffer_id);
            clear_log_frame(request.release_token);
        } else if (request.frame->header.output_type == FrameOutputType::Presentation) {
            ImageID const image_id = request.reserved_presentation_buffer_id.value_or(request.surface_id);
            m_presentation_buffers.stamp_image(image_id, request.presentation_frame_size);
            notify_broker_frame_ready(request.surface_id, request.release_token, image_id);
        }

        auto connection = m_render_client_connections.get(request.connection_id);
        if (!connection.has_value() || !connection.value() || connection.value() != request.connection)
            return;

        if (request.frame->header.output_type == FrameOutputType::Offscreen) {
            ImageID offscreen_image_id = result.offscreen_image_id;
            if (offscreen_image_id == 0 && request.frame->header.offscreen_target.kind == OffscreenTargetKind::ContentImage)
                offscreen_image_id = request.frame->header.offscreen_target.image_id;
            connection.value()->async_did_complete_offscreen_render(request.surface_id, request.frame->header.offscreen_target.request_id, offscreen_image_id, result.offscreen_content_image, result.offscreen_bitmap);
        }

        connection.value()->did_complete_render(request.surface_id, request.release_token, result.operation_result == Painter::OperationResult::Completed);
    });
}

bool Server::connect_render_client(NonnullOwnPtr<IPC::Transport> transport)
{
    auto render_connection = RenderClientConnection::construct(*this, move(transport));

    if (is_logging_enabled())
        dbgln("PaintServer::Server: connected render client connection_id={} peer_pid={} server_epoch={}", render_connection->connection_id(), render_connection->client_pid(), server_epoch());

    m_render_client_connections.set(render_connection->connection_id(), render_connection);
    return true;
}

void Server::remove_render_client(RenderClientConnection& connection)
{
    ConnectionID const connection_id = connection.connection_id();

    if (is_logging_enabled())
        dbgln("Server: removing render client connection_id={} peer_pid={} server_epoch={}", connection_id, connection.client_pid(), server_epoch());

    auto maybe_connection = m_render_client_connections.get(connection_id);
    RefPtr<RenderClientConnection> protected_connection = maybe_connection.has_value() ? maybe_connection.value() : nullptr;

    (void)post_to_compositor_thread([this, protected_connection] mutable {
        if (protected_connection) {
            protected_connection->m_pending_image_destroys.clear();
            protected_connection->resource_manager().destroy_all_images();
        }
        if (protected_connection)
            protected_connection->clear_compositor_resources();
        post_to_main_thread([protected_connection = move(protected_connection)] {
        });
    });

    for (SurfaceID surface_id : connection.referenced_surface_ids())
        (void)post_to_compositor_thread([this, surface_id, protected_connection] mutable {
            m_surface_backend->clear_surface_cached_state(surface_id);
            if (protected_connection)
                protected_connection->clear_compositor_resources_for_surface(surface_id);
            post_to_main_thread([protected_connection = move(protected_connection)] {
            });
        });

    m_render_client_connections.remove(connection_id);
}

bool Server::has_render_client(ConnectionID connection_id) const
{
    return m_render_client_connections.contains(connection_id);
}

bool Server::set_presentation_surface(SurfaceID surface_id, Gfx::IntSize size, bool require_existing_surface)
{
    if (size.is_empty()) {
        dbgln("Server: set_presentation_surface rejected invalid size id={} size={}x{}", surface_id, size.width(), size.height());
        return false;
    }

    Gfx::IntSize const logical_size = size;

    auto existing_record = m_surfaces.get(surface_id);
    if (require_existing_surface && !existing_record.has_value()) {
        dbgln("Server: resize_presentation_surface rejected unknown id={}", surface_id);
        return false;
    }

    Gfx::IntSize buffer_size = presentation_buffer_capacity_for_size(logical_size);
    if (existing_record.has_value()
        && !existing_record->configuration.buffer_size.is_empty()
        && existing_record->configuration.buffer_size.contains(logical_size)) {
        buffer_size = existing_record->configuration.buffer_size;
    }

    SurfaceConfiguration desired_configuration {
        .logical_size = logical_size,
        .buffer_size = buffer_size,
    };

    if (existing_record.has_value()) {
        if (existing_record->pending_configuration.has_value()) {
            auto const& pending_configuration = existing_record->pending_configuration->configuration;
            if (pending_configuration.logical_size == desired_configuration.logical_size
                && pending_configuration.buffer_size == desired_configuration.buffer_size) {
                return true;
            }
        } else if (existing_record->configuration.logical_size == desired_configuration.logical_size
            && existing_record->configuration.buffer_size == desired_configuration.buffer_size) {
            return true;
        }
    }

    if (is_logging_enabled()) {
        dbgln("Server: set_presentation_surface surface={} width={} height={}",
            surface_id,
            size.width(),
            size.height());
    }

    SurfaceRecord record = existing_record.value_or(SurfaceRecord {});
    u64 const generation = record.last_configuration_generation + 1;
    record.last_configuration_generation = generation;
    record.pending_configuration = SurfaceRecord::PendingConfiguration {
        .configuration = desired_configuration,
        .generation = generation,
    };

    bool posted = post_to_compositor_thread([this, surface_id, desired_configuration, generation] {
        bool success = m_surface_backend->create_surface(surface_id, desired_configuration.logical_size, desired_configuration.buffer_size);
        if (!success && is_logging_enabled()) {
            dbgln("Server: set_presentation_surface failed surface={} logical_size={} buffer_size={}", surface_id, desired_configuration.logical_size, desired_configuration.buffer_size);
        }
        post_to_main_thread([this, surface_id, generation, success] {
            did_complete_surface_configuration(surface_id, generation, success);
        });
    });

    if (!posted) {
        dbgln("Server: set_presentation_surface failed to post id={} size={}x{}", surface_id, size.width(), size.height());
        return false;
    }

    m_surfaces.set(surface_id, record);
    return true;
}

void Server::clear_presentation_buffers_for_surface(SurfaceID surface_id)
{
    m_presentation_buffers.clear_surface(surface_id);
    (void)post_to_compositor_thread([this, surface_id] {
        m_surface_backend->unregister_all_presentation_buffers_for_surface(surface_id);
    });
}

void Server::did_complete_surface_configuration(SurfaceID surface_id, u64 generation, bool success)
{
    auto maybe_record = m_surfaces.get(surface_id);
    if (!maybe_record.has_value() || !maybe_record->pending_configuration.has_value())
        return;

    auto const& pending_configuration = maybe_record->pending_configuration.value();
    if (pending_configuration.generation != generation)
        return;

    if (!success) {
        maybe_record->pending_configuration.clear();
        bool has_active_surface = !maybe_record->configuration.logical_size.is_empty();
        if (!has_active_surface) {
            clear_presentation_buffers_for_surface(surface_id);
            m_surfaces.remove(surface_id);
            return;
        }

        m_surfaces.set(surface_id, *maybe_record);
        return;
    }

    maybe_record->configuration = pending_configuration.configuration;
    maybe_record->pending_configuration.clear();
    m_surfaces.set(surface_id, *maybe_record);
}

void Server::notify_broker_frame_ready(SurfaceID surface_id, u64 present_id, ImageID image_id)
{
    clear_log_frame(present_id);

    if (!m_broker_connection)
        return;

    Gfx::IntSize frame_size;
    if (auto maybe_frame_size = m_presentation_buffers.stamped_frame_size_for_image(image_id); maybe_frame_size.has_value())
        frame_size = maybe_frame_size.value();
    else if (auto record = m_surfaces.get(surface_id); record.has_value())
        frame_size = record->configuration.logical_size;

    m_broker_connection->async_frame_ready(surface_id, present_id, image_id, frame_size);
}

void Server::did_present_or_released(SurfaceID surface_id, u64 present_id)
{
    auto const release_result = m_presentation_buffers.did_present_or_released(present_id);
    if (!release_result.had_mapping)
        return;

    for (auto const& it : m_render_client_connections) {
        if (!it.value)
            continue;
        if (!it.value->referenced_surface_ids().contains(surface_id))
            continue;
        it.value->presentation_buffer_available(surface_id);
    }
}

void Server::register_presentation_buffers(SurfaceID surface_id, Vector<ImageID> image_ids, Vector<Gfx::SharedImagePayload> image_payloads, Gfx::IntSize buffer_size)
{
    if (!has_presentation_surface(surface_id)) {
        dbgln("Server: register_presentation_buffers rejected unknown surface={} buffer_size={}", surface_id, buffer_size);
        return;
    }

    if (image_ids.size() != image_payloads.size()) {
        dbgln("Server: register_presentation_buffers mismatched arrays surface={} ids={} image_payloads={}", surface_id, image_ids.size(), image_payloads.size());
        return;
    }

    auto maybe_record = m_surfaces.get(surface_id);
    if (!maybe_record.has_value())
        return;

    SurfaceRecord record = maybe_record.release_value();
    u64 generation = ++record.presentation_buffer_generation;
    m_surfaces.set(surface_id, record);

    clear_presentation_buffers_for_surface(surface_id);

    for (size_t i = 0; i < image_ids.size(); ++i) {
        import_or_queue_presentation_buffer(surface_id, image_ids[i], move(image_payloads[i]), generation);
    }
}

void Server::did_import_presentation_buffer(SurfaceID surface_id, ImageID image_id, u64 generation)
{
    auto maybe_record = m_surfaces.get(surface_id);
    if (!maybe_record.has_value() || maybe_record->presentation_buffer_generation != generation) {
        (void)post_to_compositor_thread([this, image_id] {
            m_surface_backend->unregister_presentation_buffer_by_id(image_id);
        });
        return;
    }

    m_presentation_buffers.add_surface_buffer(surface_id, image_id);
}

void Server::import_or_queue_presentation_buffer(SurfaceID surface_id, ImageID image_id, Gfx::SharedImagePayload&& image_payload, u64 generation)
{
    auto maybe_record = m_surfaces.get(surface_id);
    if (!maybe_record.has_value() || maybe_record->presentation_buffer_generation != generation)
        return;

    (void)post_to_compositor_thread([this, surface_id, image_id, generation, image_payload = move(image_payload)]() mutable {
        auto result = m_surface_backend->register_presentation_buffer(surface_id, image_id, move(image_payload));
        if (result.is_error())
            dbgln("Server: register_presentation_buffer failed surface={} image_id={} error={}", surface_id, image_id, result.error());
        if (result.is_error())
            return;
        post_to_main_thread([this, surface_id, image_id, generation] {
            did_import_presentation_buffer(surface_id, image_id, generation);
        });
    });
}

void Server::set_presentation_surface_visible(SurfaceID surface_id, bool visible)
{
    auto record = m_surfaces.get(surface_id);
    if (!record.has_value())
        return;

    record->visible = visible;
    m_surfaces.set(surface_id, *record);
}

void Server::destroy_presentation_surface(SurfaceID surface_id)
{
    if (!m_surfaces.contains(surface_id))
        return;

    clear_presentation_buffers_for_surface(surface_id);
    for (auto const& it : m_render_client_connections) {
        if (!it.value)
            continue;
        it.value->clear_submit_state_for_surface(surface_id);
        auto protected_connection = it.value;
        (void)post_to_compositor_thread([this, protected_connection, surface_id] mutable {
            protected_connection->clear_compositor_resources_for_surface(surface_id);
            post_to_main_thread([protected_connection = move(protected_connection)] {
            });
        });
    }
    m_surfaces.remove(surface_id);
    (void)post_to_compositor_thread([this, surface_id] {
        m_surface_backend->destroy_surface(surface_id);
    });
}

bool Server::has_presentation_surface(SurfaceID surface_id) const
{
    return m_surfaces.contains(surface_id);
}

bool Server::presentation_surface_dimensions_match(SurfaceID surface_id, Gfx::IntSize frame_size) const
{
    auto surface_record = m_surfaces.get(surface_id);
    if (!surface_record.has_value())
        return false;

    // Ingress only needs to know whether the submitted frame still fits in the active backing
    // store. The frame's stamped size carries the actual drawn dimensions through presentation.
    if (!surface_record->configuration.buffer_size.is_empty() && surface_record->configuration.buffer_size.contains(frame_size))
        return true;

    if (surface_record->configuration.buffer_size.is_empty()
        && surface_record->pending_configuration.has_value()
        && surface_record->pending_configuration->configuration.buffer_size.contains(frame_size))
        return true;

    return false;
}

Server::SubmitFrameResult Server::submit_frame_for_client_async(RenderClientConnection& connection, SurfaceID surface_id, NonnullRefPtr<IngressFrame> frame)
{
    ConnectionID const connection_id = connection.connection_id();
    ReleaseToken const release_token = frame->release_token;
    FrameOutputType const output_type = frame->header.output_type;
    Gfx::IntSize const presentation_frame_size {
        static_cast<int>(frame->header.viewport_size.width()),
        static_cast<int>(frame->header.viewport_size.height()),
    };

    auto surface_record = m_surfaces.get(surface_id);
    if (!surface_record.has_value())
        return SubmitFrameResult::Failed;

    if (surface_record->configuration.logical_size.is_empty() && surface_record->pending_configuration.has_value())
        return SubmitFrameResult::Failed;

    Optional<u64> reserved_presentation_buffer_id;
    if (output_type == FrameOutputType::Presentation) {
        reserved_presentation_buffer_id = m_presentation_buffers.reserve_next_buffer(surface_id, release_token);
        if (!reserved_presentation_buffer_id.has_value()) {
            if (is_logging_enabled()) {
                dbgln("Server: submit_frame_for_client_async deferred surface={} release_token={} reason=no_reserved_presentation_buffer",
                    surface_id,
                    release_token);
            }
            return SubmitFrameResult::Blocked;
        }
    }

    AsyncSubmitRequest request {
        .connection = connection,
        .frame = move(frame),
        .connection_id = connection_id,
        .surface_id = surface_id,
        .release_token = release_token,
        .reserved_presentation_buffer_id = reserved_presentation_buffer_id,
        .presentation_frame_size = presentation_frame_size,
    };

    auto posted = post_to_compositor_thread([this, request = move(request)]() mutable {
        finish_submit_on_compositor_thread(request);
    });

    if (!posted) {
        m_presentation_buffers.release_submit_reservation(release_token, reserved_presentation_buffer_id);
        return SubmitFrameResult::Failed;
    }

    return SubmitFrameResult::Submitted;
}

void Server::finish_submit_on_compositor_thread(AsyncSubmitRequest const& request)
{
    Gfx::SharedImage* offscreen_content_image = nullptr;
    if (request.frame->header.output_type == FrameOutputType::Offscreen && request.frame->header.offscreen_target.kind == OffscreenTargetKind::ContentImage) {
        ImageID const image_id = request.frame->header.offscreen_target.image_id;
        if (image_id == 0) {
            post_submit_result_to_main_thread(request, Painter::SubmitResult {});
            return;
        }

        auto allocation_result = request.connection->resource_manager().allocate_content_image(image_id, request.presentation_frame_size, Gfx::BitmapFormat::BGRA8888, *m_surface_backend, request.connection->is_headless());
        if (allocation_result.is_error()) {
            post_submit_result_to_main_thread(request, Painter::SubmitResult {});
            return;
        }

        offscreen_content_image = request.connection->resource_manager().shared_image(image_id);
        if (!offscreen_content_image) {
            post_submit_result_to_main_thread(request, Painter::SubmitResult {});
            return;
        }
    }

    DrawImageResolver image_resolver = [this, connection = request.connection](ResourceID resource_id, ImageID image_id) {
        if (!connection)
            return sk_sp<SkImage> {};
        return connection->resource_manager().resolve_image(resource_id, image_id, *m_surface_backend, connection->is_headless());
    };
    DrawContext draw_context {
        .connection_id = request.connection_id,
        .surface_id = request.surface_id,
        .font_cache = request.connection->m_compositor_font_cache,
        .image_resolver = &image_resolver,
    };

    Painter::SubmitResult result = m_surface_backend->submit_commands(
        Painter::RenderContext {
            .draw_context = draw_context,
            .target_size = request.presentation_frame_size,
            .output_type = request.frame->header.output_type,
            .presentation_buffer_id = request.reserved_presentation_buffer_id,
            .offscreen_content_image = offscreen_content_image,
        },
        request.frame->header,
        request.frame->payload.bytes(),
        request.release_token);

    if (offscreen_content_image)
        request.connection->resource_manager().complete_image_upload(request.frame->header.offscreen_target.image_id, result.operation_result == Painter::OperationResult::Completed);

    post_submit_result_to_main_thread(request, move(result));
}

void Server::submit_canvas_draw_list_for_client_async(RenderClientConnection& connection, SurfaceID surface_id, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, ByteBuffer draw_list_payload, ReleaseToken release_token)
{
    RefPtr<RenderClientConnection> protected_connection { connection };
    auto posted = post_to_compositor_thread([this, protected_connection = move(protected_connection), surface_id, image_id, size, format, draw_list_payload = move(draw_list_payload), release_token]() mutable {
        bool success = render_canvas_draw_list_on_compositor_thread(*protected_connection, surface_id, image_id, size, format, draw_list_payload.bytes());
        post_to_main_thread([protected_connection = move(protected_connection), surface_id, release_token, success] {
            protected_connection->did_complete_render(surface_id, release_token, success);
        });
    });
    if (!posted)
        connection.did_complete_render(surface_id, release_token, false);
}

bool Server::render_canvas_draw_list_on_compositor_thread(RenderClientConnection& connection, SurfaceID surface_id, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format, ReadonlyBytes draw_list_payload)
{
    if (image_id == 0 || size.is_empty())
        return false;

    auto painting_surface = connection.resource_manager().canvas_painting_surface(image_id, size, format, *m_surface_backend, connection.is_headless());
    if (painting_surface.is_error())
        return false;

    DrawImageResolver image_resolver = [this, &connection](ResourceID resource_id, ImageID image_id) {
        return connection.resource_manager().resolve_image(resource_id, image_id, *m_surface_backend, connection.is_headless());
    };
    DrawContext draw_context {
        .connection_id = connection.connection_id(),
        .surface_id = surface_id,
        .font_cache = connection.m_compositor_font_cache,
        .image_resolver = &image_resolver,
    };

    auto paint_result = m_surface_backend->paint_draw_list_to_canvas(draw_context, painting_surface.release_value(), DrawListView { draw_list_payload });
    if (paint_result.is_error()) {
        dbgln("Server: canvas draw-list render failed image_id={} error={}", image_id, paint_result.error());
        return false;
    }

    connection.resource_manager().complete_image_upload(image_id, true);
    return true;
}

void Server::register_resource(ConnectionID connection_id, SurfaceID surface_id, Gfx::ResourceInfo descriptor, ByteBuffer resource_data, Function<void(bool success)> callback)
{
    auto maybe_connection = m_render_client_connections.get(connection_id);
    if (!maybe_connection.has_value() || !maybe_connection.value()) {
        if (callback)
            callback(false);
        return;
    }

    RefPtr<RenderClientConnection> connection = maybe_connection.value();

    (void)post_to_compositor_thread([this, connection = move(connection), connection_id, surface_id, descriptor, resource_data = move(resource_data), callback = move(callback)]() mutable {
        bool success = false;
        switch (descriptor.resource_id.type()) {
        case Gfx::WireResourceType::SkTypeface: {
            auto result = connection->m_compositor_font_cache.register_font(surface_id, descriptor.resource_id, resource_data.bytes(), descriptor.font.ttc_index);
            success = !result.is_error();
            if (!success)
                dbgln("Server: register failed {} connection_id={} surface_id={} error={}", descriptor.resource_id, connection_id, surface_id, result.error());
            break;
        }
        case Gfx::WireResourceType::LocalFont: {
            auto result = connection->m_compositor_font_cache.register_local_font(surface_id, descriptor.resource_id, resource_data.bytes());
            success = !result.is_error();
            if (!success)
                dbgln("Server: register failed {} connection_id={} surface_id={} error={}", descriptor.resource_id, connection_id, surface_id, result.error());
            break;
        }
        case Gfx::WireResourceType::Bitmap: {
            success = false;
            dbgln("Server: register rejected legacy bitmap byte resource {} connection_id={} surface_id={}", descriptor.resource_id, connection_id, surface_id);
            break;
        }
        case Gfx::WireResourceType::Invalid:
            success = false;
            dbgln("Server: register failed {} connection_id={} surface_id={}", descriptor.resource_id, connection_id, surface_id);
            break;
        }
        post_to_main_thread([connection = move(connection), callback = move(callback), success]() mutable {
            if (callback)
                callback(success);
        });
    });
}

void Server::unregister_resource(ConnectionID connection_id, SurfaceID surface_id, ResourceID resource_id, Function<void()> callback)
{
    auto maybe_connection = m_render_client_connections.get(connection_id);
    if (!maybe_connection.has_value() || !maybe_connection.value()) {
        if (callback)
            callback();
        return;
    }

    RefPtr<RenderClientConnection> connection = maybe_connection.value();

    (void)post_to_compositor_thread([this, connection = move(connection), connection_id, surface_id, resource_id, callback = move(callback)]() mutable {
        switch (resource_id.type()) {
        case Gfx::WireResourceType::SkTypeface:
        case Gfx::WireResourceType::LocalFont:
            connection->m_compositor_font_cache.unregister_font(surface_id, resource_id);
            break;
        case Gfx::WireResourceType::Bitmap:
            connection->resource_manager().unregister_bitmap_resource(resource_id);
            break;
        case Gfx::WireResourceType::Invalid:
            break;
        }

        if (is_logging_enabled(LOG_RESOURCE))
            dbgln("Server: unregistered {} connection_id={} surface_id={}", resource_id, connection_id, surface_id);

        post_to_main_thread([connection = move(connection), callback = move(callback)]() mutable {
            if (callback)
                callback();
        });
    });
}

void Server::create_content_image_for_client(ConnectionID connection_id, RequestID request_id, ImageID image_id, Gfx::IntSize size, Gfx::BitmapFormat format)
{
    auto maybe_connection = m_render_client_connections.get(connection_id);
    if (!maybe_connection.has_value() || !maybe_connection.value())
        return;

    RefPtr<RenderClientConnection> connection = maybe_connection.value();

    (void)post_to_compositor_thread([this, connection = move(connection), connection_id, request_id, image_id, size, format] {
        auto create_result = connection->resource_manager().allocate_content_image(image_id, size, format, *m_surface_backend, connection->is_headless());
        if (create_result.is_error()) {
            post_to_main_thread([this, connection_id, request_id] {
                auto connection = m_render_client_connections.get(connection_id);
                if (!connection.has_value() || !connection.value())
                    return;
                connection.value()->did_create_content_image(request_id, false, 0, {});
            });
            return;
        }

        auto content_image = create_result.release_value();
        post_to_main_thread([this, connection_id, request_id, image_id, content_image = move(content_image)]() mutable {
            auto connection = m_render_client_connections.get(connection_id);
            if (!connection.has_value() || !connection.value())
                return;

            connection.value()->did_create_content_image(request_id, true, image_id, move(content_image));
        });
    });
}

void Server::import_content_image_for_client(ConnectionID connection_id, RequestID request_id, ImageID image_id, Gfx::SharedImagePayload content_image)
{
    auto maybe_connection = m_render_client_connections.get(connection_id);
    if (!maybe_connection.has_value() || !maybe_connection.value())
        return;

    RefPtr<RenderClientConnection> connection = maybe_connection.value();

    (void)post_to_compositor_thread([this, connection = move(connection), connection_id, request_id, image_id, content_image = move(content_image)]() mutable {
        auto import_result = connection->resource_manager().import_content_image(image_id, move(content_image));
        if (import_result.is_error()) {
            post_to_main_thread([this, connection_id, request_id, image_id] {
                auto connection = m_render_client_connections.get(connection_id);
                if (!connection.has_value() || !connection.value())
                    return;
                connection.value()->async_did_import_content_image(request_id, false, image_id);
            });
            return;
        }

        post_to_main_thread([this, connection_id, request_id, image_id] {
            auto connection = m_render_client_connections.get(connection_id);
            if (!connection.has_value() || !connection.value())
                return;

            connection.value()->async_did_import_content_image(request_id, true, image_id);
        });
    });
}

void Server::complete_content_upload_for_client(ConnectionID connection_id, ImageID image_id, bool success)
{
    auto maybe_connection = m_render_client_connections.get(connection_id);
    if (!maybe_connection.has_value() || !maybe_connection.value())
        return;

    RefPtr<RenderClientConnection> connection = maybe_connection.value();

    (void)post_to_compositor_thread([this, connection = move(connection), image_id, success] mutable {
        connection->resource_manager().complete_image_upload(image_id, success);
        post_to_main_thread([connection = move(connection)]() mutable {
            (void)connection;
        });
    });
}

void Server::destroy_content_image_for_client(ConnectionID connection_id, ImageID image_id)
{
    auto maybe_connection = m_render_client_connections.get(connection_id);
    if (!maybe_connection.has_value() || !maybe_connection.value())
        return;

    RefPtr<RenderClientConnection> connection = maybe_connection.value();

    (void)post_to_compositor_thread([connection = move(connection), image_id] {
        connection->resource_manager().destroy_image(image_id);
    });
}

}
