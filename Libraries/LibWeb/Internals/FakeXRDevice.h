/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Internals/InternalsBase.h>
#include <LibWeb/WebXR/XRSession.h>

namespace Web::Internals {

struct FakeXRDeviceInit : public JS::Cell {
    GC_CELL(FakeXRDeviceInit, JS::Cell)
    GC_DECLARE_ALLOCATOR(FakeXRDeviceInit);

    Optional<Vector<Bindings::XRSessionMode>> supported_modes;

    Optional<Vector<String>> supported_features;
};

// https://github.com/immersive-web/webxr-test-api/blob/main/explainer.md
class WEB_API FakeXRDevice final : public InternalsBase {
    WEB_PLATFORM_OBJECT(FakeXRDevice, InternalsBase);
    GC_DECLARE_ALLOCATOR(FakeXRDevice);

public:
    static GC::Ref<FakeXRDevice> create(JS::Realm&);
    virtual ~FakeXRDevice() override;

    void simulate_user_activation(GC::Ref<WebIDL::CallbackType>) const;

    GC::Ref<WebIDL::Promise> disconnect() const;

private:
    explicit FakeXRDevice(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
