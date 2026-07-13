/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/TypeCasts.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/KeyboardEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

enum class DOMKeyLocation {
    Standard = 0,
    Left = 1,
    Right = 2,
    Numpad = 3,
};

// https://www.w3.org/TR/uievents/#interface-keyboardevent
class KeyboardEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(KeyboardEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(KeyboardEvent);

public:
    [[nodiscard]] static GC::Ref<KeyboardEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::KeyboardEventInit const& = {});
    [[nodiscard]] static GC::Ref<KeyboardEvent> create_from_platform_event(JS::Realm&, Utf16FlyString const& event_name, KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    static WebIDL::ExceptionOr<GC::Ref<KeyboardEvent>> construct_impl(JS::Realm&, Utf16FlyString const& event_name, Bindings::KeyboardEventInit const&);

    virtual ~KeyboardEvent() override;

    u32 key_code() const { return m_key_code; }
    u32 char_code() const { return m_char_code; }

    Utf16String const& key() const { return m_key; }
    Utf16FlyString const& code() const { return m_code; }
    u32 location() const { return m_location; }

    bool ctrl_key() const { return m_ctrl_key; }
    bool shift_key() const { return m_shift_key; }
    bool alt_key() const { return m_alt_key; }
    bool meta_key() const { return m_meta_key; }

    bool repeat() const { return m_repeat; }
    bool is_composing() const { return m_is_composing; }

    bool get_modifier_state(Utf16FlyString const& key_arg) const;

    virtual u32 which() const override { return m_key_code; }

    void init_keyboard_event(Utf16FlyString const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, Utf16String const& key, WebIDL::UnsignedLong location, bool ctrl_key, bool alt_key, bool shift_key, bool meta_key);

private:
    KeyboardEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::KeyboardEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_key;
    Utf16FlyString m_code;
    u32 m_location { 0 };
    bool m_ctrl_key { false };
    bool m_shift_key { false };
    bool m_alt_key { false };
    bool m_meta_key { false };
    bool m_modifier_alt_graph { false };
    bool m_modifier_caps_lock { false };
    bool m_modifier_fn { false };
    bool m_modifier_fn_lock { false };
    bool m_modifier_hyper { false };
    bool m_modifier_num_lock { false };
    bool m_modifier_scroll_lock { false };
    bool m_modifier_super { false };
    bool m_modifier_symbol { false };
    bool m_modifier_symbol_lock { false };
    bool m_repeat { false };
    bool m_is_composing { false };
    u32 m_key_code { 0 };
    u32 m_char_code { 0 };
};

}
