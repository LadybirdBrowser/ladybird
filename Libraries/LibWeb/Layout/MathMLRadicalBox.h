/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLRadicalElement.h>

namespace Web::Layout {

class MathMLRadicalBox final : public MathMLBox {
    GC_CELL(MathMLRadicalBox, MathMLBox);

public:
    MathMLRadicalBox(DOM::Document&, MathML::MathMLRadicalElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLRadicalBox() override = default;

    MathML::MathMLRadicalElement& dom_node() { return static_cast<MathML::MathMLRadicalElement&>(MathMLBox::dom_node()); }
    MathML::MathMLRadicalElement const& dom_node() const { return static_cast<MathML::MathMLRadicalElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_radical_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLRadicalBox>() const { return is_mathml_radical_box(); }

}
