/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebGL {

class WebGLActiveInfo : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLActiveInfo, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(WebGLActiveInfo);

public:
    virtual ~WebGLActiveInfo();

protected:
    explicit WebGLActiveInfo(JS::Realm&);
};

}
