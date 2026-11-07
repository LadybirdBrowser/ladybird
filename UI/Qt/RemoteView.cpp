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
