/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#dom-sourcebuffer
class SourceBuffer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBuffer, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(SourceBuffer);

protected:
    SourceBuffer(JS::Realm&);

    virtual ~SourceBuffer() override;

    virtual void initialize(JS::Realm&) override;

private:
};

}
