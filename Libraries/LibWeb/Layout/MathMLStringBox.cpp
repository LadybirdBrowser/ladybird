/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLStringBox.h>
#include <LibWeb/MathML/MathMLStringElement.h>

namespace Web::Layout {

MathMLStringBox::MathMLStringBox(DOM::Document& document, MathML::MathMLStringElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
