/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLStyleElement.h>

namespace Web::Layout {

class MathMLStyleBox final : public MathMLBox {
    GC_CELL(MathMLStyleBox, MathMLBox);

public:
    MathMLStyleBox(DOM::Document&, MathML::MathMLStyleElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLStyleBox() override = default;

    MathML::MathMLStyleElement& dom_node() { return static_cast<MathML::MathMLStyleElement&>(MathMLBox::dom_node()); }
    MathML::MathMLStyleElement const& dom_node() const { return static_cast<MathML::MathMLStyleElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_style_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLStyleBox>() const { return is_mathml_style_box(); }

}
