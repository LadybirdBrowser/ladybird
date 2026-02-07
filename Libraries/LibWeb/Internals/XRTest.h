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
    WEB_PLATFORM_OBJECT(XRTest, InternalsBase);
    GC_DECLARE_ALLOCATOR(XRTest);

public:
    virtual ~XRTest() override;

    GC::Ref<WebIDL::Promise> simulate_device_connection(FakeXRDeviceInit const&) const;

    void simulate_user_activation(GC::Ref<WebIDL::CallbackType>) const;

    GC::Ref<WebIDL::Promise> disconnect_all_devices() const;

private:
    explicit XRTest(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
