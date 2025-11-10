/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLSemanticsElement.h>

namespace Web::Layout {

class MathMLSemanticsBox final : public MathMLBox {
    GC_CELL(MathMLSemanticsBox, MathMLBox);

public:
    MathMLSemanticsBox(DOM::Document&, MathML::MathMLSemanticsElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLSemanticsBox() override = default;

    MathML::MathMLSemanticsElement& dom_node() { return static_cast<MathML::MathMLSemanticsElement&>(MathMLBox::dom_node()); }
    MathML::MathMLSemanticsElement const& dom_node() const { return static_cast<MathML::MathMLSemanticsElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_semantics_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLSemanticsBox>() const { return is_mathml_semantics_box(); }

}
