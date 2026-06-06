/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Internals/FakeXRDevice.h>
#include <LibWeb/Internals/InternalsBase.h>

namespace Web::Internals {

// https://github.com/immersive-web/webxr-test-api/blob/main/explainer.md
class WEB_API XRTest final : public InternalsBase {
    WEB_WRAPPABLE(XRTest, InternalsBase);
    GC_DECLARE_ALLOCATOR(XRTest);

public:
    static GC::Ref<XRTest> create(HTML::Window&);

    virtual ~XRTest() override;

    GC::Ref<WebIDL::Promise> simulate_device_connection(Bindings::FakeXRDeviceInit const&) const;

    void simulate_user_activation(GC::Ref<WebIDL::CallbackType>) const;

    GC::Ref<WebIDL::Promise> disconnect_all_devices() const;

private:
    explicit XRTest(HTML::Window&);
};

}
