/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <Compositor/CompositorCore.h>
#include <LibCompositing/RustFFI.h>
#include <LibIPC/Message.h>
#include <LibIPC/RawAttachment.h>
#include <LibIPC/RawMessage.h>

namespace Compositor {

static_assert(IsTriviallyCopyable<Gfx::IntRect>);
static_assert(IsTriviallyCopyable<Gfx::IntSize>);
static_assert(IsTriviallyCopyable<Gfx::FloatPoint>);
static_assert(IsTriviallyCopyable<Gfx::FloatRect>);
static_assert(IsTriviallyCopyable<Gfx::Color>);
static_assert(IsTriviallyCopyable<Compositing::SpatialNodeIndex>);
static_assert(IsTriviallyCopyable<ReadonlyBytes>);

static void post_message_to_web_content(void* host, u64 client_id, ReadonlyBytes bytes, ReadonlySpan<IPC::RawAttachment> attachments)
{
    auto buffer = IPC::adopt_encoded_message(bytes, attachments);
    static_cast<CompositorCoreHost*>(host)->post_message_to_web_content(WebContentClientId { client_id }, buffer);
}

static void post_message_to_render_clock_of_web_content(void* host, u64 client_id, ReadonlyBytes bytes, ReadonlySpan<IPC::RawAttachment> attachments)
{
    auto buffer = IPC::adopt_encoded_message(bytes, attachments);
    static_cast<CompositorCoreHost*>(host)->post_message_to_render_clock_of_web_content(WebContentClientId { client_id }, buffer);
}

static void post_message_to_presentation(void* host, u64 presentation_id, ReadonlyBytes bytes, ReadonlySpan<IPC::RawAttachment> attachments)
{
    auto buffer = IPC::adopt_encoded_message(bytes, attachments);
    static_cast<CompositorCoreHost*>(host)->post_message_to_presentation(PresentationId { presentation_id }, buffer);
}

static void request_display_ticks(void* host, u64 context_id)
{
    static_cast<CompositorCoreHost*>(host)->request_display_ticks(Web::CompositorContextId { context_id });
}

static void set_timer(void* host, u64 context_id, ContextTimer timer, i64 delay_nanoseconds)
{
    static_cast<CompositorCoreHost*>(host)->set_timer(Web::CompositorContextId { context_id }, timer, AK::Duration::from_nanoseconds(delay_nanoseconds));
}

static void cancel_timer(void* host, u64 context_id, ContextTimer timer)
{
    static_cast<CompositorCoreHost*>(host)->cancel_timer(Web::CompositorContextId { context_id }, timer);
}

CompositorCore::CompositorCore(CompositorCoreHost& host, bool scrollbars_always_shown)
{
    RustFFI::FfiCompositorCoreHost ffi_host {
        .host = &host,
        .post_message_to_web_content = Compositor::post_message_to_web_content,
        .post_message_to_render_clock_of_web_content = Compositor::post_message_to_render_clock_of_web_content,
        .post_message_to_presentation = Compositor::post_message_to_presentation,
        .request_display_ticks = Compositor::request_display_ticks,
        .set_timer = Compositor::set_timer,
        .cancel_timer = Compositor::cancel_timer,
    };
    m_core = RustFFI::compositor_core_create(ffi_host, scrollbars_always_shown);
}

CompositorCore::~CompositorCore()
{
    RustFFI::compositor_core_destroy(m_core);
}

void CompositorCore::create_context(Web::CompositorContextId context_id, Optional<u64> page_id, WebContentClientId web_content_client_id)
{
    RustFFI::compositor_core_create_context(m_core, context_id.value(), page_id.has_value(), page_id.value_or(0), web_content_client_id.value());
}

void CompositorCore::destroy_context(Web::CompositorContextId context_id)
{
    RustFFI::compositor_core_destroy_context(m_core, context_id.value());
}

CompositorCore::ContextOwnerCheckResult CompositorCore::check_context_owner(Web::CompositorContextId context_id, WebContentClientId client) const
{
    return RustFFI::compositor_core_check_context_owner(m_core, context_id.value(), client.value());
}

void CompositorCore::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    RustFFI::compositor_core_set_parent_context(m_core, context_id.value(), parent_context_id.has_value(), parent_context_id.value_or({}).value());
}

void CompositorCore::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    RustFFI::compositor_core_stop_presenting_to_client(m_core, context_id.value());
}

void CompositorCore::set_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility, MonotonicTime now)
{
    RustFFI::compositor_core_set_visibility(m_core, context_id.value(), visibility == Compositing::ContextVisibility::Visible, now.nanoseconds());
}

void CompositorCore::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size)
{
    RustFFI::compositor_core_viewport_size_updated(m_core, context_id.value(), viewport_size);
}

void CompositorCore::set_paused_debugger_overlay_is_shown(Web::CompositorContextId context_id, bool is_shown)
{
    RustFFI::compositor_core_set_paused_debugger_overlay_is_shown(m_core, context_id.value(), is_shown);
}

void CompositorCore::set_bound_presentation(Web::CompositorContextId context_id, Optional<PresentationId> presentation_id)
{
    RustFFI::compositor_core_set_bound_presentation(m_core, context_id.value(), presentation_id.has_value(), presentation_id.value_or({}).value());
}

void CompositorCore::set_scroll_capabilities(Web::CompositorContextId context_id, ScrollCapabilities const& capabilities)
{
    RustFFI::compositor_core_set_scroll_capabilities(m_core, context_id.value(), capabilities);
}

bool CompositorCore::install_display_list_update(Web::CompositorContextId context_id, Compositing::DisplayList const& display_list, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>> const& async_scrolling_metadata, Compositing::AccumulatedVisualContextTree const& visual_context_tree, ReadonlyBytes encoded_scroll_state, MonotonicTime now)
{
    auto display_list_for_scroll_logic = display_list.for_scroll_logic(async_scrolling_metadata.bytes());
    return RustFFI::compositor_core_install_display_list_update(m_core, context_id.value(), &display_list_for_scroll_logic, visual_context_tree.rust_handle(), encoded_scroll_state, now.nanoseconds());
}

CompositorCore::VisualContextTreeUpdateResult CompositorCore::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree const& visual_context_tree, ReadonlyBytes encoded_scroll_state, IPC::Encoded<Compositing::KeyboardScrollState> const& keyboard_scroll_state, MonotonicTime now)
{
    return RustFFI::compositor_core_update_visual_context_tree(m_core, context_id.value(), visual_context_tree.rust_handle(), encoded_scroll_state, keyboard_scroll_state.bytes(), now.nanoseconds());
}

bool CompositorCore::needs_animation_frames(Web::CompositorContextId context_id, MonotonicTime now)
{
    return RustFFI::compositor_core_needs_animation_frames(m_core, context_id.value(), now.nanoseconds());
}

Optional<FrameInputs> CompositorCore::frame_inputs(Web::CompositorContextId context_id, MonotonicTime now)
{
    auto* frame_inputs = RustFFI::compositor_core_frame_inputs(m_core, context_id.value(), now.nanoseconds());
    if (!frame_inputs)
        return {};
    return FrameInputs::adopt(frame_inputs);
}

CompositorCore::ScreenshotInputs CompositorCore::frame_inputs_for_screenshot(Web::CompositorContextId context_id, MonotonicTime now)
{
    Gfx::IntRect scrollbar_fade_damage_rect;
    auto* frame_inputs = RustFFI::compositor_core_frame_inputs_for_screenshot(m_core, context_id.value(), now.nanoseconds(), &scrollbar_fade_damage_rect);
    return { .frame_inputs = FrameInputs::adopt(frame_inputs), .scrollbar_fade_damage_rect = scrollbar_fade_damage_rect };
}

DisplayTickResult CompositorCore::display_tick(Web::CompositorContextId context_id, MonotonicTime frame_time, MonotonicTime now, DisplayTickInputs const& inputs)
{
    return RustFFI::compositor_core_display_tick(
        m_core, context_id.value(), frame_time.nanoseconds(), now.nanoseconds(),
        {
            .context_is_on_tick_display = inputs.context_is_on_tick_display,
            .present_is_scheduled_on_tick_display = inputs.present_is_scheduled_on_tick_display,
            .ui_overlay_fades_need_frame = inputs.ui_overlay_fades_need_frame,
            .clock_tick_is_due = inputs.clock_tick_interval_in_milliseconds.has_value(),
            .clock_tick_interval_in_milliseconds = inputs.clock_tick_interval_in_milliseconds.value_or(0),
        });
}

void CompositorCore::timer_fired(Web::CompositorContextId context_id, ContextTimer timer, MonotonicTime now)
{
    RustFFI::compositor_core_timer_fired(m_core, context_id.value(), timer, now.nanoseconds());
}

static void receive_present_request(void* receiver, RustFFI::FfiPresentRequest request)
{
    Optional<Gfx::IntRect> viewport_rect;
    if (request.has_viewport_rect)
        viewport_rect = request.viewport_rect;
    static_cast<Vector<PresentRequest>*>(receiver)->append({ Web::CompositorContextId { request.context_id }, viewport_rect, request.forced_damage_rect, request.scheduling });
}

Vector<PresentRequest> CompositorCore::take_present_requests()
{
    Vector<PresentRequest> requests;
    RustFFI::compositor_core_take_present_requests(m_core, &requests, receive_present_request);
    return requests;
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_web_content(WebContentClientId client_id, IPC::RawMessage& message, MonotonicTime now)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_web_content_message(m_core, client_id.value(), raw_message, now.nanoseconds());
    });
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_presentation(PresentationId presentation_id, IPC::RawMessage& message, MonotonicTime now)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_presentation_message(m_core, presentation_id.value(), raw_message, now.nanoseconds());
    });
}

}
