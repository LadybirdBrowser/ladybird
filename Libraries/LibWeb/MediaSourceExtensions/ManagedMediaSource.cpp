/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ManagedMediaSourcePrototype.h>
#include <LibWeb/MediaSourceExtensions/ManagedMediaSource.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(ManagedMediaSource);

WebIDL::ExceptionOr<GC::Ref<ManagedMediaSource>> ManagedMediaSource::construct_impl(JS::Realm& realm)
{
    return realm.create<ManagedMediaSource>(realm);
}

ManagedMediaSource::ManagedMediaSource(JS::Realm& realm)
    : MediaSource(realm)
{
}

ManagedMediaSource::~ManagedMediaSource() = default;

void ManagedMediaSource::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ManagedMediaSource);
}

}
