/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/XRTestPrototype.h>
#include <LibWeb/Internals/FakeXRDevice.h>
#include <LibWeb/Internals/XRTest.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(XRTest);

XRTest::XRTest(JS::Realm& realm)
    : InternalsBase(realm)
{
}

XRTest::~XRTest() = default;

void XRTest::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XRTest);
    Base::initialize(realm);
}

GC::Ref<WebIDL::Promise> XRTest::simulate_device_connection(FakeXRDeviceInit const&) const
{
    // Simulates connecting a device to the system.
    // Used to instantiate a fake device for use in tests.
    // FIXME: Actually perform whatever device connection steps are needed once those are implemented.
    auto& realm = HTML::relevant_realm(*this);
    auto promise = WebIDL::create_promise(realm);
    WebIDL::resolve_promise(realm, promise, FakeXRDevice::create(realm));
    return promise;
}

void XRTest::simulate_user_activation(GC::Ref<WebIDL::CallbackType> f) const
{
    (void)WebIDL::invoke_callback(*f, {}, {});
}

GC::Ref<WebIDL::Promise> XRTest::disconnect_all_devices() const
{
    // Disconnect all fake devices
    // FIXME: Actually disconnect fake devices once we have any.
    auto& realm = HTML::relevant_realm(*this);
    auto promise = WebIDL::create_promise(realm);
    WebIDL::resolve_promise(realm, promise);
    return promise;
}

}
