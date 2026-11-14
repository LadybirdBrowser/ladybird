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

    virtual PresentationId presentation_id() const override { return m_presentation_id; }
    virtual void post_message_to_presentation(IPC::MessageBuffer&) override;
    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) override;
    virtual void did_hold_synchronized_resize_frame(u64 remote_layer_id, u64 token) override { async_did_hold_synchronized_resize_frame(remote_layer_id, token); }
    virtual void did_commit_synchronized_resize(u64 remote_layer_id, u64 token) override { async_did_commit_synchronized_resize(remote_layer_id, token); }

    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_raw(IPC::RawMessage&) override;
    virtual void presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id) override;
    virtual void take_frame_snapshot(Web::CompositorContextId, u64 request_id, Optional<Gfx::IntSize> maximum_size) override;
    virtual void set_client_gpu_presentation_capability(bool supported, u64 adapter_luid) override;
    virtual void set_scrollbar_track_press_jumps(bool jumps) override;

    bool is_bound_to(Web::CompositorContextId) const;

    NonnullRefPtr<CompositorState> m_compositor_state;
    PresentationId m_presentation_id;
    Function<void()> m_on_death;
};

}
