/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLAnnotationXmlElement.h>

namespace Web::Layout {

class MathMLAnnotationXmlBox final : public MathMLBox {
    GC_CELL(MathMLAnnotationXmlBox, MathMLBox);

public:
    MathMLAnnotationXmlBox(DOM::Document&, MathML::MathMLAnnotationXmlElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLAnnotationXmlBox() override = default;

    MathML::MathMLAnnotationXmlElement& dom_node() { return static_cast<MathML::MathMLAnnotationXmlElement&>(MathMLBox::dom_node()); }
    MathML::MathMLAnnotationXmlElement const& dom_node() const { return static_cast<MathML::MathMLAnnotationXmlElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_annotation_xml_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLAnnotationXmlBox>() const { return is_mathml_annotation_xml_box(); }

}
