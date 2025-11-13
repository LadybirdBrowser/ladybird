/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLAnnotationXmlBox.h>
#include <LibWeb/MathML/MathMLAnnotationXmlElement.h>

namespace Web::Layout {

MathMLAnnotationXmlBox::MathMLAnnotationXmlBox(DOM::Document& document, MathML::MathMLAnnotationXmlElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
