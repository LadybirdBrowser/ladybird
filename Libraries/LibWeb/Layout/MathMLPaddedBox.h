/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLPaddedElement.h>

namespace Web::Layout {

class MathMLPaddedBox final : public MathMLBox {
    GC_CELL(MathMLPaddedBox, MathMLBox);

public:
    MathMLPaddedBox(DOM::Document&, MathML::MathMLPaddedElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLPaddedBox() override = default;

    MathML::MathMLPaddedElement& dom_node() { return static_cast<MathML::MathMLPaddedElement&>(MathMLBox::dom_node()); }
    MathML::MathMLPaddedElement const& dom_node() const { return static_cast<MathML::MathMLPaddedElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_padded_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLPaddedBox>() const { return is_mathml_padded_box(); }

}
