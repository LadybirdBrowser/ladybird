/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/FakeXRDevice.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Bindings/XRTest.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/FakeXRDevice.h>
#include <LibWeb/Internals/XRTest.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(XRTest);

GC::Ref<XRTest> XRTest::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<XRTest>(window);
}

XRTest::XRTest(HTML::Window& window)
    : InternalsBase(window)
{
}

XRTest::~XRTest() = default;

GC::Ref<WebIDL::Promise> XRTest::simulate_device_connection(Bindings::FakeXRDeviceInit const&) const
{
    // Simulates connecting a device to the system.
    // Used to instantiate a fake device for use in tests.
    // FIXME: Actually perform whatever device connection steps are needed once those are implemented.
    auto& realm = window().realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::resolve_promise(realm, promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, FakeXRDevice::create(window())));
    return promise;
}

void XRTest::simulate_user_activation(GC::Ref<WebIDL::CallbackType> function) const
{
    // Simulates a user activation (aka user gesture) for the current scope.
    // The activation is only guaranteed to be valid in the provided function and only applies to WebXR
    // Device API methods.
    // FIXME: Actually simulate a user activation here
    (void)WebIDL::invoke_callback(*function, {}, {});
}

GC::Ref<WebIDL::Promise> XRTest::disconnect_all_devices() const
{
    // Disconnect all fake devices
    // FIXME: Actually disconnect fake devices once we have any.
    auto& realm = window().realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::resolve_promise(realm, promise);
    return promise;
}

}
