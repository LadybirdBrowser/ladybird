/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibGfx/Color.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvascompositing

// https://drafts.fxtf.org/compositing/#ltblendmodegt
// https://drafts.fxtf.org/compositing/#compositemode
#define ENUMERATE_GLOBAL_COMPOSITE_OPERATIONS                                            \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Normal, "normal", SrcOver)                    \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Multiply, "multiply", Multiply)               \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Screen, "screen", Screen)                     \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Overlay, "overlay", Overlay)                  \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Darken, "darken", Darken)                     \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Lighten, "lighten", Lighten)                  \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(ColorDodge, "color-dodge", ColorDodge)        \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(ColorBurn, "color-burn", ColorBurn)           \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(HardLight, "hard-light", HardLight)           \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(SoftLight, "soft-light", SoftLight)           \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Difference, "difference", Difference)         \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Exclusion, "exclusion", Exclusion)            \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Hue, "hue", Hue)                              \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Saturation, "saturation", Saturation)         \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Color, "color", Color)                        \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Luminosity, "luminosity", Luminosity)         \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Clear, "clear", Clear)                        \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Copy, "copy", Src)                            \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(SourceOver, "source-over", SrcOver)           \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(DestinationOver, "destination-over", DstOver) \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(SourceIn, "source-in", SrcIn)                 \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(DestinationIn, "destination-in", DstIn)       \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(SourceOut, "source-out", SrcOut)              \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(DestinationOut, "destination-out", DstOut)    \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(SourceAtop, "source-atop", SrcATop)           \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(DestinationAtop, "destination-atop", DstATop) \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Xor, "xor", Xor)                              \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(Lighter, "lighter", Plus)                     \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(PlusDarker, "plus-darker", SrcOver)           \
    __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(PlusLighter, "plus-lighter", SrcOver)

enum class GlobalCompositeOperation {
#define __ENUMERATE_GLOBAL_COMPOSITE_OPERATION(name, text, gfx_name) name,
    ENUMERATE_GLOBAL_COMPOSITE_OPERATIONS
#undef __ENUMERATE_GLOBAL_COMPOSITE_OPERATION
};

class CanvasCompositing {
public:
    virtual ~CanvasCompositing() = default;

    virtual float global_alpha() const = 0;
    virtual void set_global_alpha(float) = 0;

    virtual String global_composite_operation() const = 0;
    virtual void set_global_composite_operation(String composite_operation) = 0;
protected:
    CanvasCompositing() = default;
};

}
