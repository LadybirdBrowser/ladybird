/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLTableElement.h>

namespace Web::Layout {

class MathMLTableBox final : public MathMLBox {
    GC_CELL(MathMLTableBox, MathMLBox);

public:
    MathMLTableBox(DOM::Document&, MathML::MathMLTableElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLTableBox() override = default;

    MathML::MathMLTableElement& dom_node() { return static_cast<MathML::MathMLTableElement&>(MathMLBox::dom_node()); }
    MathML::MathMLTableElement const& dom_node() const { return static_cast<MathML::MathMLTableElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_table_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLTableBox>() const { return is_mathml_table_box(); }

}
