/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>

namespace Web::WebGL {

class WebGLExtension : public Bindings::Wrappable {
    WEB_NON_IDL_WRAPPABLE(WebGLExtension, Bindings::Wrappable);

public:
    virtual ~WebGLExtension() override = default;

protected:
    WebGLExtension() = default;
};

}
