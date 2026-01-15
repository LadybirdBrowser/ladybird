/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasCompositing.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>

namespace Web::HTML {

float CanvasCompositing::global_alpha() const
{
    return drawing_state().global_alpha;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
void CanvasCompositing::set_global_alpha(float alpha)
{
    // 1. If the given value is either infinite, NaN, or not in the range 0.0 to 1.0, then return.
    if (!isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return;
    }
    // 2. Otherwise, set this's global alpha to the given value.
    drawing_state().global_alpha = alpha;
}

#define ENUMERATE_COMPOSITE_OPERATIONS(E)  \
    E("normal", Normal)                    \
    E("multiply", Multiply)                \
    E("screen", Screen)                    \
    E("overlay", Overlay)                  \
    E("darken", Darken)                    \
    E("lighten", Lighten)                  \
    E("color-dodge", ColorDodge)           \
    E("color-burn", ColorBurn)             \
    E("hard-light", HardLight)             \
    E("soft-light", SoftLight)             \
    E("difference", Difference)            \
    E("exclusion", Exclusion)              \
    E("hue", Hue)                          \
    E("saturation", Saturation)            \
    E("color", Color)                      \
    E("luminosity", Luminosity)            \
    E("clear", Clear)                      \
    E("copy", Copy)                        \
    E("source-over", SourceOver)           \
    E("destination-over", DestinationOver) \
    E("source-in", SourceIn)               \
    E("destination-in", DestinationIn)     \
    E("source-out", SourceOut)             \
    E("destination-out", DestinationOut)   \
    E("source-atop", SourceATop)           \
    E("destination-atop", DestinationATop) \
    E("xor", Xor)                          \
    E("lighter", Lighter)                  \
    E("plus-darker", PlusDarker)           \
    E("plus-lighter", PlusLighter)

String CanvasCompositing::global_composite_operation() const
{
    auto current_compositing_and_blending_operator = drawing_state().current_compositing_and_blending_operator;
    switch (current_compositing_and_blending_operator) {
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                \
    case Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator: \
        return operation##_string;
        ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
void CanvasCompositing::set_global_composite_operation(String global_composite_operation)
{
    // 1. If the given value is not identical to any of the values that the <blend-mode> or the <composite-mode> properties are defined to take, then return.
    // 2. Otherwise, set this's current compositing and blending operator to the given value.
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                                                                           \
    if (global_composite_operation == operation##sv) {                                                                                      \
        drawing_state().current_compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator; \
        return;                                                                                                                             \
    }
    ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
}

}
