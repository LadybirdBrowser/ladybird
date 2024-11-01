/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#dom-sourcebufferlist
class SourceBufferList : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBufferList, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(SourceBufferList);

private:
    SourceBufferList(JS::Realm&);

    virtual ~SourceBufferList() override;

    virtual void initialize(JS::Realm&) override;
};

}
