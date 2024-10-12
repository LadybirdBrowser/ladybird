#pragma once

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
    static WebIDL::ExceptionOr<JS::Promise> lock(AK::Vector<AK::String> key_codes);

    void unlock() {}

private:
    Keyboard(JS::Realm& realm);

    virtual void initialize(JS::Realm& realm) override;
};
}
