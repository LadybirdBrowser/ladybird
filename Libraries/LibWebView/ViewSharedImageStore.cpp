/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibPaintServer/BrokerOfPaintServer.h>
#include <LibWebView/Application.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/ViewSharedImageStore.h>

namespace WebView {

ViewSharedImageStore::ViewSharedImageStore(ViewImplementation& view)
    : m_view(view)
{
}

ViewSharedImageStore::~ViewSharedImageStore()
{
    if (auto* paint_server_broker_client = Application::paint_server_broker_client(); paint_server_broker_client)
        paint_server_broker_client->destroy_presentation_surface(m_view.view_id());
}

void ViewSharedImageStore::reset()
{
    m_pending_present_id.clear();
    m_pending_present_was_submitted = false;
    m_presentation_current_image_id.clear();
    m_presentation_current_frame_size = {};
}

Optional<ViewSharedImageStore::PresentableFrame> ViewSharedImageStore::presentable_frame() const
{
    if (m_pending_present_id.has_value() && m_presentation_current_image_id.has_value()) {
        u64 image_id = m_presentation_current_image_id.value();
        if (auto* paint_server_broker_client = Application::paint_server_broker_client(); paint_server_broker_client) {
            bool const has_pool_image = paint_server_broker_client->has_pool_image(m_view.view_id(), image_id);
            Optional<void*> platform_surface_handle = paint_server_broker_client->platform_surface_handle_for_image(m_view.view_id(), image_id);
            Gfx::IntSize const frame_size = m_presentation_current_frame_size;

            if (has_pool_image) {
                return PresentableFrame {
                    .source = PresentableFrame::Source::PresentationSurface,
                    .bitmap = nullptr,
                    .bitmap_size = frame_size,
                    .platform_surface_handle = platform_surface_handle,
                    .surface_size = frame_size,
                    .present_id = m_pending_present_id,
                    .image_id = image_id,
                };
            }
            warnln("presentable_frame missing gpu surface view={} present_id={} image_id={} has_pool_image={}",
                m_view.view_id(),
                m_pending_present_id.value(),
                image_id,
                has_pool_image);
        }
    }

    return {};
}

void ViewSharedImageStore::did_receive_presentation_frame(u64 present_id, u64 image_id, Gfx::IntSize frame_size)
{
    auto* paint_server_broker_client = Application::paint_server_broker_client();
    if (!paint_server_broker_client)
        return;

    if (!paint_server_broker_client->has_pool_image(m_view.view_id(), image_id)) {
        paint_server_broker_client->async_did_present_or_released(m_view.view_id(), present_id);
        return;
    }

    if (m_pending_present_id.has_value() && !m_pending_present_was_submitted)
        paint_server_broker_client->async_did_present_or_released(m_view.view_id(), m_pending_present_id.value());

    m_pending_present_id = present_id;
    m_pending_present_was_submitted = false;
    m_presentation_current_image_id = image_id;
    m_presentation_current_frame_size = frame_size;

    if (m_view.on_ready_to_paint)
        m_view.on_ready_to_paint();
}

void ViewSharedImageStore::did_submit_presentation_frame(u64 present_id)
{
    if (m_pending_present_id.has_value() && m_pending_present_id.value() == present_id)
        m_pending_present_was_submitted = true;
}

void ViewSharedImageStore::configure_presentation_surface(Gfx::IntSize size)
{
    auto* paint_server_broker_client = Application::paint_server_broker_client();
    if (!paint_server_broker_client)
        return;

    Gfx::IntSize const presentation_size { max(size.width(), 1), max(size.height(), 1) };
    paint_server_broker_client->async_create_presentation_surface(m_view.view_id(), presentation_size);
    ensure_presentation_buffers(presentation_size);
}

void ViewSharedImageStore::ensure_presentation_buffers(Gfx::IntSize size)
{
    auto* paint_server_broker_client = Application::paint_server_broker_client();
    if (!paint_server_broker_client)
        return;

    if (!m_presentation_current_frame_size.is_empty() && m_presentation_current_frame_size == size) {
        paint_server_broker_client->ensure_broker_owned_presentation_buffers(m_view.view_id(), size);
        return;
    }

    m_presentation_current_image_id.clear();
    m_pending_present_id.clear();

    paint_server_broker_client->ensure_broker_owned_presentation_buffers(m_view.view_id(), size);
}

Optional<LinuxDmaBufPresentationBuffer> ViewSharedImageStore::clone_linux_dmabuf_presentation_buffer(u64 image_id) const
{
    auto* paint_server_broker_client = Application::paint_server_broker_client();
    if (!paint_server_broker_client)
        return {};
    return paint_server_broker_client->clone_linux_dmabuf_presentation_buffer(m_view.view_id(), image_id);
}

RefPtr<Gfx::Bitmap const> ViewSharedImageStore::bitmap_for_presentation_image(u64 image_id) const
{
    auto* paint_server_broker_client = Application::paint_server_broker_client();
    if (!paint_server_broker_client)
        return nullptr;
    return paint_server_broker_client->bitmap_for_presentation_image(m_view.view_id(), image_id);
}

void ViewSharedImageStore::did_present_frame(u64 present_id)
{
    bool const is_current_pending_present = m_pending_present_id.has_value() && m_pending_present_id.value() == present_id;

    if (auto* paint_server_broker_client = Application::paint_server_broker_client(); paint_server_broker_client)
        paint_server_broker_client->async_did_present_or_released(m_view.view_id(), present_id);

    if (is_current_pending_present) {
        m_pending_present_id.clear();
        m_pending_present_was_submitted = false;
        m_presentation_current_image_id.clear();
    }
}

}
