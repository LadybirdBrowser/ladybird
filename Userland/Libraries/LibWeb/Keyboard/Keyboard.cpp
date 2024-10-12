#include <LibJS/Runtime/Realm.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/KeyboardPrototype.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Keyboard/Keyboard.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

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
static WebIDL::ExceptionOr<JS::Promise> lock(AK::Vector<AK::String> key_codes)
{
    // auto& realm = HTML::relevant_realm(*this);
    // auto promise = WebIDL::create_promise(realm);
    // return JS::throw_completion(WebIDL::InvalidStateError::create(realm, "not currently executing in the currently active top-level browsing context"_fly_string));

    // return JS::NonnullGCPtr { verify_cast<JS::Promise>(*promise->promise()) };
}

}
