/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/FakeXRDevice.h>
#include <LibWeb/Internals/FakeXRDevice.h>
#include <LibWeb/Internals/InternalsBase.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Internals {

using FakeXRDeviceInit = Bindings::FakeXRDeviceInit;

// https://github.com/immersive-web/webxr-test-api/blob/main/explainer.md
class WEB_API XRTest final : public InternalsBase {
    WEB_WRAPPABLE(XRTest, InternalsBase);
    GC_DECLARE_ALLOCATOR(XRTest);

public:
    static GC::Ref<XRTest> create(HTML::Window&);

    virtual ~XRTest() override;

    void simulate_device_connection(JS::Realm&, FakeXRDeviceInit const&, GC::Ref<WebIDL::Promise>) const;
    WebIDL::ExceptionOr<void> simulate_user_activation(WebIDL::CallbackType&) const;

    void disconnect_all_devices(GC::Ref<WebIDL::Promise>) const;

private:
    explicit XRTest(HTML::Window&);
};

}
