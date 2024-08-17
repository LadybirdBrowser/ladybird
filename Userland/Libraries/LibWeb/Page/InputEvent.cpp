/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Page/InputEvent.h>

namespace Web {

KeyEvent KeyEvent::clone_without_chrome_data() const
{
    return { type, key, modifiers, code_point, nullptr };
}

MouseEvent MouseEvent::clone_without_chrome_data() const
{
    return { type, position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y, nullptr };
}

DragEvent DragEvent::clone_without_chrome_data() const
{
    return { type, position, screen_position, button, buttons, modifiers, {}, nullptr };
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::KeyEvent const& event)
{
    TRY(encoder.encode(event.type));
    TRY(encoder.encode(event.key));
    TRY(encoder.encode(event.modifiers));
    TRY(encoder.encode(event.code_point));
    return {};
}

template<>
ErrorOr<Web::KeyEvent> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Web::KeyEvent::Type>());
    auto key = TRY(decoder.decode<Web::UIEvents::KeyCode>());
    auto modifiers = TRY(decoder.decode<Web::UIEvents::KeyModifier>());
    auto code_point = TRY(decoder.decode<u32>());

    return Web::KeyEvent { type, key, modifiers, code_point, nullptr };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::MouseEvent const& event)
{
    TRY(encoder.encode(event.type));
    TRY(encoder.encode(event.position));
    TRY(encoder.encode(event.screen_position));
    TRY(encoder.encode(event.button));
    TRY(encoder.encode(event.buttons));
    TRY(encoder.encode(event.modifiers));
    TRY(encoder.encode(event.wheel_delta_x));
    TRY(encoder.encode(event.wheel_delta_y));
    return {};
}

template<>
ErrorOr<Web::MouseEvent> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Web::MouseEvent::Type>());
    auto position = TRY(decoder.decode<Web::DevicePixelPoint>());
    auto screen_position = TRY(decoder.decode<Web::DevicePixelPoint>());
    auto button = TRY(decoder.decode<Web::UIEvents::MouseButton>());
    auto buttons = TRY(decoder.decode<Web::UIEvents::MouseButton>());
    auto modifiers = TRY(decoder.decode<Web::UIEvents::KeyModifier>());
    auto wheel_delta_x = TRY(decoder.decode<int>());
    auto wheel_delta_y = TRY(decoder.decode<int>());

    return Web::MouseEvent { type, position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y, nullptr };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::DragEvent const& event)
{
    TRY(encoder.encode(event.type));
    TRY(encoder.encode(event.position));
    TRY(encoder.encode(event.screen_position));
    TRY(encoder.encode(event.button));
    TRY(encoder.encode(event.buttons));
    TRY(encoder.encode(event.modifiers));
    TRY(encoder.encode(event.files));
    return {};
}

template<>
ErrorOr<Web::DragEvent> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Web::DragEvent::Type>());
    auto position = TRY(decoder.decode<Web::DevicePixelPoint>());
    auto screen_position = TRY(decoder.decode<Web::DevicePixelPoint>());
    auto button = TRY(decoder.decode<Web::UIEvents::MouseButton>());
    auto buttons = TRY(decoder.decode<Web::UIEvents::MouseButton>());
    auto modifiers = TRY(decoder.decode<Web::UIEvents::KeyModifier>());
    auto files = TRY(decoder.decode<Vector<Web::HTML::SelectedFile>>());

    return Web::DragEvent { type, position, screen_position, button, buttons, modifiers, move(files), nullptr };
}
