/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/BufferedChangeEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/MediaSourceExtensions/BufferedChangeEvent.h>

namespace Web::MediaSourceExtensions {

JS_DEFINE_ALLOCATOR(BufferedChangeEvent);

WebIDL::ExceptionOr<JS::NonnullGCPtr<BufferedChangeEvent>> BufferedChangeEvent::construct_impl(JS::Realm& realm, AK::FlyString const& type, BufferedChangeEventInit const& event_init)
{
    return realm.heap().allocate<BufferedChangeEvent>(realm, realm, type, event_init);
}

BufferedChangeEvent::BufferedChangeEvent(JS::Realm& realm, AK::FlyString const& type, BufferedChangeEventInit const&)
    : DOM::Event(realm, type)
{
}

BufferedChangeEvent::~BufferedChangeEvent() = default;

void BufferedChangeEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BufferedChangeEvent);
}

}
