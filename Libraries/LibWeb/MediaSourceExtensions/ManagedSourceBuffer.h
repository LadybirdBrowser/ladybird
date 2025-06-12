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
    GC_DECLARE_ALLOCATOR(ManagedSourceBuffer);

public:
    void set_onbufferedchange(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onbufferedchange();

private:
    ManagedSourceBuffer(JS::Realm&);

    virtual ~ManagedSourceBuffer() override;

    virtual void initialize(JS::Realm&) override;
};

}
