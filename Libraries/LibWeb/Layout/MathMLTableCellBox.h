/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLTableCellElement.h>

namespace Web::Layout {

class MathMLTableCellBox final : public MathMLBox {
    GC_CELL(MathMLTableCellBox, MathMLBox);

public:
    MathMLTableCellBox(DOM::Document&, MathML::MathMLTableCellElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLTableCellBox() override = default;

    MathML::MathMLTableCellElement& dom_node() { return static_cast<MathML::MathMLTableCellElement&>(MathMLBox::dom_node()); }
    MathML::MathMLTableCellElement const& dom_node() const { return static_cast<MathML::MathMLTableCellElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_table_cell_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLTableCellBox>() const { return is_mathml_table_cell_box(); }

}
