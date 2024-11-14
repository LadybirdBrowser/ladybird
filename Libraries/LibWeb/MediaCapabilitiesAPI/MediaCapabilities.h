/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::MediaCapabilitiesAPI {

class MediaCapabilities final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaCapabilities, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaCapabilities);

public:
    static GC::Ref<MediaCapabilities> create(JS::Realm&);
    virtual ~MediaCapabilities() override = default;

private:
    MediaCapabilities(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
