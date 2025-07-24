/*
 * Copyright (c) 2025, Saksham Goyal <sakgoy2001@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/KeyboardPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventNames.h>
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
namespace Web::Keyboard {

GC_DEFINE_ALLOCATOR(Keyboard);
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
GC::Ref<WebIDL::Promise> Keyboard::lock(const AK::Vector<AK::String>& key_codes = {})
{
    // FIXME: This entire section is not correct. I just did the basic logic but this needs to be parallel
    // 1. Let p be a new promise in realm.
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    // 2. If not currently executing in the currently active top-level browsing context, then
    if (false /*!HTML::BrowsingContext::is_top_level()*/) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "not currently executing in the currently active top-level browsing context"_string));
        return promise;
    }
    // 3. Run the following steps in parallel:
    // 3.1
    m_reserved_key_codes.clear();
    // 3.2.1
    for (auto const& key : key_codes) {
        auto const code = UIEvents::key_code_from_string(key);
        if (code == UIEvents::KeyCode::Key_Invalid) {
            m_enable_keyboard_lock = false;
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Invalid Key Code"_string));
            return promise;
        }
        m_reserved_key_codes.append(code);
    }
    // 3.3
    if (!m_enable_keyboard_lock) {
        // FIXME 3.3.1
        m_enable_keyboard_lock = true;
    }
    // FIXME 3.4
    WebIDL::resolve_promise(realm, promise, JS::js_undefined());
    return promise;
}
void Keyboard::unlock()
{
    if (m_enable_keyboard_lock) {
        // FIXME 1.1.1
        m_enable_keyboard_lock = false;
        m_reserved_key_codes.clear();
    }
}

WebIDL::CallbackType* Keyboard::onlayoutchange()
{
    return event_handler_attribute(HTML::EventNames::layoutchange);
}

void Keyboard::set_onlayoutchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::layoutchange, event_handler);
}

Keyboard::~Keyboard() { unlock(); }

}
