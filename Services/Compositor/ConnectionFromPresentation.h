/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorPresentationClientEndpoint.h>
#include <Compositor/CompositorPresentationServerEndpoint.h>
#include <Compositor/CompositorState.h>
#include <LibIPC/ConnectionFromClient.h>

namespace Compositor {

// Carries the frames of the contexts bound to it, the release of their buffers and the input to them.
class ConnectionFromPresentation final
    : public IPC::ConnectionFromClient<CompositorPresentationClientEndpoint, CompositorPresentationServerEndpoint>
    , public CompositorStateClient {
    C_OBJECT(ConnectionFromPresentation);

public:
    virtual ~ConnectionFromPresentation() override;

    void set_on_death(Function<void()> on_death) { m_on_death = move(on_death); }

private:
    ConnectionFromPresentation(NonnullOwnPtr<IPC::Transport>, NonnullRefPtr<CompositorState>, CompositorState::PresentationCapabilities);

    virtual void die() override;

    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) override;
    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio) override;
    virtual void did_consume_input_event(Web::CompositorContextId, u64 event_id) override;
    virtual void did_not_dispatch_input_event(Web::CompositorContextId, u64 event_id) override;
    virtual void did_dispatch_input_event(Web::CompositorContextId, u64 event_id, bool async_scroll_performed_default_action) override;

    virtual void handle_and_dispatch_key_event(Web::CompositorContextId, Web::KeyEvent, bool allow_keyboard_scroll) override;
    virtual void handle_pinch_event(Web::CompositorContextId, Web::PinchEvent) override;
    virtual void handle_and_dispatch_mouse_event(Web::CompositorContextId, Web::MouseEvent) override;
    virtual void presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id) override;
    virtual void set_client_gpu_presentation_capability(bool supported, u64 adapter_luid) override;
    virtual void set_scrollbar_track_press_jumps(bool jumps) override;

    bool is_bound_to(Web::CompositorContextId) const;

    NonnullRefPtr<CompositorState> m_compositor_state;
    Function<void()> m_on_death;
};

}
