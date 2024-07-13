/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/BroadcastChannelPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(BroadcastChannel);

JS::NonnullGCPtr<BroadcastChannel> BroadcastChannel::construct_impl(JS::Realm& realm, FlyString const& name)
{
    return realm.heap().allocate<BroadcastChannel>(realm, realm, name);
}

BroadcastChannel::BroadcastChannel(JS::Realm& realm, FlyString const& name)
    : DOM::EventTarget(realm)
    , m_channel_name(name)
{
}

void BroadcastChannel::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BroadcastChannel);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-name
FlyString BroadcastChannel::name()
{
    // The name getter steps are to return this's channel name.
    return m_channel_name;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-close
void BroadcastChannel::close()
{
    // The close() method steps are to set this's closed flag to true.
    m_closed_flag = true;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessage
void BroadcastChannel::set_onmessage(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::message, event_handler);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessage
WebIDL::CallbackType* BroadcastChannel::onmessage()
{
    return event_handler_attribute(HTML::EventNames::message);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessageerror
void BroadcastChannel::set_onmessageerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::messageerror, event_handler);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessageerror
WebIDL::CallbackType* BroadcastChannel::onmessageerror()
{
    return event_handler_attribute(HTML::EventNames::messageerror);
}

}
