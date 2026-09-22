/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibCompositing/InputEvent.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>

namespace Web {

struct WEB_API DragEvent {
    enum class Type : u8 {
        DragStart,
        DragMove,
        DragEnd,
        Drop,
    };

    DragEvent clone_without_browser_data() const;

    Type type;
    Compositing::DevicePixelPoint position;
    Compositing::DevicePixelPoint screen_position;
    UIEvents::MouseButton button { UIEvents::MouseButton::None };
    UIEvents::MouseButton buttons { UIEvents::MouseButton::None };
    UIEvents::KeyModifier modifiers { UIEvents::KeyModifier::Mod_None };
    Vector<HTML::SelectedFile> files;

    OwnPtr<Compositing::BrowserInputData> browser_data;
    u64 id { 0 };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::DragEvent const&);

template<>
WEB_API ErrorOr<Web::DragEvent> decode(Decoder&);

}
