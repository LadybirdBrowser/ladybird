/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <Compositor/ConnectionFromPresentation.h>
#include <LibIPC/Transport.h>
#include <math.h>

namespace Compositor {

// NB: The IPC decoder already rejects event types, wheel delta precisions and scroll gesture phases that do not
//     exist, and pinch scales that are not finite. These check the rest of what the UI sends.

static bool is_valid_key_code(Web::UIEvents::KeyCode key)
{
    switch (key) {
#define __ENUMERATE_KEY_CODE(name, ui_name, code) case Web::UIEvents::Key_##name:
        ENUMERATE_KEY_CODES
#undef __ENUMERATE_KEY_CODE
        return true;
    }
    return false;
}

static bool is_valid_modifiers(Web::UIEvents::KeyModifier modifiers)
{
    // NB: KeyModifier has no fixed underlying type, so ~Mod_Mask is out of its range; mask the integers instead.
    return (to_underlying(modifiers) & ~to_underlying(Web::UIEvents::Mod_Mask)) == 0;
}

static bool is_valid_key_event(Web::KeyEvent const& event)
{
    return is_valid_key_code(event.key)
        && is_valid_modifiers(event.modifiers)
        && event.code_point <= 0x10ffff
        // Only the compositor sets this, on its way to WebContent.
        && !event.async_scroll_performed_default_action;
}

static bool is_valid_mouse_event(Web::MouseEvent const& event)
{
    // Wheel deltas are scrolled by as floats.
    auto is_valid_wheel_delta = [](double delta) {
        return isfinite(delta) && fabs(delta) <= NumericLimits<float>::max();
    };
    constexpr auto all_buttons = Web::UIEvents::Primary | Web::UIEvents::Secondary | Web::UIEvents::Middle | Web::UIEvents::Backward | Web::UIEvents::Forward;
    return first_is_one_of(event.button, Web::UIEvents::None, Web::UIEvents::Primary, Web::UIEvents::Secondary, Web::UIEvents::Middle, Web::UIEvents::Backward, Web::UIEvents::Forward)
        && (event.buttons & ~all_buttons) == 0
        && is_valid_modifiers(event.modifiers)
        && is_valid_wheel_delta(event.wheel_delta_x)
        && is_valid_wheel_delta(event.wheel_delta_y)
        && is_valid_wheel_delta(event.wheel_tick_120ths_x)
        && is_valid_wheel_delta(event.wheel_tick_120ths_y)
        && event.click_count >= 0
        // Only the compositor sets these, on its way to WebContent.
        && !event.async_scroll_performed_default_action
        && !event.scrollbar_dragged_by_compositor.has_value();
}

static bool is_valid_pinch_event(Web::PinchEvent const& event)
{
    return is_valid_modifiers(event.modifiers);
}

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

void ConnectionFromPresentation::did_dispatch_input_event(Web::CompositorContextId context_id, u64 event_id, bool async_scroll_performed_default_action)
{
    async_did_dispatch_input_event(context_id, event_id, async_scroll_performed_default_action);
}

void ConnectionFromPresentation::handle_and_dispatch_key_event(Web::CompositorContextId context_id, Web::KeyEvent event, bool allow_keyboard_scroll)
{
    if (!is_valid_key_event(event)) {
        did_misbehave("Invalid key event");
        return;
    }
    if (!is_bound_to(context_id)) {
        did_not_dispatch_input_event(context_id, event.id);
        return;
    }
    m_compositor_state->handle_and_dispatch_key_event(context_id, move(event), allow_keyboard_scroll);
}

void ConnectionFromPresentation::handle_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent event)
{
    if (!is_valid_pinch_event(event)) {
        did_misbehave("Invalid pinch event");
        return;
    }
    if (!is_bound_to(context_id))
        return;
    m_compositor_state->handle_pinch_event(context_id, event);
}

void ConnectionFromPresentation::handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event)
{
    if (!is_valid_mouse_event(event)) {
        did_misbehave("Invalid mouse event");
        return;
    }
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

void ConnectionFromPresentation::set_scrollbar_track_press_jumps(bool jumps)
{
    m_compositor_state->set_scrollbar_track_press_jumps(*this, jumps);
}

}
