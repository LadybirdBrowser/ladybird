/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLAnnotationBox.h>
#include <LibWeb/MathML/MathMLAnnotationElement.h>

namespace Web::Layout {

MathMLAnnotationBox::MathMLAnnotationBox(DOM::Document& document, MathML::MathMLAnnotationElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
