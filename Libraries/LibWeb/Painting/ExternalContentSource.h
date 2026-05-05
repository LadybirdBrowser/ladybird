/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibPaintServer/RenderClientOfPaintServer.h>
#include <LibPaintServer/Types.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>

namespace Web::Painting {

class WEB_API ExternalContentSource final : public AtomicRefCounted<ExternalContentSource> {
public:
    static NonnullRefPtr<ExternalContentSource> create();
    ~ExternalContentSource();

    void update(RefPtr<Gfx::DecodedImageFrame>);
    void set_pending_content();
    void set_content_image(PaintServer::ImageID, Gfx::SharedImagePayload);
    bool import_content_image(Gfx::SharedImagePayload);
    Optional<PaintServer::ImageID> ensure_canvas_render_target(Gfx::IntSize, Gfx::BitmapFormat);
    void did_submit_canvas_render();
    void did_complete_canvas_render(bool success);
    RefPtr<Gfx::PaintingSurface> painting_surface_for_size(Gfx::IntSize, Gfx::BitmapFormat);
    void did_update_painting_surface_content();
    void clear();
    Optional<PaintServer::ImageID> current_image_id();
    bool has_finalized_content();
    void when_content_is_finalized(ESCAPING GC::Root<GC::Function<void()>>);

private:
    ExternalContentSource() = default;

    bool has_finalized_content_holding_lock() const;
    Vector<GC::Root<GC::Function<void()>>> take_content_observers_if_finalized_holding_lock();
    void create_painting_surface_holding_lock();
    void did_create_content_image(PaintServer::ImageID requested_image_id, bool success, PaintServer::ImageID image_id, Optional<Gfx::SharedImagePayload>);
    void did_import_content_image(PaintServer::ImageID requested_image_id, bool success);
    void reset_image_state(bool destroy_server_image);
    void reset_image_state_holding_lock(PaintServer::RenderClientOfPaintServer*, bool destroy_server_image);
    void synchronize_server_epoch_holding_lock(PaintServer::RenderClientOfPaintServer*);
    void ensure_image_for_frame_holding_lock(PaintServer::RenderClientOfPaintServer*, Gfx::DecodedImageFrame const&);
    bool pump_upload_holding_lock(PaintServer::RenderClientOfPaintServer*);

    mutable Threading::Mutex m_mutex;
    RefPtr<Gfx::DecodedImageFrame> m_frame;
    OwnPtr<Gfx::SharedImage> m_shared_image;
    RefPtr<Gfx::PaintingSurface> m_painting_surface;
    PaintServer::GPUEpoch m_server_epoch { 0 };
    PaintServer::ImageID m_image_id { 0 };
    Optional<Gfx::SharedImagePayload> m_content_image;
    Optional<Gfx::SharedImagePayload> m_pending_imported_content_image;
    Optional<Gfx::BitmapInfo> m_bitmap_info;
    bool m_waiting_for_allocation { false };
    bool m_needs_upload { false };
    bool m_canvas_render_target { false };
    Vector<GC::Root<GC::Function<void()>>> m_content_observers;
};

}
