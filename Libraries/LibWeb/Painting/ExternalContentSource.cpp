/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibPaintServer/Debug.h>
#include <LibPaintServer/RenderClientOfPaintServer.h>
#include <LibWeb/Painting/ExternalContentSource.h>

namespace Web::Painting {

static void track_timing(StringView key, Optional<Core::ElapsedTimer> const& timer)
{
    if (!timer.has_value())
        return;
    PaintServer::dbgtrack(key, static_cast<f32>(timer->elapsed_time().to_microseconds()) / 1000.f, 5000);
}

static Gfx::BitmapInfo bitmap_info_for_shared_image(Gfx::SharedImagePayload const& payload, Gfx::BitmapAlpha alpha_type)
{
    auto const& description = payload.info();
    return {
        .size = description.size,
        .row_bytes = payload.row_bytes(),
        .mip_level_count = 1,
        .sample_count = 1,
        .tiling_modifier = 0,
        .pixel_format = description.pixel_format,
        .color_space = description.color_space,
        .alpha_type = alpha_type,
        .origin = description.origin,
    };
}

static void notify_content_observers(Vector<GC::Root<GC::Function<void()>>> observers)
{
    if (observers.is_empty())
        return;
    Core::deferred_invoke([observers = move(observers)]() mutable {
        for (auto& observer : observers)
            observer->function()();
    });
}

NonnullRefPtr<ExternalContentSource> ExternalContentSource::create()
{
    return adopt_ref(*new ExternalContentSource());
}

ExternalContentSource::~ExternalContentSource()
{
    reset_image_state(true);
}

void ExternalContentSource::update(RefPtr<Gfx::DecodedImageFrame> frame)
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        m_frame = move(frame);

        auto* render_client = PaintServer::RenderClientOfPaintServer::get();
        synchronize_server_epoch_holding_lock(render_client);
        if (m_frame) {
            ensure_image_for_frame_holding_lock(render_client, *m_frame);
            m_needs_upload = m_content_image.has_value();
            if (pump_upload_holding_lock(render_client))
                observers = move(m_content_observers);
        }
        if (observers.is_empty())
            observers = take_content_observers_if_finalized_holding_lock();
    }
    notify_content_observers(move(observers));
}

void ExternalContentSource::set_pending_content()
{
    Threading::MutexLocker const locker { m_mutex };
    reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), true);
    m_waiting_for_allocation = true;
}

void ExternalContentSource::set_content_image(PaintServer::ImageID image_id, Gfx::SharedImagePayload content_image)
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), true);
        m_server_epoch = PaintServer::RenderClientOfPaintServer::get() ? PaintServer::RenderClientOfPaintServer::get()->server_epoch() : 0;
        m_image_id = image_id;
        m_bitmap_info = bitmap_info_for_shared_image(content_image, content_image.info().alpha_type);
        m_content_image = move(content_image);
        observers = take_content_observers_if_finalized_holding_lock();
    }
    notify_content_observers(move(observers));
}

bool ExternalContentSource::import_content_image(Gfx::SharedImagePayload content_image)
{
    Threading::MutexLocker const locker { m_mutex };

    auto const content_image_info = content_image.info();

    auto* render_client = PaintServer::RenderClientOfPaintServer::get();
    synchronize_server_epoch_holding_lock(render_client);
    if (!render_client)
        return false;

    if (m_image_id != 0 && !m_waiting_for_allocation && !m_needs_upload && !m_frame && !m_canvas_render_target && m_content_image.has_value()
        && m_content_image->info().size == content_image_info.size && m_content_image->info().pixel_format == content_image_info.pixel_format)
        return true;

    reset_image_state_holding_lock(render_client, true);

    auto shared_image = Gfx::SharedImage::import_from_payload(move(content_image));
    auto server_content_image = shared_image.export_payload();

    m_server_epoch = render_client->server_epoch();
    m_image_id = render_client->allocate_image_id();
    if (m_image_id == 0) {
        reset_image_state_holding_lock(render_client, false);
        return false;
    }

    m_waiting_for_allocation = true;
    m_pending_imported_content_image = shared_image.export_payload();

    RefPtr<ExternalContentSource> protected_this { this };
    PaintServer::ImageID const requested_image_id = m_image_id;
    render_client->import_content_image(requested_image_id, move(server_content_image), [protected_this = move(protected_this), requested_image_id](bool success) mutable {
        protected_this->did_import_content_image(requested_image_id, success);
    });
    return true;
}

Optional<PaintServer::ImageID> ExternalContentSource::ensure_canvas_render_target(Gfx::IntSize size, Gfx::BitmapFormat format)
{
    Threading::MutexLocker const locker { m_mutex };
    if (size.is_empty())
        return {};

    auto* render_client = PaintServer::RenderClientOfPaintServer::get();
    synchronize_server_epoch_holding_lock(render_client);
    if (!render_client)
        return {};

    m_frame = nullptr;
    m_shared_image = nullptr;
    m_painting_surface = nullptr;

    if (m_content_image.has_value() && (m_content_image->info().size != size || m_content_image->info().pixel_format != format))
        reset_image_state_holding_lock(render_client, true);

    m_canvas_render_target = true;

    if (m_content_image.has_value()) {
        m_bitmap_info = bitmap_info_for_shared_image(*m_content_image, m_content_image->info().alpha_type);
        return m_image_id;
    }

    if (m_image_id != 0 || m_waiting_for_allocation)
        return {};

    m_waiting_for_allocation = true;
    m_server_epoch = render_client->server_epoch();
    m_image_id = render_client->allocate_image_id();
    if (m_image_id == 0) {
        m_waiting_for_allocation = false;
        return {};
    }

    RefPtr<ExternalContentSource> protected_this { this };
    PaintServer::ImageID const requested_image_id = m_image_id;
    render_client->create_content_image(requested_image_id,
        size,
        format,
        [protected_this = move(protected_this), requested_image_id](PaintServer::RenderClientOfPaintServer::CreatedContentImage result) mutable {
            protected_this->did_create_content_image(requested_image_id, result.success, result.image_id, move(result.content_image));
        });
    return {};
}

void ExternalContentSource::did_submit_canvas_render()
{
    Threading::MutexLocker const locker { m_mutex };
    if (m_image_id != 0 && m_content_image.has_value())
        m_needs_upload = true;
}

void ExternalContentSource::did_complete_canvas_render(bool success)
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        if (!success || m_image_id == 0 || !m_content_image.has_value()) {
            reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), false);
            observers = move(m_content_observers);
        } else {
            m_needs_upload = false;
            observers = take_content_observers_if_finalized_holding_lock();
        }
    }
    notify_content_observers(move(observers));
}

RefPtr<Gfx::PaintingSurface> ExternalContentSource::painting_surface_for_size(Gfx::IntSize size, Gfx::BitmapFormat format)
{
    Threading::MutexLocker const locker { m_mutex };
    if (size.is_empty())
        return nullptr;

    auto* render_client = PaintServer::RenderClientOfPaintServer::get();
    synchronize_server_epoch_holding_lock(render_client);
    if (!render_client)
        return nullptr;

    if (m_content_image.has_value() && (m_content_image->info().size != size || m_content_image->info().pixel_format != format))
        reset_image_state_holding_lock(render_client, true);

    if (m_content_image.has_value() && !m_painting_surface)
        create_painting_surface_holding_lock();

    if (m_painting_surface)
        return m_painting_surface;

    if (m_image_id != 0 || m_waiting_for_allocation)
        return nullptr;

    m_waiting_for_allocation = true;
    m_server_epoch = render_client->server_epoch();
    m_image_id = render_client->allocate_image_id();
    if (m_image_id == 0) {
        m_waiting_for_allocation = false;
        return nullptr;
    }

    RefPtr<ExternalContentSource> protected_this { this };
    PaintServer::ImageID const requested_image_id = m_image_id;
    render_client->create_content_image(requested_image_id,
        size,
        format,
        [protected_this = move(protected_this), requested_image_id](PaintServer::RenderClientOfPaintServer::CreatedContentImage result) mutable {
            protected_this->did_create_content_image(requested_image_id, result.success, result.image_id, move(result.content_image));
        });
    return nullptr;
}

void ExternalContentSource::did_update_painting_surface_content()
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        if (!m_painting_surface || !m_shared_image || !m_content_image.has_value() || m_image_id == 0)
            return;

        m_painting_surface->flush();
        auto frame = Gfx::DecodedImageFrame::create(*m_shared_image->bitmap(), m_shared_image->color_space());
        auto* render_client = PaintServer::RenderClientOfPaintServer::get();
        bool uploaded = render_client && !Gfx::upload_decoded_image_frame_to_shared_image(*frame, *m_content_image).is_error();
        if (render_client)
            render_client->complete_content_upload(m_image_id, uploaded);
        if (!uploaded)
            reset_image_state_holding_lock(render_client, true);
        observers = move(m_content_observers);
    }
    notify_content_observers(move(observers));
}

void ExternalContentSource::clear()
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        m_frame = nullptr;
        reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), true);
        observers = take_content_observers_if_finalized_holding_lock();
    }
    notify_content_observers(move(observers));
}

Optional<PaintServer::ImageID> ExternalContentSource::current_image_id()
{
    Optional<PaintServer::ImageID> image_id;
    {
        Threading::MutexLocker const locker { m_mutex };
        auto* render_client = PaintServer::RenderClientOfPaintServer::get();
        synchronize_server_epoch_holding_lock(render_client);
        if (m_frame)
            ensure_image_for_frame_holding_lock(render_client, *m_frame);
        pump_upload_holding_lock(render_client);

        if (has_finalized_content_holding_lock() && m_image_id != 0)
            image_id = m_image_id;
    }
    return image_id;
}

bool ExternalContentSource::has_finalized_content()
{
    bool is_finalized = false;
    {
        Threading::MutexLocker const locker { m_mutex };
        auto* render_client = PaintServer::RenderClientOfPaintServer::get();
        synchronize_server_epoch_holding_lock(render_client);
        if (m_frame)
            ensure_image_for_frame_holding_lock(render_client, *m_frame);
        pump_upload_holding_lock(render_client);

        is_finalized = has_finalized_content_holding_lock();
    }
    return is_finalized;
}

void ExternalContentSource::when_content_is_finalized(ESCAPING GC::Root<GC::Function<void()>> observer)
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        auto* render_client = PaintServer::RenderClientOfPaintServer::get();
        synchronize_server_epoch_holding_lock(render_client);
        if (m_frame)
            ensure_image_for_frame_holding_lock(render_client, *m_frame);
        bool upload_failed = pump_upload_holding_lock(render_client);

        if (upload_failed || has_finalized_content_holding_lock())
            observers.append(move(observer));
        else
            m_content_observers.append(move(observer));
    }
    notify_content_observers(move(observers));
}

bool ExternalContentSource::has_finalized_content_holding_lock() const
{
    if (m_waiting_for_allocation)
        return false;
    if (!m_frame && !m_content_image.has_value())
        return true;
    return m_image_id != 0 && m_content_image.has_value() && !m_needs_upload;
}

Vector<GC::Root<GC::Function<void()>>> ExternalContentSource::take_content_observers_if_finalized_holding_lock()
{
    if (!has_finalized_content_holding_lock())
        return {};
    return move(m_content_observers);
}

void ExternalContentSource::create_painting_surface_holding_lock()
{
    VERIFY(m_content_image.has_value());
    auto content_image = m_content_image.release_value();
    m_shared_image = make<Gfx::SharedImage>(Gfx::SharedImage::import_from_payload(move(content_image)));
    m_content_image = m_shared_image->export_payload();
    m_bitmap_info = bitmap_info_for_shared_image(*m_content_image, m_shared_image->bitmap()->alpha_type());
    m_painting_surface = Gfx::PaintingSurface::wrap_bitmap(*m_shared_image->bitmap());
    m_frame = nullptr;
}

void ExternalContentSource::did_create_content_image(PaintServer::ImageID requested_image_id, bool success, PaintServer::ImageID image_id, Optional<Gfx::SharedImagePayload> content_image)
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        if (requested_image_id == 0 || requested_image_id != m_image_id)
            return;

        m_waiting_for_allocation = false;

        if (!success || image_id == 0 || image_id != requested_image_id || !content_image.has_value()) {
            reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), false);
            observers = move(m_content_observers);
        } else {
            m_content_image = move(content_image);
            if (m_canvas_render_target && !m_frame) {
                m_needs_upload = true;
                m_bitmap_info = bitmap_info_for_shared_image(*m_content_image, m_content_image->info().alpha_type);
                observers = move(m_content_observers);
            } else if (!m_frame) {
                create_painting_surface_holding_lock();
            } else {
                m_needs_upload = true;
                m_bitmap_info = bitmap_info_for_shared_image(*m_content_image, m_frame->bitmap().alpha_type());
            }

            if (observers.is_empty()) {
                if (pump_upload_holding_lock(PaintServer::RenderClientOfPaintServer::get()))
                    observers = move(m_content_observers);
                else
                    observers = take_content_observers_if_finalized_holding_lock();
            }
        }
    }
    notify_content_observers(move(observers));
}

void ExternalContentSource::did_import_content_image(PaintServer::ImageID requested_image_id, bool success)
{
    Vector<GC::Root<GC::Function<void()>>> observers;
    {
        Threading::MutexLocker const locker { m_mutex };
        if (requested_image_id == 0 || requested_image_id != m_image_id)
            return;

        m_waiting_for_allocation = false;

        if (!success || !m_pending_imported_content_image.has_value()) {
            m_pending_imported_content_image.clear();
            reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), false);
            observers = move(m_content_observers);
        } else {
            m_content_image = m_pending_imported_content_image.release_value();
            m_bitmap_info = bitmap_info_for_shared_image(*m_content_image, m_content_image->info().alpha_type);
            observers = take_content_observers_if_finalized_holding_lock();
        }
    }
    notify_content_observers(move(observers));
}

void ExternalContentSource::reset_image_state(bool destroy_server_image)
{
    Threading::MutexLocker const locker { m_mutex };
    reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer::get(), destroy_server_image);
}

void ExternalContentSource::reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer* render_client, bool destroy_server_image)
{
    if (destroy_server_image && render_client && m_image_id != 0 && m_server_epoch == render_client->server_epoch())
        render_client->destroy_content_image(m_image_id);

    m_server_epoch = render_client ? render_client->server_epoch() : 0;
    m_image_id = 0;
    m_frame = nullptr;
    m_shared_image = nullptr;
    m_painting_surface = nullptr;
    m_content_image.clear();
    m_pending_imported_content_image.clear();
    m_bitmap_info.clear();
    m_waiting_for_allocation = false;
    m_needs_upload = false;
    m_canvas_render_target = false;
}

void ExternalContentSource::synchronize_server_epoch_holding_lock(PaintServer::RenderClientOfPaintServer* render_client)
{
    PaintServer::GPUEpoch const current_epoch = render_client ? render_client->server_epoch() : 0;
    if (m_server_epoch == current_epoch)
        return;

    reset_image_state_holding_lock(render_client, false);
    m_server_epoch = current_epoch;
}

void ExternalContentSource::ensure_image_for_frame_holding_lock(PaintServer::RenderClientOfPaintServer* render_client, Gfx::DecodedImageFrame const& frame)
{
    if (!render_client)
        return;
    if (frame.width() <= 0 || frame.height() <= 0)
        return;

    if (m_image_id != 0 && m_content_image.has_value()) {
        if (m_content_image->info().size != frame.size())
            reset_image_state_holding_lock(render_client, true);
    }

    if (m_image_id != 0 || m_waiting_for_allocation)
        return;

    m_waiting_for_allocation = true;
    m_server_epoch = render_client->server_epoch();
    m_image_id = render_client->allocate_image_id();
    if (m_image_id == 0) {
        m_waiting_for_allocation = false;
        return;
    }

    RefPtr<ExternalContentSource> protected_this { this };
    PaintServer::ImageID const requested_image_id = m_image_id;
    render_client->create_content_image(requested_image_id,
        frame.size(),
        Gfx::BitmapFormat::BGRA8888,
        [protected_this = move(protected_this), requested_image_id](PaintServer::RenderClientOfPaintServer::CreatedContentImage result) mutable {
            protected_this->did_create_content_image(requested_image_id, result.success, result.image_id, move(result.content_image));
        });
}

bool ExternalContentSource::pump_upload_holding_lock(PaintServer::RenderClientOfPaintServer* render_client)
{
    if (!render_client || !m_frame || !m_content_image.has_value() || !m_needs_upload || m_image_id == 0)
        return false;

    Optional<Core::ElapsedTimer> upload_timer;
    if (PaintServer::is_logging_enabled(PaintServer::LOG_TIMING))
        upload_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto upload_result = Gfx::upload_decoded_image_frame_to_shared_image(*m_frame, *m_content_image);
    track_timing("content image upload ms"sv, upload_timer);
    bool const uploaded = !upload_result.is_error();

    render_client->complete_content_upload(m_image_id, uploaded);
    if (!uploaded) {
        reset_image_state_holding_lock(render_client, true);
        return true;
    }
    m_needs_upload = false;
    m_bitmap_info = bitmap_info_for_shared_image(*m_content_image, m_frame->bitmap().alpha_type());
    return false;
}

}
