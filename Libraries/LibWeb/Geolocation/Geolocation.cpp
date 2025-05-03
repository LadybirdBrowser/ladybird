/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/GeolocationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geolocation/Geolocation.h>

namespace Web::Geolocation {

GC_DEFINE_ALLOCATOR(Geolocation);

GC::Ref<Geolocation> Geolocation::create(JS::Realm& realm)
{
    return realm.create<Geolocation>(realm);
}

Geolocation::Geolocation(JS::Realm& realm)
    : PlatformObject(realm)
{
}

Geolocation::~Geolocation() = default;

void Geolocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Geolocation);
    Base::initialize(realm);
}

// https://w3c.github.io/geolocation/#getcurrentposition-method
void Geolocation::get_current_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback, Optional<PositionOptions> options)
{
    // FIXME: WIP
    (void)success_callback;
    (void)error_callback;
    (void)options;
}

// https://w3c.github.io/geolocation/#watchposition-method
WebIDL::Long Geolocation::watch_position(WebIDL::CallbackType& success_callback, WebIDL::CallbackType* error_callback, Optional<PositionOptions> options)
{
    // FIXME: WIP
    (void)success_callback;
    (void)error_callback;
    (void)options;
    return 0;
}

// https://w3c.github.io/geolocation/#clearwatch-method
void Geolocation::clear_watch(WebIDL::Long watch_id)
{
    // FIXME: WIP
    (void)watch_id;
}

}
