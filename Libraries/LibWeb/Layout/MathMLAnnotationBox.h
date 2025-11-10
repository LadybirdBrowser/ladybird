/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLAnnotationElement.h>

namespace Web::Layout {

class MathMLAnnotationBox final : public MathMLBox {
    GC_CELL(MathMLAnnotationBox, MathMLBox);

public:
    MathMLAnnotationBox(DOM::Document&, MathML::MathMLAnnotationElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLAnnotationBox() override = default;

    MathML::MathMLAnnotationElement& dom_node() { return static_cast<MathML::MathMLAnnotationElement&>(MathMLBox::dom_node()); }
    MathML::MathMLAnnotationElement const& dom_node() const { return static_cast<MathML::MathMLAnnotationElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_annotation_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLAnnotationBox>() const { return is_mathml_annotation_box(); }

}
