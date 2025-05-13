/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

struct CanvasRenderingContext2DSettings {
    bool alpha { true };
    bool desynchronized { false };
    Bindings::PredefinedColorSpace color_space { Bindings::PredefinedColorSpace::Srgb };
    Bindings::CanvasColorType color_type { Bindings::CanvasColorType::Unorm8 };
    bool will_read_frequently { false };
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
