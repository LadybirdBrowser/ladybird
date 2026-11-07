/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <UI/Qt/RemoteView.h>

namespace Ladybird {

static HashMap<u64, RemoteView*> s_views;

RemoteView::RemoteView(Hub::View::InitialState initial_state)
    : m_view(make<Hub::View>(*this, move(initial_state)))
{
    s_views.set(view_id(), this);
}

RemoteView::~RemoteView()
{
    s_views.remove(view_id());
    m_view->detach_from_host();
}

void RemoteView::did_install_backing_stores(u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    m_backing_stores_page_id = page_id;
    m_backing_stores.install(move(bitmap_ids), move(backing_stores));
}

void RemoteView::did_present_backing_store(u64 page_id, i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio)
{
    // NB: The Compositor may paint into the backing store this frame replaced as soon as it gets it back, so that must
    //     wait until nothing here reads from it anymore.
    auto result = m_backing_stores.present(bitmap_id, size, device_pixel_ratio);
    release_backing_store(page_id, result.bitmap_id_to_release);
}

void RemoteView::did_accept_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect)
{
    did_accept_presented_backing_store(bitmap_id, damage_rect);
    if (on_ready_to_paint)
        on_ready_to_paint();
}

void RemoteView::release_backing_store_to_page(u64 page_id, i32 bitmap_id)
{
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_release_backing_store(page_id, bitmap_id);
}

void RemoteView::enqueue_input_event(Web::InputEvent event)
{
    auto input_id = m_next_input_id++;
    event.visit(
        [&](Web::KeyEvent& event) {
            m_input_in_hub.set(input_id, move(event.browser_data));
            HubConnection::the().async_view_enqueue_key_event(view_id(), input_id, event.clone_without_browser_data());
        },
        [&](Web::MouseEvent& event) {
            m_input_in_hub.set(input_id, move(event.browser_data));
            HubConnection::the().async_view_enqueue_mouse_event(view_id(), input_id, event.clone_without_browser_data());
        },
        [&](Web::DragEvent& event) {
            m_input_in_hub.set(input_id, move(event.browser_data));
            auto event_to_send = event.clone_without_browser_data();
            event_to_send.files = move(event.files);
            HubConnection::the().async_view_enqueue_drag_event(view_id(), input_id, move(event_to_send));
        },
        [&](Web::PinchEvent& event) {
            HubConnection::the().async_view_enqueue_pinch_event(view_id(), event);
        });
}

void RemoteView::did_finish_handling_key_event(u64 input_id, Web::KeyEvent event)
{
    if (!on_finish_handling_key_event)
        return;
    if (auto browser_data = m_input_in_hub.get(input_id); browser_data.has_value())
        event.browser_data = m_input_in_hub.take(input_id).release_value();
    on_finish_handling_key_event(event);
}

void RemoteView::did_finish_handling_mouse_event(u64 input_id, Web::MouseEvent event, Web::EventResult result)
{
    if (!on_finish_handling_mouse_event)
        return;
    if (auto browser_data = m_input_in_hub.get(input_id); browser_data.has_value())
        event.browser_data = m_input_in_hub.take(input_id).release_value();
    on_finish_handling_mouse_event(event, result);
}

void RemoteView::did_finish_handling_drag_event(u64 input_id, Web::DragEvent event)
{
    if (!on_finish_handling_drag_event)
        return;
    if (auto browser_data = m_input_in_hub.get(input_id); browser_data.has_value())
        event.browser_data = m_input_in_hub.take(input_id).release_value();
    on_finish_handling_drag_event(event);
}

RemoteView* RemoteView::find(u64 view_id)
{
    return s_views.get(view_id).value_or(nullptr);
}

RemoteView& RemoteView::of(WebView::ViewImplementation const& view)
{
    return static_cast<RemoteView&>(*static_cast<Hub::View const&>(view).host());
}

}
