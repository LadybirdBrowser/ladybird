/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImage.h>
#include <LibPaintServer/BrokerOfPaintServer.h>
#include <LibPaintServer/Policy.h>
#include <LibPaintServer/Presentation.h>

namespace WebView {

class MetalPresentation final : public Presentation {
public:
    virtual void ensure_broker_owned_presentation_buffers(BrokerOfPaintServer& broker, PaintServer::SurfaceID surface_id, Gfx::IntSize requested_size) override
    {
        auto& surface_state = m_surfaces.ensure(surface_id, [] { return SurfaceState {}; });

        Gfx::IntSize buffer_size = PaintServer::presentation_buffer_capacity_for_size(requested_size);
        if (!surface_state.pool.is_empty() && surface_state.buffer_size.contains(requested_size))
            return;

        surface_state.pool.clear();
        surface_state.buffer_size = buffer_size;

        static Atomic<u64> s_next_presentation_image_id { 0x8000'0000'0000'0001ULL };

        Vector<u64> image_ids;
        Vector<Gfx::SharedImagePayload> image_payloads;
        image_ids.ensure_capacity(PaintServer::PRESENTATION_BUFFER_COUNT);
        image_payloads.ensure_capacity(PaintServer::PRESENTATION_BUFFER_COUNT);

        for (size_t i = 0; i < PaintServer::PRESENTATION_BUFFER_COUNT; ++i) {
            u64 image_id = s_next_presentation_image_id.fetch_add(1, MemoryOrder::memory_order_relaxed);
            if (image_id == 0)
                continue;

            auto shared_image = Gfx::SharedImage::create({
                .size = buffer_size,
                .pixel_format = Gfx::BitmapFormat::BGRA8888,
                .color_space = Gfx::BitmapColorSpace::SRGB,
                .alpha_type = Gfx::BitmapAlpha::Premultiplied,
                .origin = Gfx::BitmapOrigin::TopLeft,
            });
            auto image_payload = shared_image.export_payload();

            surface_state.pool.set(image_id, make<Gfx::SharedImage>(move(shared_image)));
            image_ids.append(image_id);
            image_payloads.append(move(image_payload));
        }

        if (image_ids.is_empty())
            return;

        broker.async_register_presentation_buffers(surface_id, move(image_ids), move(image_payloads), buffer_size);
    }

    virtual bool has_pool_image(PaintServer::SurfaceID surface_id, u64 image_id) const override
    {
        auto surface_it = m_surfaces.find(surface_id);
        if (surface_it == m_surfaces.end())
            return false;
        return surface_it->value.pool.contains(image_id);
    }

    virtual Optional<void*> platform_surface_handle_for_image(PaintServer::SurfaceID surface_id, u64 image_id) override
    {
        auto surface_it = m_surfaces.find(surface_id);
        if (surface_it == m_surfaces.end())
            return {};

        auto it = surface_it->value.pool.find(image_id);
        if (it != surface_it->value.pool.end())
            return it->value->platform_surface_handle();

        return {};
    }

    virtual RefPtr<Gfx::Bitmap const> bitmap_for_presentation_image(PaintServer::SurfaceID surface_id, u64 image_id) const override
    {
        auto surface_it = m_surfaces.find(surface_id);
        if (surface_it == m_surfaces.end())
            return nullptr;

        auto pool_it = surface_it->value.pool.find(image_id);
        if (pool_it != surface_it->value.pool.end())
            return pool_it->value->bitmap();

        return nullptr;
    }

    virtual Optional<LinuxDmaBufPresentationBuffer> clone_linux_dmabuf_presentation_buffer(PaintServer::SurfaceID surface_id, u64 image_id) const override
    {
        (void)surface_id;
        (void)image_id;
        return {};
    }

    virtual void clear_surface(PaintServer::SurfaceID surface_id) override
    {
        m_surfaces.remove(surface_id);
    }

private:
    struct SurfaceState {
        HashMap<u64, NonnullOwnPtr<Gfx::SharedImage>> pool;
        Gfx::IntSize buffer_size;
    };

    HashMap<PaintServer::SurfaceID, SurfaceState> m_surfaces;
};

OwnPtr<Presentation> create_presentation_backend()
{
    return make<MetalPresentation>();
}

}
