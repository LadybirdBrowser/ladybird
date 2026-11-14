/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/RustFFI.h>
#include <LibCompositing/UIOverlays.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

int scaled_overlay_length(int length, double device_pixel_ratio)
{
    return RustFFI::compositing_scaled_overlay_length(length, device_pixel_ratio);
}

PictureInPictureControlsGeometry picture_in_picture_controls_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    RustFFI::PictureInPictureControlsGeometry geometry;
    RustFFI::compositing_picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio, &geometry);
    return { .back_to_tab_button = geometry.back_to_tab_button, .close_button = geometry.close_button };
}

PausedDebuggerOverlayGeometry paused_debugger_overlay_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    RustFFI::PausedDebuggerOverlayGeometry geometry;
    RustFFI::compositing_paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio, &geometry);
    return {
        .toolbar = geometry.toolbar,
        .message = geometry.message,
        .step_over_button = geometry.step_over_button,
        .continue_button = geometry.continue_button,
    };
}

}

namespace IPC {

template<typename Action, typename Callback>
static ErrorOr<Optional<Action>> decode_overlay_action(Decoder& decoder, Callback from_underlying)
{
    auto value = TRY(decoder.decode<Optional<u8>>());
    if (!value.has_value())
        return Optional<Action> {};
    auto action = from_underlying(*value);
    if (!action.has_value())
        return Error::from_string_literal("IPC decode: Invalid UI overlay action");
    return action;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::PausedDebuggerOverlayState const& state)
{
    TRY(encoder.encode(state.font_family));
    TRY(encoder.encode(state.hovered_action));
    return {};
}

template<>
ErrorOr<Compositing::PausedDebuggerOverlayState> decode(Decoder& decoder)
{
    return Compositing::PausedDebuggerOverlayState {
        .font_family = TRY(decoder.decode<Optional<String>>()),
        .hovered_action = TRY(decode_overlay_action<Compositing::PausedDebuggerOverlayAction>(decoder, Compositing::paused_debugger_overlay_action_from_underlying)),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::PictureInPictureControlsState const& state)
{
    TRY(encoder.encode(state.shown));
    TRY(encoder.encode(state.hovered_control));
    return {};
}

template<>
ErrorOr<Compositing::PictureInPictureControlsState> decode(Decoder& decoder)
{
    return Compositing::PictureInPictureControlsState {
        .shown = TRY(decoder.decode<bool>()),
        .hovered_control = TRY(decode_overlay_action<Compositing::PictureInPictureControl>(decoder, Compositing::picture_in_picture_control_from_underlying)),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::UIOverlays const& overlays)
{
    TRY(encoder.encode(overlays.paused_debugger));
    TRY(encoder.encode(overlays.picture_in_picture_controls));
    return {};
}

template<>
ErrorOr<Compositing::UIOverlays> decode(Decoder& decoder)
{
    return Compositing::UIOverlays {
        .paused_debugger = TRY(decoder.decode<Optional<Compositing::PausedDebuggerOverlayState>>()),
        .picture_in_picture_controls = TRY(decoder.decode<Optional<Compositing::PictureInPictureControlsState>>()),
    };
}

}
