/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCompositing/UIOverlays.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Page/InputEvent.h>

using namespace Compositing;

template<typename Encode>
static ErrorOr<UIOverlays> decode_overlays(Encode encode)
{
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encode(encoder));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return IPC::decode<UIOverlays>(decoder);
}

TEST_CASE(overlays_survive_ipc)
{
    UIOverlays overlays;
    overlays.paused_debugger = PausedDebuggerOverlayState {
        .font_family = "Inter"_string,
        .hovered_action = PausedDebuggerOverlayAction::Continue,
    };
    overlays.picture_in_picture_controls = PictureInPictureControlsState {
        .shown = true,
        .hovered_control = PictureInPictureControl::BackToTab,
    };

    auto decoded = MUST(decode_overlays([&](IPC::Encoder& encoder) { return encoder.encode(overlays); }));
    EXPECT(decoded == overlays);
}

TEST_CASE(an_overlay_that_is_not_shown_stays_absent)
{
    UIOverlays overlays;

    auto decoded = MUST(decode_overlays([&](IPC::Encoder& encoder) { return encoder.encode(overlays); }));
    EXPECT(!decoded.paused_debugger.has_value());
    EXPECT(!decoded.picture_in_picture_controls.has_value());
}

TEST_CASE(an_unknown_paused_debugger_action_is_rejected)
{
    auto decoded = decode_overlays([](IPC::Encoder& encoder) -> ErrorOr<void> {
        TRY(encoder.encode(true));
        TRY(encoder.encode(Optional<String> {}));
        TRY(encoder.encode(Optional<u8> { 255 }));
        TRY(encoder.encode(false));
        return {};
    });
    EXPECT(decoded.is_error());
}

TEST_CASE(an_unknown_picture_in_picture_control_is_rejected)
{
    auto decoded = decode_overlays([](IPC::Encoder& encoder) -> ErrorOr<void> {
        TRY(encoder.encode(false));
        TRY(encoder.encode(true));
        TRY(encoder.encode(true));
        TRY(encoder.encode(Optional<u8> { 255 }));
        return {};
    });
    EXPECT(decoded.is_error());
}

static Web::MouseEvent mouse_event(Web::MouseEvent::Type type, Web::UIEvents::MouseButton button = Web::UIEvents::MouseButton::Primary, Web::UIEvents::MouseButton buttons = Web::UIEvents::MouseButton::None)
{
    return {
        .type = type,
        .position = {},
        .screen_position = {},
        .button = button,
        .buttons = buttons,
        .browser_data = nullptr,
    };
}

static constexpr auto MouseDown = Web::MouseEvent::Type::MouseDown;
static constexpr auto MouseUp = Web::MouseEvent::Type::MouseUp;
static constexpr auto MouseMove = Web::MouseEvent::Type::MouseMove;
static constexpr auto MouseLeave = Web::MouseEvent::Type::MouseLeave;
static constexpr auto MouseCancel = Web::MouseEvent::Type::MouseCancel;

TEST_CASE(a_press_acts_once_released_over_its_button)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;
    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::StepOver);
    EXPECT(pointer_state.is_pressing());

    auto outcome = pointer_state.handle_mouse_event(mouse_event(MouseUp), PausedDebuggerOverlayAction::StepOver);
    EXPECT(outcome.activated_action == PausedDebuggerOverlayAction::StepOver);
    EXPECT(!pointer_state.is_pressing());
}

TEST_CASE(a_press_released_over_another_button_does_not_act)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;
    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::StepOver);

    auto outcome = pointer_state.handle_mouse_event(mouse_event(MouseUp), PausedDebuggerOverlayAction::Continue);
    EXPECT(!outcome.activated_action.has_value());
    EXPECT(!pointer_state.is_pressing());
}

TEST_CASE(a_press_released_away_from_the_buttons_does_not_act)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;
    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::StepOver);

    EXPECT(!pointer_state.handle_mouse_event(mouse_event(MouseUp), {}).activated_action.has_value());
}

TEST_CASE(a_press_ends_when_it_is_cancelled_or_the_pointer_leaves)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;
    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::Continue);
    pointer_state.handle_mouse_event(mouse_event(MouseCancel), {});
    EXPECT(!pointer_state.is_pressing());
    EXPECT(!pointer_state.handle_mouse_event(mouse_event(MouseUp), PausedDebuggerOverlayAction::Continue).activated_action.has_value());

    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::Continue);
    pointer_state.handle_mouse_event(mouse_event(MouseLeave), {});
    EXPECT(!pointer_state.is_pressing());
    EXPECT(!pointer_state.handle_mouse_event(mouse_event(MouseUp), PausedDebuggerOverlayAction::Continue).activated_action.has_value());
}

TEST_CASE(a_press_ends_once_its_button_is_no_longer_held)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;
    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::StepOver);
    pointer_state.handle_mouse_event(mouse_event(MouseMove, Web::UIEvents::MouseButton::None, Web::UIEvents::MouseButton::Primary), PausedDebuggerOverlayAction::StepOver);
    EXPECT(pointer_state.is_pressing());

    pointer_state.handle_mouse_event(mouse_event(MouseMove, Web::UIEvents::MouseButton::None), PausedDebuggerOverlayAction::StepOver);
    EXPECT(!pointer_state.is_pressing());
}

TEST_CASE(only_the_primary_button_presses_and_releases)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;
    pointer_state.handle_mouse_event(mouse_event(MouseDown, Web::UIEvents::MouseButton::Secondary), PausedDebuggerOverlayAction::StepOver);
    EXPECT(!pointer_state.is_pressing());

    pointer_state.handle_mouse_event(mouse_event(MouseDown), PausedDebuggerOverlayAction::StepOver);
    EXPECT(!pointer_state.handle_mouse_event(mouse_event(MouseUp, Web::UIEvents::MouseButton::Secondary), PausedDebuggerOverlayAction::StepOver).activated_action.has_value());
    EXPECT(pointer_state.is_pressing());
}

TEST_CASE(the_hovered_button_follows_the_pointer)
{
    OverlayPointerState<PausedDebuggerOverlayAction> pointer_state;

    auto outcome = pointer_state.handle_mouse_event(mouse_event(MouseMove, Web::UIEvents::MouseButton::None), PausedDebuggerOverlayAction::Continue);
    EXPECT(outcome.hover_changed);
    EXPECT(pointer_state.hovered_action() == PausedDebuggerOverlayAction::Continue);

    outcome = pointer_state.handle_mouse_event(mouse_event(MouseMove, Web::UIEvents::MouseButton::None), PausedDebuggerOverlayAction::Continue);
    EXPECT(!outcome.hover_changed);

    outcome = pointer_state.handle_mouse_event(mouse_event(MouseLeave), {});
    EXPECT(outcome.hover_changed);
    EXPECT(!pointer_state.hovered_action().has_value());
}
