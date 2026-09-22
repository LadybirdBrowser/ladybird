/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Page/DragEvent.h>

namespace Web {

DragEvent DragEvent::clone_without_browser_data() const
{
    return { type, position, screen_position, button, buttons, modifiers, {}, nullptr, id };
}

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
    TRY(encoder.encode(event.id));
    return {};
}

template<>
ErrorOr<Web::DragEvent> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Web::DragEvent::Type>());
    auto position = TRY(decoder.decode<Compositing::DevicePixelPoint>());
    auto screen_position = TRY(decoder.decode<Compositing::DevicePixelPoint>());
    auto button = TRY(decoder.decode<Web::UIEvents::MouseButton>());
    auto buttons = TRY(decoder.decode<Web::UIEvents::MouseButton>());
    auto modifiers = TRY(decoder.decode<Web::UIEvents::KeyModifier>());
    auto files = TRY(decoder.decode<Vector<Web::HTML::SelectedFile>>());
    auto id = TRY(decoder.decode<u64>());

    return Web::DragEvent { type, position, screen_position, button, buttons, modifiers, move(files), nullptr, id };
}
