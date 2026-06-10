/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/FakeXRDevice.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
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

void XRTest::simulate_device_connection(JS::Realm& realm, FakeXRDeviceInit const&, GC::Ref<WebIDL::Promise> promise) const
{
    // Simulates connecting a device to the system.
    // Used to instantiate a fake device for use in tests.
    // FIXME: Actually perform whatever device connection steps are needed once those are implemented.
    auto fake_device = FakeXRDevice::create(window());
    WebIDL::resolve_promise(realm, promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, fake_device));
}

WebIDL::ExceptionOr<void> XRTest::simulate_user_activation(WebIDL::CallbackType& function) const
{
    // FIXME: Actually simulate a user activation here.
    TRY(WebIDL::invoke_callback(function, {}, {}));
    return {};
}

void XRTest::disconnect_all_devices(GC::Ref<WebIDL::Promise> promise) const
{
    // Disconnect all fake devices
    // FIXME: Actually disconnect fake devices once we have any.
    auto& realm = window().realm();
    WebIDL::resolve_promise(realm, promise);
}

}
