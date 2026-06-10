/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#dom-mediasourcehandle
class MediaSourceHandle : public Bindings::Wrappable {
    WEB_WRAPPABLE(MediaSourceHandle, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MediaSourceHandle);

public:
private:
    MediaSourceHandle();

    virtual ~MediaSourceHandle() override;
};

}
