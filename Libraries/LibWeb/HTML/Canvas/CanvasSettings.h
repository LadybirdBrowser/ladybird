/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>

// FIXME: this needs to be shared inside the idl, this is not that easily fixable, so until that we are just using the definition of ImageDataPrototype and CanvasRenderingContext2DPrototype, and assert that it's the same in all duplicate declarations
#include <LibWeb/Bindings/CanvasRenderingContext2DPrototype.h>
#include <LibWeb/Bindings/ImageDataPrototype.h>

namespace Web::HTML {

struct CanvasRenderingContext2DSettings {
    bool alpha { true };
    bool desynchronized { false };
    Web::Bindings::PredefinedColorSpace color_space { Web::Bindings::PredefinedColorSpace::Srgb };
    Web::Bindings::CanvasColorType color_type { Web::Bindings::CanvasColorType::Unorm8 };
    bool will_read_frequently { false };

    [[nodiscard]] static JS::ThrowCompletionOr<CanvasRenderingContext2DSettings> from_js_value(JS::VM&, JS::Value);
};

// https://html.spec.whatwg.org/multipage/canvas.html#canvassettings
class CanvasSettings {
public:
    virtual ~CanvasSettings() = default;

    virtual CanvasRenderingContext2DSettings get_context_attributes() const = 0;

protected:
    CanvasSettings() = default;
};

}
