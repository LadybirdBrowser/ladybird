/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibWeb/Compositor/NavigablePresenter.h>
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::Compositor {

NavigablePresenter::NavigablePresenter()
    : m_presenter(Layout::RustFFI::navigable_presenter_create())
{
}

NavigablePresenter::~NavigablePresenter()
{
    Layout::RustFFI::navigable_presenter_release(m_presenter);
}

static HTML::PaintConfig paint_config_from_ffi(Layout::RustFFI::FfiPaintConfig const& config)
{
    HTML::PaintConfig paint_config;
    paint_config.paint_overlay = config.paint_overlay;
    paint_config.should_show_line_box_borders = config.should_show_line_box_borders;
    paint_config.should_show_caret_hit_test_debug_overlay = config.should_show_caret_hit_test_debug_overlay;
    paint_config.force_dark_enabled = config.force_dark_enabled;
    paint_config.force_dark_foreground_threshold = config.force_dark_foreground_threshold;
    paint_config.force_dark_background_threshold = config.force_dark_background_threshold;
    if (config.has_canvas_fill_rect)
        paint_config.canvas_fill_rect = config.canvas_fill_rect;
    return paint_config;
}

static Layout::RustFFI::FfiPaintConfig paint_config_to_ffi(HTML::PaintConfig const& config)
{
    return {
        .paint_overlay = config.paint_overlay,
        .should_show_line_box_borders = config.should_show_line_box_borders,
        .should_show_caret_hit_test_debug_overlay = config.should_show_caret_hit_test_debug_overlay,
        .force_dark_enabled = config.force_dark_enabled,
        .force_dark_foreground_threshold = config.force_dark_foreground_threshold,
        .force_dark_background_threshold = config.force_dark_background_threshold,
        .has_canvas_fill_rect = config.canvas_fill_rect.has_value(),
        .canvas_fill_rect = config.canvas_fill_rect.value_or({}),
    };
}

Optional<HTML::PaintConfig> NavigablePresenter::compositor_display_list_paint_config() const
{
    Layout::RustFFI::FfiPaintConfig config {};
    if (!Layout::RustFFI::navigable_presenter_compositor_display_list_paint_config(m_presenter, &config))
        return {};
    return paint_config_from_ffi(config);
}

Optional<u64> NavigablePresenter::compositor_display_list_structural_epoch() const
{
    u64 epoch = 0;
    if (!Layout::RustFFI::navigable_presenter_compositor_display_list_structural_epoch(m_presenter, &epoch))
        return {};
    return epoch;
}

void NavigablePresenter::forget_compositor_display_list()
{
    Layout::RustFFI::navigable_presenter_forget_compositor_display_list(m_presenter);
}

String NavigablePresenter::display_list_window_scroll_positions_for_testing() const
{
    String positions;
    Layout::RustFFI::navigable_presenter_display_list_window_scroll_positions_for_testing(m_presenter, &positions, [](void* receiver, u8 const* bytes, size_t byte_count) {
        *static_cast<String*>(receiver) = MUST(String::from_utf8(StringView { bytes, byte_count }));
    });
    return positions;
}

Optional<PresentedBy> NavigablePresenter::last_frame_presented_by() const
{
    switch (Layout::RustFFI::navigable_presenter_last_frame_presented_by(m_presenter)) {
    case Layout::RustFFI::FfiPresentedBy::None:
        return {};
    case Layout::RustFFI::FfiPresentedBy::Commit:
        return PresentedBy::Commit;
    case Layout::RustFFI::FfiPresentedBy::Clock:
        return PresentedBy::Clock;
    }
    VERIFY_NOT_REACHED();
}

Optional<u64> NavigablePresenter::last_keyboard_scroll_state_generation() const
{
    u64 generation = 0;
    if (!Layout::RustFFI::navigable_presenter_last_keyboard_scroll_state_generation(m_presenter, &generation))
        return {};
    return generation;
}

template<typename T>
static IPC::MessageBuffer encode_for_rust(T const& value)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(value));
    return buffer;
}

void NavigablePresenter::lend_seal(SealedPresentation const& sealed, Function<void(Layout::RustFFI::FfiSeal const&)> const& take) const
{
    auto scroll_requests = encode_for_rust(sealed.scroll_requests);
    auto keyboard_scroll_state = encode_for_rust(sealed.keyboard_scroll_state);
    Layout::RustFFI::FfiSeal seal {
        .presenter = m_presenter,
        .paint_config = paint_config_to_ffi(sealed.paint_config),
        .has_visual_context_tree_structural_epoch = sealed.visual_context_tree_structural_epoch.has_value(),
        .visual_context_tree_structural_epoch = sealed.visual_context_tree_structural_epoch.value_or(0),
        .sends_visual_context_tree = sealed.sends_visual_context_tree,
        .encoded_scroll_requests = scroll_requests.data().data(),
        .encoded_scroll_requests_size = scroll_requests.data().size(),
        .encoded_keyboard_scroll_state = keyboard_scroll_state.data().data(),
        .encoded_keyboard_scroll_state_size = keyboard_scroll_state.data().size(),
        .context_id = sealed.context_id.value(),
        .has_present_viewport_rect = sealed.present_viewport_rect.has_value(),
        .present_viewport_rect = sealed.present_viewport_rect.value_or({}),
        .generation = sealed.generation,
        .visual_context_tree_mismatches_for_testing = sealed.visual_context_tree_mismatches_for_testing,
    };
    take(seal);
}

}
