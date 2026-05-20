/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CanvasRenderingContext2DSettings.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvassettings
class CanvasSettings {
public:
    virtual ~CanvasSettings() = default;

    virtual Bindings::CanvasRenderingContext2DSettings get_context_attributes() const = 0;

protected:
    CanvasSettings() = default;
};

}
