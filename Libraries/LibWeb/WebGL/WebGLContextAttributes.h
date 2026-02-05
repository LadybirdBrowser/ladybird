/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/WebGLRenderingContextPrototype.h>

namespace Web::WebGL {

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#WEBGLCONTEXTATTRIBUTES
struct WebGLContextAttributes {
    bool alpha { true };
    bool depth { true };
    bool stencil { false };
    bool antialias { true };
    bool premultiplied_alpha { true };
    bool preserve_drawing_buffer { false };
    Bindings::WebGLPowerPreference power_preference { Bindings::WebGLPowerPreference::Default };
    bool fail_if_major_performance_caveat { false };
    bool desynchronized { false };
};

JS::ThrowCompletionOr<WebGLContextAttributes> convert_value_to_context_attributes_dictionary(JS::VM&, JS::Value value);

}
