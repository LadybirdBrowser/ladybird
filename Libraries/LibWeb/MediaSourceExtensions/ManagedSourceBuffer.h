/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#managedsourcebuffer-interface
class ManagedSourceBuffer : public SourceBuffer {
    WEB_PLATFORM_OBJECT(ManagedSourceBuffer, SourceBuffer);
    JS_DECLARE_ALLOCATOR(ManagedSourceBuffer);

public:
private:
    ManagedSourceBuffer(JS::Realm&);

    virtual ~ManagedSourceBuffer() override;

    virtual void initialize(JS::Realm&) override;
};

}
