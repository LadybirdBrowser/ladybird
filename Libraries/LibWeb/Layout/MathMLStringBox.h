/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLStringElement.h>

namespace Web::Layout {

class MathMLStringBox final : public MathMLBox {
    GC_CELL(MathMLStringBox, MathMLBox);

public:
    MathMLStringBox(DOM::Document&, MathML::MathMLStringElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLStringBox() override = default;

    MathML::MathMLStringElement& dom_node() { return static_cast<MathML::MathMLStringElement&>(MathMLBox::dom_node()); }
    MathML::MathMLStringElement const& dom_node() const { return static_cast<MathML::MathMLStringElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_string_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLStringBox>() const { return is_mathml_string_box(); }

}
