/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::Gamepad {

// Handles are never reused.
using GamepadHandle = u32;

struct WEBCOMMON_API GamepadInputDescriptor {
    Optional<u32> canonical_index;
    i16 logical_minimum { 0 };
    i16 logical_maximum { 0 };
};

struct WEBCOMMON_API GamepadDescription {
    GamepadHandle handle { 0 };
    String id;
    bool has_standard_mapping { false };
    bool supports_dual_rumble { false };
    bool supports_trigger_rumble { false };
    Vector<GamepadInputDescriptor> axes;
    Vector<GamepadInputDescriptor> buttons;
};

struct WEBCOMMON_API GamepadState {
    GamepadHandle handle { 0 };
    Vector<i16> axis_values;
    Vector<i16> button_values;

    bool operator==(GamepadState const&) const = default;
};

struct WEBCOMMON_API GamepadDualRumbleEffect {
    u16 strong_magnitude { 0 };
    u16 weak_magnitude { 0 };
};

struct WEBCOMMON_API GamepadTriggerRumbleEffect {
    u16 left_trigger_magnitude { 0 };
    u16 right_trigger_magnitude { 0 };
};

using GamepadEffect = Variant<GamepadDualRumbleEffect, GamepadTriggerRumbleEffect>;

struct WEBCOMMON_API GamepadConnectedEvent {
    GamepadDescription description;
};

struct WEBCOMMON_API GamepadDisconnectedEvent {
    GamepadHandle handle { 0 };
};

using GamepadChangeEvent = Variant<GamepadConnectedEvent, GamepadDisconnectedEvent>;

struct WEBCOMMON_API ReceivedDualRumbleEffect {
    u16 low_frequency_rumble { 0 };
    u16 high_frequency_rumble { 0 };
};

struct WEBCOMMON_API ReceivedTriggerRumbleEffect {
    u16 left_rumble { 0 };
    u16 right_rumble { 0 };
};

struct WEBCOMMON_API ReceivedRumbleEffects {
    Vector<ReceivedDualRumbleEffect> dual_rumble_effects;
    Vector<ReceivedTriggerRumbleEffect> trigger_rumble_effects;
};

struct WEBCOMMON_API VirtualGamepad {
    GamepadHandle handle { 0 };
    Vector<i32> buttons;
    Vector<i32> axes;
    Vector<i32> triggers;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadInputDescriptor const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadInputDescriptor> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadDescription const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadDescription> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadState const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadState> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadDualRumbleEffect const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadDualRumbleEffect> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadTriggerRumbleEffect const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadTriggerRumbleEffect> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadConnectedEvent const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadConnectedEvent> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadDisconnectedEvent const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadDisconnectedEvent> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::ReceivedDualRumbleEffect const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::ReceivedDualRumbleEffect> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::ReceivedTriggerRumbleEffect const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::ReceivedTriggerRumbleEffect> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::VirtualGamepad const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::VirtualGamepad> decode(Decoder&);

}
