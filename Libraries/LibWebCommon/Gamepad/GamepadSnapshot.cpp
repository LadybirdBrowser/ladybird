/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadInputDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.canonical_index));
    TRY(encoder.encode(descriptor.logical_minimum));
    TRY(encoder.encode(descriptor.logical_maximum));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadInputDescriptor> IPC::decode(Decoder& decoder)
{
    auto canonical_index = TRY(decoder.decode<Optional<u32>>());
    auto logical_minimum = TRY(decoder.decode<i16>());
    auto logical_maximum = TRY(decoder.decode<i16>());

    return Web::Gamepad::GamepadInputDescriptor { canonical_index, logical_minimum, logical_maximum };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadDescription const& description)
{
    TRY(encoder.encode(description.handle));
    TRY(encoder.encode(description.id));
    TRY(encoder.encode(description.has_standard_mapping));
    TRY(encoder.encode(description.supports_dual_rumble));
    TRY(encoder.encode(description.supports_trigger_rumble));
    TRY(encoder.encode(description.axes));
    TRY(encoder.encode(description.buttons));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadDescription> IPC::decode(Decoder& decoder)
{
    auto handle = TRY(decoder.decode<Web::Gamepad::GamepadHandle>());
    auto id = TRY(decoder.decode<String>());
    auto has_standard_mapping = TRY(decoder.decode<bool>());
    auto supports_dual_rumble = TRY(decoder.decode<bool>());
    auto supports_trigger_rumble = TRY(decoder.decode<bool>());
    auto axes = TRY(decoder.decode<Vector<Web::Gamepad::GamepadInputDescriptor>>());
    auto buttons = TRY(decoder.decode<Vector<Web::Gamepad::GamepadInputDescriptor>>());

    return Web::Gamepad::GamepadDescription { handle, move(id), has_standard_mapping, supports_dual_rumble, supports_trigger_rumble, move(axes), move(buttons) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadState const& state)
{
    TRY(encoder.encode(state.handle));
    TRY(encoder.encode(state.axis_values));
    TRY(encoder.encode(state.button_values));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadState> IPC::decode(Decoder& decoder)
{
    auto handle = TRY(decoder.decode<Web::Gamepad::GamepadHandle>());
    auto axis_values = TRY(decoder.decode<Vector<i16>>());
    auto button_values = TRY(decoder.decode<Vector<i16>>());

    return Web::Gamepad::GamepadState { handle, move(axis_values), move(button_values) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadDualRumbleEffect const& effect)
{
    TRY(encoder.encode(effect.strong_magnitude));
    TRY(encoder.encode(effect.weak_magnitude));
    TRY(encoder.encode(effect.duration));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadDualRumbleEffect> IPC::decode(Decoder& decoder)
{
    auto strong_magnitude = TRY(decoder.decode<u16>());
    auto weak_magnitude = TRY(decoder.decode<u16>());
    auto duration = TRY(decoder.decode<u32>());

    return Web::Gamepad::GamepadDualRumbleEffect { strong_magnitude, weak_magnitude, duration };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadTriggerRumbleEffect const& effect)
{
    TRY(encoder.encode(effect.left_trigger_magnitude));
    TRY(encoder.encode(effect.right_trigger_magnitude));
    TRY(encoder.encode(effect.duration));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadTriggerRumbleEffect> IPC::decode(Decoder& decoder)
{
    auto left_trigger_magnitude = TRY(decoder.decode<u16>());
    auto right_trigger_magnitude = TRY(decoder.decode<u16>());
    auto duration = TRY(decoder.decode<u32>());

    return Web::Gamepad::GamepadTriggerRumbleEffect { left_trigger_magnitude, right_trigger_magnitude, duration };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadConnectedEvent const& event)
{
    TRY(encoder.encode(event.description));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadConnectedEvent> IPC::decode(Decoder& decoder)
{
    auto description = TRY(decoder.decode<Web::Gamepad::GamepadDescription>());

    return Web::Gamepad::GamepadConnectedEvent { move(description) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::GamepadDisconnectedEvent const& event)
{
    TRY(encoder.encode(event.handle));
    return {};
}

template<>
ErrorOr<Web::Gamepad::GamepadDisconnectedEvent> IPC::decode(Decoder& decoder)
{
    auto handle = TRY(decoder.decode<Web::Gamepad::GamepadHandle>());

    return Web::Gamepad::GamepadDisconnectedEvent { handle };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::ReceivedDualRumbleEffect const& effect)
{
    TRY(encoder.encode(effect.low_frequency_rumble));
    TRY(encoder.encode(effect.high_frequency_rumble));
    return {};
}

template<>
ErrorOr<Web::Gamepad::ReceivedDualRumbleEffect> IPC::decode(Decoder& decoder)
{
    auto low_frequency_rumble = TRY(decoder.decode<u16>());
    auto high_frequency_rumble = TRY(decoder.decode<u16>());

    return Web::Gamepad::ReceivedDualRumbleEffect { low_frequency_rumble, high_frequency_rumble };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::ReceivedTriggerRumbleEffect const& effect)
{
    TRY(encoder.encode(effect.left_rumble));
    TRY(encoder.encode(effect.right_rumble));
    return {};
}

template<>
ErrorOr<Web::Gamepad::ReceivedTriggerRumbleEffect> IPC::decode(Decoder& decoder)
{
    auto left_rumble = TRY(decoder.decode<u16>());
    auto right_rumble = TRY(decoder.decode<u16>());

    return Web::Gamepad::ReceivedTriggerRumbleEffect { left_rumble, right_rumble };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Gamepad::VirtualGamepad const& virtual_gamepad)
{
    TRY(encoder.encode(virtual_gamepad.handle));
    TRY(encoder.encode(virtual_gamepad.buttons));
    TRY(encoder.encode(virtual_gamepad.axes));
    TRY(encoder.encode(virtual_gamepad.triggers));
    return {};
}

template<>
ErrorOr<Web::Gamepad::VirtualGamepad> IPC::decode(Decoder& decoder)
{
    auto handle = TRY(decoder.decode<Web::Gamepad::GamepadHandle>());
    auto buttons = TRY(decoder.decode<Vector<i32>>());
    auto axes = TRY(decoder.decode<Vector<i32>>());
    auto triggers = TRY(decoder.decode<Vector<i32>>());

    return Web::Gamepad::VirtualGamepad { handle, move(buttons), move(axes), move(triggers) };
}
