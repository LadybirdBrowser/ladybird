/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLPrescriptsBox.h>
#include <LibWeb/MathML/MathMLPrescriptsElement.h>

namespace Web::Layout {

MathMLPrescriptsBox::MathMLPrescriptsBox(DOM::Document& document, MathML::MathMLPrescriptsElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

}
