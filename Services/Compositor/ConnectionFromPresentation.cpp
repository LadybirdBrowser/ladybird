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
    m_presentation_id = m_compositor_state->register_presentation(*this, capabilities);
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

void ConnectionFromPresentation::post_message_to_presentation(IPC::MessageBuffer& buffer)
{
    (void)post_message(buffer);
}

ErrorOr<OwnPtr<IPC::MessageBuffer>> ConnectionFromPresentation::handle_raw(IPC::RawMessage& message)
{
    auto outcome = m_compositor_state->handle_message_from_presentation(m_presentation_id, message);
    if (outcome.misbehavior.has_value())
        did_misbehave(outcome.misbehavior->characters());
    return nullptr;
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

void ConnectionFromPresentation::set_scrollbar_track_press_jumps(bool jumps)
{
    m_compositor_state->set_scrollbar_track_press_jumps(*this, jumps);
}

}
