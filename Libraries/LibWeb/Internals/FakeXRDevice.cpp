/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/FakeXRDevice.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/FakeXRDevice.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(FakeXRDevice);

GC::Ref<FakeXRDevice> FakeXRDevice::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<FakeXRDevice>(window);
}

FakeXRDevice::FakeXRDevice(HTML::Window& window)
    : InternalsBase(window)
{
}

FakeXRDevice::~FakeXRDevice() = default;

GC::Ref<WebIDL::Promise> FakeXRDevice::disconnect() const
{
    // behaves as if device was disconnected
    // FIXME: Implement this once we have actual devices that can disconnect.
    auto& realm = window().realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::resolve_promise(realm, promise);
    return promise;
}

}
