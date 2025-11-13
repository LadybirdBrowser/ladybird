/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLPhantomElement.h>

namespace Web::Layout {

class MathMLPhantomBox final : public MathMLBox {
    GC_CELL(MathMLPhantomBox, MathMLBox);

public:
    MathMLPhantomBox(DOM::Document&, MathML::MathMLPhantomElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLPhantomBox() override = default;

    MathML::MathMLPhantomElement& dom_node() { return static_cast<MathML::MathMLPhantomElement&>(MathMLBox::dom_node()); }
    MathML::MathMLPhantomElement const& dom_node() const { return static_cast<MathML::MathMLPhantomElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_phantom_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLPhantomBox>() const { return is_mathml_phantom_box(); }

}
