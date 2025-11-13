/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLUnderOverElement.h>

namespace Web::Layout {

class MathMLUnderOverBox final : public MathMLBox {
    GC_CELL(MathMLUnderOverBox, MathMLBox);

public:
    MathMLUnderOverBox(DOM::Document&, MathML::MathMLUnderOverElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLUnderOverBox() override = default;

    MathML::MathMLUnderOverElement& dom_node() { return static_cast<MathML::MathMLUnderOverElement&>(MathMLBox::dom_node()); }
    MathML::MathMLUnderOverElement const& dom_node() const { return static_cast<MathML::MathMLUnderOverElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_underover_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLUnderOverBox>() const { return is_mathml_underover_box(); }

}
