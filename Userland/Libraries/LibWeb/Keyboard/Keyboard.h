#pragma once

#include "AK/Vector.h"
#include "LibWeb/HTML/DOMStringList.h"
#include "LibWeb/UIEvents/KeyCode.h"
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Keyboard {

class Keyboard final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Keyboard, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(Keyboard);

public:
    virtual ~Keyboard() override = default;

    // JS::NonnullGCPtr<JS::Promise>
    // https://wicg.github.io/keyboard-lock/#keyboard-lock
    WebIDL::ExceptionOr<JS::Promise> lock(const AK::Vector<AK::String>& key_codes);

    void unlock() { }

private:
    Keyboard(JS::Realm& realm);

    AK::Vector<UIEvents::KeyCode> m_reserved_key_codes;

    bool m_enable_keyboard_lock = false;

    virtual void initialize(JS::Realm& realm) override;
};
}
