/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/BufferedChangeEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/MediaSourceExtensions/BufferedChangeEvent.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(BufferedChangeEvent);

WebIDL::ExceptionOr<GC::Ref<BufferedChangeEvent>> BufferedChangeEvent::construct_impl(JS::Realm& realm, AK::FlyString const& type, BufferedChangeEventInit const& event_init)
{
    return realm.create<BufferedChangeEvent>(realm, type, event_init);
}

BufferedChangeEvent::BufferedChangeEvent(JS::Realm& realm, AK::FlyString const& type, BufferedChangeEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
{
}

BufferedChangeEvent::~BufferedChangeEvent() = default;

void BufferedChangeEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BufferedChangeEvent);
}

}
