/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLFractionElement.h>

namespace Web::Layout {

class MathMLFractionBox final : public MathMLBox {
    GC_CELL(MathMLFractionBox, MathMLBox);

public:
    MathMLFractionBox(DOM::Document&, MathML::MathMLFractionElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLFractionBox() override = default;

    MathML::MathMLFractionElement& dom_node() { return static_cast<MathML::MathMLFractionElement&>(MathMLBox::dom_node()); }
    MathML::MathMLFractionElement const& dom_node() const { return static_cast<MathML::MathMLFractionElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_fraction_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLFractionBox>() const { return is_mathml_fraction_box(); }

}
