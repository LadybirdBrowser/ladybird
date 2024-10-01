/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

enum class InputSourceType {
    None,
    Key,
    Pointer,
    Wheel,
};

// https://w3c.github.io/webdriver/#dfn-null-input-source
struct NullInputSource {
};

// https://w3c.github.io/webdriver/#dfn-key-input-source
struct KeyInputSource {
    HashTable<String> pressed;
    bool alt { false };
    bool ctrl { false };
    bool meta { false };
    bool shift { false };
};

// https://w3c.github.io/webdriver/#dfn-pointer-input-source
struct PointerInputSource {
    enum class Subtype {
        Mouse,
        Pen,
        Touch,
    };

    PointerInputSource(InputState const&, Subtype);

    Subtype subtype { Subtype::Mouse };
    u32 pointer_id { 0 };
    UIEvents::MouseButton pressed { UIEvents::MouseButton::None };
    CSSPixelPoint position;
};

// https://w3c.github.io/webdriver/#dfn-wheel-input-source
struct WheelInputSource {
};

// https://w3c.github.io/webdriver/#dfn-input-source
using InputSource = Variant<NullInputSource, KeyInputSource, PointerInputSource, WheelInputSource>;

// https://w3c.github.io/webdriver/#dfn-global-key-state
struct GlobalKeyState {
    UIEvents::KeyModifier modifiers() const;

    HashTable<String> pressed;
    bool alt_key { false };
    bool ctrl_key { false };
    bool meta_key { false };
    bool shift_key { false };
};

Optional<InputSourceType> input_source_type_from_string(StringView);
Optional<PointerInputSource::Subtype> pointer_input_source_subtype_from_string(StringView);

InputSource create_input_source(InputState const&, InputSourceType, Optional<PointerInputSource::Subtype>);
void add_input_source(InputState&, String id, InputSource);
void remove_input_source(InputState&, StringView id);
Optional<InputSource&> get_input_source(InputState&, StringView id);
ErrorOr<InputSource*, WebDriver::Error> get_or_create_input_source(InputState&, InputSourceType, StringView id, Optional<PointerInputSource::Subtype>);

GlobalKeyState get_global_key_state(InputState const&);

}
