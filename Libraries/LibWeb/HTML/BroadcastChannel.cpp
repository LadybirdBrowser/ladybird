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

GC_DEFINE_ALLOCATOR(BroadcastChannel);

GC::Ref<BroadcastChannel> BroadcastChannel::construct_impl(JS::Realm& realm, FlyString const& name)
{
    return realm.create<BroadcastChannel>(realm, name);
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

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-close
void BroadcastChannel::close()
{
    // The close() method steps are to set this's closed flag to true.
    m_closed_flag = true;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessage
void BroadcastChannel::set_onmessage(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::message, event_handler);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessage
GC::Ptr<WebIDL::CallbackType> BroadcastChannel::onmessage()
{
    return event_handler_attribute(HTML::EventNames::message);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessageerror
void BroadcastChannel::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::messageerror, event_handler);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-broadcastchannel-onmessageerror
GC::Ptr<WebIDL::CallbackType> BroadcastChannel::onmessageerror()
{
    return event_handler_attribute(HTML::EventNames::messageerror);
}

}
