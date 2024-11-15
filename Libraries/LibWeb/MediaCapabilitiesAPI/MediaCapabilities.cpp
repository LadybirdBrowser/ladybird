/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaCapabilitiesPrototype.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>

namespace Web::MediaCapabilitiesAPI {

GC_DEFINE_ALLOCATOR(MediaCapabilities);

GC::Ref<MediaCapabilities> MediaCapabilities::create(JS::Realm& realm)
{
    return realm.create<MediaCapabilities>(realm);
}

MediaCapabilities::MediaCapabilities(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void MediaCapabilities::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaCapabilities);
}

}
