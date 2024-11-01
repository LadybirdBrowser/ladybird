/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#dom-mediasourcehandle
class MediaSourceHandle : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaSourceHandle, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(MediaSourceHandle);

public:
private:
    MediaSourceHandle(JS::Realm&);

    virtual ~MediaSourceHandle() override;

    virtual void initialize(JS::Realm&) override;
};

}
