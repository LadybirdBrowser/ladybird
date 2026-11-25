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

static void set_timer(void* host, u64 timer_number, i64 delay_nanoseconds)
{
    static_cast<CompositorCoreHost*>(host)->set_timer(timer_number, AK::Duration::from_nanoseconds(delay_nanoseconds));
}

static void cancel_timer(void* host, u64 timer_number)
{
    static_cast<CompositorCoreHost*>(host)->cancel_timer(timer_number);
}

static void destroy_timer(void* host, u64 timer_number)
{
    static_cast<CompositorCoreHost*>(host)->destroy_timer(timer_number);
}

static void shape_overlay_label(void* host, u64 context_id, bool has_font_family, ReadonlyBytes font_family, float font_size_in_points, double device_pixel_ratio, ReadonlyBytes text, void* receiver, void (*receive)(void*, RustFFI::FfiOverlayLabel const*))
{
    Optional<StringView> font_family_view;
    if (has_font_family)
        font_family_view = StringView { font_family };
    auto label = static_cast<CompositorCoreHost*>(host)->shape_overlay_label(Web::CompositorContextId { context_id }, font_family_view, font_size_in_points, device_pixel_ratio, StringView { text });
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
        .destroy_timer = Compositor::destroy_timer,
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

bool CompositorCore::set_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    return RustFFI::compositor_core_set_visibility(m_core, context_id.value(), visibility == Compositing::ContextVisibility::Visible, now().nanoseconds());
}

void CompositorCore::register_presentation(PresentationId presentation_id, PresentationCapabilities const& capabilities)
{
    RustFFI::compositor_core_register_presentation(m_core, presentation_id.value(), &capabilities);
}

void CompositorCore::unregister_presentation(PresentationId presentation_id)
{
    RustFFI::compositor_core_unregister_presentation(m_core, presentation_id.value(), now().nanoseconds());
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

void CompositorCore::forget_web_content_client(WebContentClientId client)
{
    RustFFI::compositor_core_forget_web_content_client(m_core, client.value());
}

bool CompositorCore::paint_screenshot(Web::CompositorContextId context_id, Gfx::PaintingSurface& surface)
{
    return RustFFI::compositor_core_paint_screenshot(m_core, context_id.value(), &surface, now().nanoseconds());
}

Optional<CompositorCore::PresentedFrame> CompositorCore::presented_frame_for_snapshot(Web::CompositorContextId context_id)
{
    Gfx::IntSize frame_size;
    auto const* surface = RustFFI::compositor_core_presented_frame_for_snapshot(m_core, context_id.value(), now().nanoseconds(), &frame_size);
    if (!surface)
        return {};
    return PresentedFrame { adopt_ref(const_cast<Gfx::PaintingSurface&>(*surface)), frame_size };
}

void CompositorCore::timer_fired(u64 timer_number)
{
    RustFFI::compositor_core_timer_fired(m_core, timer_number, now().nanoseconds());
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_web_content(WebContentClientId client_id, IPC::RawMessage& message)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_web_content_message(m_core, client_id.value(), raw_message, now().nanoseconds());
    });
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_control(IPC::RawMessage& message)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_control_message(m_core, raw_message, now().nanoseconds());
    });
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_presentation(PresentationId presentation_id, IPC::RawMessage& message)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_presentation_message(m_core, presentation_id.value(), raw_message, now().nanoseconds());
    });
}

IPC::RawMessage::OutcomeInRust CompositorCore::handle_message_from_render_clock(WebContentClientId client_id, IPC::RawMessage& message)
{
    return message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
        RustFFI::compositor_core_handle_render_clock_message(m_core, client_id.value(), raw_message, now().nanoseconds());
    });
}

}
