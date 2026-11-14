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

static void set_timer(void* host, u64 context_id, ContextTimer timer, i64 delay_nanoseconds)
{
    static_cast<CompositorCoreHost*>(host)->set_timer(Web::CompositorContextId { context_id }, timer, AK::Duration::from_nanoseconds(delay_nanoseconds));
}

static void cancel_timer(void* host, u64 context_id, ContextTimer timer)
{
    static_cast<CompositorCoreHost*>(host)->cancel_timer(Web::CompositorContextId { context_id }, timer);
}

static void shape_overlay_label(void* host, u64 context_id, bool has_font_family, ReadonlyBytes font_family, double device_pixel_ratio, ReadonlyBytes text, void* receiver, void (*receive)(void*, RustFFI::FfiOverlayLabel const*))
{
    Optional<StringView> font_family_view;
    if (has_font_family)
        font_family_view = StringView { font_family };
    auto label = static_cast<CompositorCoreHost*>(host)->shape_overlay_label(Web::CompositorContextId { context_id }, font_family_view, device_pixel_ratio, StringView { text });
    if (!label.has_value())
        return;
    RustFFI::FfiOverlayLabel ffi_label {
        .font_id = label->font_id,
        .glyph_ids = label->glyph_ids.span(),
        .glyph_x_positions = label->glyph_x_positions.span(),
        .width = label->width,
        .ascent = label->ascent,
        .descent = label->descent,
        .font_ascent = label->font_ascent,
    };
    receive(receiver, &ffi_label);
}

static MonotonicTime now()
{
    return MonotonicTime::now();
}

CompositorCore::CompositorCore(CompositorCoreHost& host, RustFFI::FfiRasterHost raster_host, bool scrollbars_always_shown, RustFFI::GpuBackend gpu)
{
    RustFFI::FfiCompositorCoreHost ffi_host {
        .host = &host,
        .post_message_to_web_content = Compositor::post_message_to_web_content,
        .post_message_to_render_clock_of_web_content = Compositor::post_message_to_render_clock_of_web_content,
        .post_message_to_presentation = Compositor::post_message_to_presentation,
        .set_timer = Compositor::set_timer,
        .cancel_timer = Compositor::cancel_timer,
        .shape_overlay_label = Compositor::shape_overlay_label,
    };
    m_core = RustFFI::compositor_core_create(ffi_host, raster_host, scrollbars_always_shown, gpu);
}

CompositorCore::~CompositorCore()
{
    RustFFI::compositor_core_destroy(m_core);
}

void CompositorCore::create_context(Web::CompositorContextId context_id, Optional<u64> page_id, WebContentClientId web_content_client_id)
{
    RustFFI::compositor_core_create_context(m_core, context_id.value(), page_id.has_value(), page_id.value_or(0), web_content_client_id.value(), now().nanoseconds());
}

void CompositorCore::destroy_context(Web::CompositorContextId context_id)
{
    RustFFI::compositor_core_destroy_context(m_core, context_id.value(), now().nanoseconds());
}

CompositorCore::ContextOwnerCheckResult CompositorCore::check_context_owner(Web::CompositorContextId context_id, WebContentClientId client) const
{
    return RustFFI::compositor_core_check_context_owner(m_core, context_id.value(), client.value());
}

bool CompositorCore::context_can_be_nested_in(Web::CompositorContextId context_id, Web::CompositorContextId parent_context_id) const
{
    return RustFFI::compositor_core_context_can_be_nested_in(m_core, context_id.value(), parent_context_id.value());
}

bool CompositorCore::presents_to_client(Web::CompositorContextId context_id) const
{
    return RustFFI::compositor_core_presents_to_client(m_core, context_id.value());
}

bool CompositorCore::context_is_effectively_visible(Web::CompositorContextId context_id) const
{
    return RustFFI::compositor_core_context_is_effectively_visible(m_core, context_id.value());
}

Optional<Web::CompositorContextId> CompositorCore::parent_context(Web::CompositorContextId context_id) const
{
    u64 parent_context_id = 0;
    if (!RustFFI::compositor_core_parent_context(m_core, context_id.value(), &parent_context_id))
        return {};
    return Web::CompositorContextId { parent_context_id };
}

Optional<PresentationId> CompositorCore::bound_presentation(Web::CompositorContextId context_id) const
{
    u64 presentation_id = 0;
    if (!RustFFI::compositor_core_bound_presentation(m_core, context_id.value(), &presentation_id))
        return {};
    return PresentationId { presentation_id };
}

void CompositorCore::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    RustFFI::compositor_core_set_parent_context(m_core, context_id.value(), parent_context_id.has_value(), parent_context_id.value_or({}).value(), now().nanoseconds());
}

void CompositorCore::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    RustFFI::compositor_core_stop_presenting_to_client(m_core, context_id.value(), now().nanoseconds());
}

bool CompositorCore::set_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    return RustFFI::compositor_core_set_visibility(m_core, context_id.value(), visibility == Compositing::ContextVisibility::Visible, now().nanoseconds());
}

void CompositorCore::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    RustFFI::compositor_core_viewport_size_updated(m_core, context_id.value(), viewport_size, device_pixel_ratio, window_resize_in_progress == Compositing::WindowResizingInProgress::Yes, now().nanoseconds());
}

void CompositorCore::set_ui_overlays(Web::CompositorContextId context_id, Compositing::UIOverlays const& overlays)
{
    auto const& paused_debugger = overlays.paused_debugger;
    auto const& picture_in_picture_controls = overlays.picture_in_picture_controls;
    auto font_family = paused_debugger.has_value() && paused_debugger->font_family.has_value() ? paused_debugger->font_family->bytes() : ReadonlyBytes {};
    RustFFI::FfiUIOverlays ffi_overlays {
        .has_paused_debugger = paused_debugger.has_value(),
        .paused_debugger_has_font_family = paused_debugger.has_value() && paused_debugger->font_family.has_value(),
        .paused_debugger_font_family = font_family,
        .paused_debugger_has_hovered_action = paused_debugger.has_value() && paused_debugger->hovered_action.has_value(),
        .paused_debugger_hovered_action = paused_debugger.has_value() && paused_debugger->hovered_action.has_value() ? to_underlying(*paused_debugger->hovered_action) : u8 { 0 },
        .has_picture_in_picture_controls = picture_in_picture_controls.has_value(),
        .picture_in_picture_controls_shown = picture_in_picture_controls.has_value() && picture_in_picture_controls->shown,
        .picture_in_picture_controls_has_hovered_control = picture_in_picture_controls.has_value() && picture_in_picture_controls->hovered_control.has_value(),
        .picture_in_picture_controls_hovered_control = picture_in_picture_controls.has_value() && picture_in_picture_controls->hovered_control.has_value() ? to_underlying(*picture_in_picture_controls->hovered_control) : u8 { 0 },
    };
    RustFFI::compositor_core_set_ui_overlays(m_core, context_id.value(), &ffi_overlays, now().nanoseconds());
}

void CompositorCore::register_presentation(PresentationId presentation_id, PresentationCapabilities const& capabilities)
{
    RustFFI::compositor_core_register_presentation(m_core, presentation_id.value(), &capabilities);
}

void CompositorCore::unregister_presentation(PresentationId presentation_id)
{
    RustFFI::compositor_core_unregister_presentation(m_core, presentation_id.value(), now().nanoseconds());
}

void CompositorCore::set_presentation_gpu_capability(PresentationId presentation_id, bool supported, u64 adapter_luid)
{
    RustFFI::compositor_core_set_presentation_gpu_capability(m_core, presentation_id.value(), supported, adapter_luid, now().nanoseconds());
}

void CompositorCore::set_scrollbar_track_press_jumps(PresentationId presentation_id, bool jumps)
{
    RustFFI::compositor_core_set_scrollbar_track_press_jumps(m_core, presentation_id.value(), jumps);
}

void CompositorCore::bind_presentation(Web::CompositorContextId context_id, PresentationId presentation_id)
{
    RustFFI::compositor_core_bind_presentation(m_core, context_id.value(), presentation_id.value(), now().nanoseconds());
}

void CompositorCore::set_shown_in_remote_layer(Web::CompositorContextId context_id, bool shown)
{
    RustFFI::compositor_core_set_shown_in_remote_layer(m_core, context_id.value(), shown, now().nanoseconds());
}

void CompositorCore::set_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    RustFFI::compositor_core_set_display_metadata(m_core, context_id.value(), display_id.has_value(), display_id.value_or(0), refresh_rate, now().nanoseconds());
}

void CompositorCore::set_window_server_may_show_high_dynamic_range(bool may_show)
{
    RustFFI::compositor_core_set_window_server_may_show_high_dynamic_range(m_core, may_show, now().nanoseconds());
}

void CompositorCore::set_presents_from_cpu_memory(Web::CompositorContextId context_id, bool presents_from_cpu_memory)
{
    RustFFI::compositor_core_set_presents_from_cpu_memory(m_core, context_id.value(), presents_from_cpu_memory, now().nanoseconds());
}

void CompositorCore::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    RustFFI::compositor_core_request_rendering_opportunity(m_core, context_id.value(), maximum_frames_per_second, now().nanoseconds());
}

void CompositorCore::request_clock_tick(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    RustFFI::compositor_core_request_clock_tick(m_core, context_id.value(), maximum_frames_per_second);
}

void CompositorCore::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    RustFFI::compositor_core_hurry_rendering_opportunity(m_core, context_id.value(), now().nanoseconds());
}

void CompositorCore::present_frame_of_page(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    RustFFI::compositor_core_present_frame_of_page(m_core, context_id.value(), viewport_rect, now().nanoseconds());
}

void CompositorCore::presented_store_released(Web::CompositorContextId context_id, i32 bitmap_id)
{
    RustFFI::compositor_core_presented_store_released(m_core, context_id.value(), bitmap_id, now().nanoseconds());
}

void CompositorCore::vsync(Optional<u64> display_id, MonotonicTime frame_time)
{
    RustFFI::compositor_core_vsync(m_core, display_id.has_value(), display_id.value_or(0), frame_time.nanoseconds(), now().nanoseconds());
}

void CompositorCore::gpu_work_finished(u64 token)
{
    RustFFI::compositor_core_gpu_work_finished(m_core, token, now().nanoseconds());
}

void CompositorCore::frame_ready_for_hand_off(u64 token)
{
    RustFFI::compositor_core_frame_ready_for_hand_off(m_core, token, now().nanoseconds());
}

void CompositorCore::context_content_changed(Web::CompositorContextId context_id)
{
    RustFFI::compositor_core_context_content_changed(m_core, context_id.value(), now().nanoseconds());
}

void CompositorCore::canvas_content_changed(WebContentClientId client, Compositing::CanvasId canvas_id)
{
    RustFFI::compositor_core_canvas_content_changed(m_core, client.value(), canvas_id.value(), now().nanoseconds());
}

bool CompositorCore::paint_screenshot(Web::CompositorContextId context_id, Gfx::PaintingSurface& surface)
{
    return RustFFI::compositor_core_paint_screenshot(m_core, context_id.value(), &surface, now().nanoseconds());
}

Optional<CompositorCore::PresentedFrame> CompositorCore::presented_frame_for_snapshot(Web::CompositorContextId context_id)
{
    void* surface = nullptr;
    Gfx::IntSize frame_size;
    if (!RustFFI::compositor_core_presented_frame_for_snapshot(m_core, context_id.value(), now().nanoseconds(), &surface, &frame_size))
        return {};
    return PresentedFrame { adopt_ref(*static_cast<Gfx::PaintingSurface*>(surface)), frame_size };
}

void CompositorCore::timer_fired(Web::CompositorContextId context_id, ContextTimer timer)
{
    RustFFI::compositor_core_timer_fired(m_core, context_id.value(), timer, now().nanoseconds());
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_web_content(WebContentClientId client_id, IPC::RawMessage& message)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_web_content_message(m_core, client_id.value(), raw_message, now().nanoseconds());
    });
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_presentation(PresentationId presentation_id, IPC::RawMessage& message)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_presentation_message(m_core, presentation_id.value(), raw_message, now().nanoseconds());
    });
}

}
