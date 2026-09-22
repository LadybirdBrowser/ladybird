/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/InputEvent.h>
#include <LibCompositing/Types.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <math.h>

namespace Compositing {

KeyEvent KeyEvent::clone_without_browser_data() const
{
    return { type, key, modifiers, code_point, repeat, should_insert_text, nullptr, async_scroll_performed_default_action, id };
}

MouseEvent MouseEvent::clone_without_browser_data() const
{
    return { type, position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_precision, scroll_gesture_phase, click_count, nullptr, async_scroll_performed_default_action, id, scrollbar_dragged_by_compositor };
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Compositing::KeyEvent const& event)
{
    TRY(encoder.encode(event.type));
    TRY(encoder.encode(event.key));
    TRY(encoder.encode(event.modifiers));
    TRY(encoder.encode(event.code_point));
    TRY(encoder.encode(event.repeat));
    TRY(encoder.encode(event.should_insert_text));
    TRY(encoder.encode(event.async_scroll_performed_default_action));
    TRY(encoder.encode(event.id));
    return {};
}

template<>
ErrorOr<Compositing::KeyEvent> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Compositing::KeyEvent::Type>());
    auto key = TRY(decoder.decode<Compositing::KeyCode>());
    auto modifiers = TRY(decoder.decode<Compositing::KeyModifier>());
    auto code_point = TRY(decoder.decode<u32>());
    auto repeat = TRY(decoder.decode<bool>());
    auto should_insert_text = TRY(decoder.decode<bool>());
    auto async_scroll_performed_default_action = TRY(decoder.decode<bool>());
    auto id = TRY(decoder.decode<u64>());

    return Compositing::KeyEvent { type, key, modifiers, code_point, repeat, should_insert_text, nullptr, async_scroll_performed_default_action, id };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Compositing::MouseEvent const& event)
{
    TRY(encoder.encode(event.type));
    TRY(encoder.encode(event.position));
    TRY(encoder.encode(event.screen_position));
    TRY(encoder.encode(event.button));
    TRY(encoder.encode(event.buttons));
    TRY(encoder.encode(event.modifiers));
    TRY(encoder.encode(event.wheel_delta_x));
    TRY(encoder.encode(event.wheel_delta_y));
    TRY(encoder.encode(event.wheel_delta_precision));
    TRY(encoder.encode(event.scroll_gesture_phase));
    TRY(encoder.encode(event.click_count));
    TRY(encoder.encode(event.async_scroll_performed_default_action));
    TRY(encoder.encode(event.id));
    TRY(encoder.encode(event.scrollbar_dragged_by_compositor));
    return {};
}

template<>
ErrorOr<Compositing::MouseEvent> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Compositing::MouseEvent::Type>());
    auto position = TRY(decoder.decode<Compositing::DevicePixelPoint>());
    auto screen_position = TRY(decoder.decode<Compositing::DevicePixelPoint>());
    auto button = TRY(decoder.decode<Compositing::MouseButton>());
    auto buttons = TRY(decoder.decode<Compositing::MouseButton>());
    auto modifiers = TRY(decoder.decode<Compositing::KeyModifier>());
    auto wheel_delta_x = TRY(decoder.decode<double>());
    auto wheel_delta_y = TRY(decoder.decode<double>());
    auto wheel_delta_precision = TRY(decoder.decode<Compositing::WheelDeltaPrecision>());
    auto scroll_gesture_phase = TRY(decoder.decode<Compositing::ScrollGesturePhase>());
    auto click_count = TRY(decoder.decode<int>());
    auto async_scroll_performed_default_action = TRY(decoder.decode<bool>());
    auto id = TRY(decoder.decode<u64>());
    auto scrollbar_dragged_by_compositor = TRY(decoder.decode<Optional<Compositing::ScrollbarDraggedByCompositor>>());

    return Compositing::MouseEvent { type, position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_precision, scroll_gesture_phase, click_count, nullptr, async_scroll_performed_default_action, id, scrollbar_dragged_by_compositor };
}

template<>
COMPOSITING_API ErrorOr<void> IPC::encode(Encoder& encoder, Compositing::PinchEvent const& event)
{
    TRY(encoder.encode(event.position));
    TRY(encoder.encode(event.modifiers));
    TRY(encoder.encode(event.scale_delta));
    TRY(encoder.encode(event.id));
    return {};
}

template<>
COMPOSITING_API ErrorOr<Compositing::PinchEvent> IPC::decode(Decoder& decoder)
{
    auto position = TRY(decoder.decode<Compositing::DevicePixelPoint>());
    auto modifiers = TRY(decoder.decode<Compositing::KeyModifier>());
    auto scale_delta = TRY(decoder.decode<double>());
    auto id = TRY(decoder.decode<u64>());

    if (isnan(scale_delta) || isinf(scale_delta))
        return Error::from_string_literal("IPC: Invalid scale_delta value");

    return Compositing::PinchEvent { position, modifiers, scale_delta, id };
}
