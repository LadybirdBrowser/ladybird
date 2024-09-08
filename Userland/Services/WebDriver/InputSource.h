/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Debug.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>

namespace Web::WebDriver {

// InputState has lists of InputSources, but the pointer input source constructor
// depends on InputState, so the definitions are circular
// forward declare InputState here
class InputState;

// https://w3c.github.io/webdriver/#input-sources
class InputSource {
public:
    static ErrorOr<NonnullOwnPtr<InputSource>> create(InputState const&, String const&, String const&);
    String const& input_id() const { return m_input_id; }

private:
    InputSource() { }
    // Each input source has an input id which is stored as a key in the input state map.
    // input id is a UUID
    String m_input_id;
};

// https://w3c.github.io/webdriver/#null-input-source
class NullInputSource : InputSource {
public:
    static NonnullOwnPtr<NullInputSource> create();
    void pause(int tick_duration);
    // leaving public null constructor for now
    // Classes inheriting can't chain into private ctor
    NullInputSource();

private:
    // NullInputSource();
};

// The other device types inherit from NullInputSource because they all need to Pause
// but this leads to a non-leaf class as nonfinal, could restructure
// https://w3c.github.io/webdriver/#key-input-source
class KeyInputSource final : public NullInputSource {
public:
    static NonnullOwnPtr<KeyInputSource> create();
    // FIXME: Implement when we have more clarity
    // void keyDown();
    // void keyUp();

private:
    HashTable<String> m_pressed;
    bool m_alt { false };
    bool m_ctrl { false };
    bool m_meta { false };
    bool m_shift { false };
};

// https://w3c.github.io/webdriver/#pointer-input-source
class PointerInputSource final : public NullInputSource {
public:
    static NonnullOwnPtr<PointerInputSource> create(InputState const& input_state, String const& subtype);

    // FIXME: Implement when we have more clarity
    // void pointerDown();
    // void pointerUp();
    // void pointerMove();
    // void pointerCancel();

private:
    PointerInputSource(InputState const& input_state, String const& subtype);
    String m_subtype;
    // The numeric id of the pointing device.
    // This is a positive integer, with the values 0 and 1 reserved for mouse-type pointers.
    unsigned m_pointer_id;
    // A set of unsigned integers representing the pointer buttons that are currently depressed
    HashTable<unsigned int> m_pressed;
    // An unsigned integer representing the pointer x/y location in viewport coordinates
    unsigned m_x { 0 }, m_y { 0 };
};

// https://w3c.github.io/webdriver/#wheel-input-source
enum class WheelDirections { Up,
    Down,
    Left,
    Right };

class WheelInputSource final : public NullInputSource {
public:
    static NonnullOwnPtr<WheelInputSource> create();
    WheelDirections scroll();

private:
    WheelInputSource();
};
}
