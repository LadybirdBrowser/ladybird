/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/FakeXRDevice.h>
#include <LibWeb/Internals/FakeXRDevice.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(FakeXRDevice);

GC::Ref<FakeXRDevice> FakeXRDevice::create(JS::Realm& realm)
{
    return realm.create<FakeXRDevice>(realm);
}

FakeXRDevice::FakeXRDevice(JS::Realm& realm)
    : InternalsBase(realm)
{
}

FakeXRDevice::~FakeXRDevice() = default;

GC::Ref<WebIDL::Promise> FakeXRDevice::disconnect() const
{
    // behaves as if device was disconnected
    // FIXME: Implement this once we have actual devices that can disconnect.
    auto& realm = HTML::relevant_realm(*this);
    auto promise = WebIDL::create_promise(realm);
    WebIDL::resolve_promise(realm, promise);
    return promise;
}

}
