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
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>

namespace Web {

struct WEBCOMMON_API DragEvent {
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
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::DragEvent const&);

template<>
WEBCOMMON_API ErrorOr<Web::DragEvent> decode(Decoder&);

}
