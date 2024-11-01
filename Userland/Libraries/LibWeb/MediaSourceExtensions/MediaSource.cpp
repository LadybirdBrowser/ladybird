/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>

namespace Web::MediaSourceExtensions {

JS_DEFINE_ALLOCATOR(MediaSource);

WebIDL::ExceptionOr<JS::NonnullGCPtr<MediaSource>> MediaSource::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<MediaSource>(realm, realm);
}

MediaSource::MediaSource(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

MediaSource::~MediaSource() = default;

void MediaSource::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSource);
}

}
