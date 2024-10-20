#include "LibWeb/HTML/EventNames.h"
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/KeyboardPrototype.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Keyboard/Keyboard.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>
#include "LibWeb/DOM/EventTarget.h"

namespace Web::Keyboard {
JS_DEFINE_ALLOCATOR(Keyboard);

Keyboard::Keyboard(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void Keyboard::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Keyboard);
}
// https://wicg.github.io/keyboard-lock/#keyboard-lock
JS::NonnullGCPtr<JS::Promise> Keyboard::lock(const AK::Vector<AK::String>& key_codes = {})
{
    // TODO FIXME: This entire section is not correct. I just did the basic logic but this needs to be parallel

    // 1. Let p be a new promise in realm.
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);

    // 2. If not currently executing in the currently active top-level browsing context, then
    // if (!HTML::BrowsingContext::top_level_browsing_context()) {
    //     // 2.1
    //     return WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "not currently executing in the currently active top-level browsing context"_fly_string)));
    // }

    // 3. Run the following steps in parallel:
    // 3.1
    m_reserved_key_codes = {};
    // 3.2
    if (!key_codes.is_empty()) {
        // 3.2.1
        for (auto const& key : key_codes) {
            auto const code = UIEvents::key_code_from_string(key);
            // 3.2.1.1
            if (code == UIEvents::KeyCode::Key_Invalid) {
                // 3.2.1.1.1
                m_enable_keyboard_lock = false;
                // 3.2.1.1.2
                WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Invalid Key Code"_string));
            }
            // 3.2.1.2
            m_reserved_key_codes.append(code);
        }
    }
    // 3.3
    if (m_enable_keyboard_lock) {
        // 3.3.1
        // TODO FIXME
        // 3.3.2
        m_enable_keyboard_lock = true;
    }
    // 3.4
    // TODO FIXME

    // 3.5
    WebIDL::resolve_promise(realm, promise, JS::js_undefined());

    // 4. Return p.
    return verify_cast<JS::Promise>(*promise->promise());
}

void Keyboard::unlock()
{
    // TODO FIXME 1.

    // 1.1
    if (m_enable_keyboard_lock) {
        // TODO FIXME 1.1.1
        // 1.1.2
        m_enable_keyboard_lock = false;
        // 1.1.2
        m_reserved_key_codes = {};
    }
}

// JS::GCPtr<WebIDL::CallbackType> onlayoutchange()
// {
//     return event_handler_attribute(HTML::EventNames::layoutchange);
// }

Keyboard::~Keyboard() {unlock();};

// JS::NonnullGCPtr<KeyboardLayoutMap> Keyboard::getLayoutMap() {}

}
