/*
 * Copyright (c) 2025, Saksham Goyal <sakgoy2001@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Promise.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
namespace Web::Keyboard {

class Keyboard final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Keyboard, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Keyboard);

public:
    virtual ~Keyboard() override;
    // https://wicg.github.io/keyboard-lock/#keyboard-lock
    GC::Ref<WebIDL::Promise> lock(const AK::Vector<AK::String>& key_codes);
    void unlock();
    // FIXME TODO
    // https://wicg.github.io/keyboard-map/#h-keyboard-getlayoutmap
    // GC::Ref<KeyboardLayoutMap> getLayoutMap();
    void set_onlayoutchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onlayoutchange();

private:
    Keyboard(JS::Realm& realm);
    AK::Vector<UIEvents::KeyCode> m_reserved_key_codes;
    bool m_enable_keyboard_lock = false;
    virtual void initialize(JS::Realm& realm) override;
};

}
