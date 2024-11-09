/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSourceHandlePrototype.h>
#include <LibWeb/MediaSourceExtensions/MediaSourceHandle.h>

namespace Web::MediaSourceExtensions {

JS_DEFINE_ALLOCATOR(MediaSourceHandle);

MediaSourceHandle::MediaSourceHandle(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

MediaSourceHandle::~MediaSourceHandle() = default;

void MediaSourceHandle::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSourceHandle);
}

}
