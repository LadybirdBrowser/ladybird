/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromPresentation.h>
#include <LibIPC/Transport.h>

namespace Compositor {

ConnectionFromPresentation::ConnectionFromPresentation(NonnullOwnPtr<IPC::Transport> transport, NonnullRefPtr<CompositorState> compositor_state, CompositorState::PresentationCapabilities capabilities)
    : IPC::ConnectionFromClient<CompositorPresentationClientEndpoint, CompositorPresentationServerEndpoint>(*this, move(transport), 1)
    , m_compositor_state(move(compositor_state))
{
    m_compositor_state->register_presentation(*this, capabilities);
}

ConnectionFromPresentation::~ConnectionFromPresentation()
{
    m_compositor_state->unregister_presentation(*this);
}

void ConnectionFromPresentation::die()
{
    auto protector = NonnullRefPtr { *this };
    m_compositor_state->unregister_presentation(*this);
    if (auto on_death = move(m_on_death))
        on_death();
}

bool ConnectionFromPresentation::is_bound_to(Web::CompositorContextId context_id) const
{
    return m_compositor_state->presentation_for_context(context_id) == this;
}

void ConnectionFromPresentation::did_allocate_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores)
{
    async_did_allocate_backing_stores(context_id, move(bitmap_ids), move(backing_stores));
}

void ConnectionFromPresentation::did_add_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores)
{
    async_did_add_backing_stores(context_id, move(bitmap_ids), move(backing_stores));
}

void ConnectionFromPresentation::did_retire_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids)
{
    async_did_retire_backing_stores(context_id, move(bitmap_ids));
}

void ConnectionFromPresentation::did_present_frame(Web::CompositorContextId context_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio)
{
    async_did_present_frame(context_id, content_rect, damage_rect, bitmap_id, device_pixel_ratio);
}

void ConnectionFromPresentation::did_consume_input_event(Web::CompositorContextId context_id, u64 event_id)
{
    async_did_consume_input_event(context_id, event_id);
}

void ConnectionFromPresentation::did_not_dispatch_input_event(Web::CompositorContextId context_id, u64 event_id)
{
    async_did_not_dispatch_input_event(context_id, event_id);
}

Messages::CompositorPresentationServer::HandleKeyEventResponse ConnectionFromPresentation::handle_key_event(Web::CompositorContextId context_id, Web::KeyEvent event)
{
    if (!is_bound_to(context_id))
        return false;
    return m_compositor_state->handle_key_event(context_id, event);
}

Messages::CompositorPresentationServer::DispatchKeyEventToWebContentResponse ConnectionFromPresentation::dispatch_key_event_to_web_content(Web::CompositorContextId context_id, Web::KeyEvent event)
{
    if (!is_bound_to(context_id))
        return false;
    return m_compositor_state->dispatch_key_event_to_web_content(context_id, event);
}

void ConnectionFromPresentation::handle_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent event)
{
    if (!is_bound_to(context_id))
        return;
    m_compositor_state->handle_pinch_event(context_id, event);
}

void ConnectionFromPresentation::handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event)
{
    if (!is_bound_to(context_id)) {
        did_not_dispatch_input_event(context_id, event.id);
        return;
    }
    m_compositor_state->handle_and_dispatch_mouse_event(context_id, move(event));
}

void ConnectionFromPresentation::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    if (!is_bound_to(context_id))
        return;
    m_compositor_state->presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void ConnectionFromPresentation::set_client_gpu_presentation_capability(bool supported, u64 adapter_luid)
{
    m_compositor_state->set_client_gpu_presentation_capability(*this, supported, adapter_luid);
}

}
