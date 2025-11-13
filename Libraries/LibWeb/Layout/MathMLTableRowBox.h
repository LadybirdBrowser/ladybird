/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLTableRowElement.h>

namespace Web::Layout {

class MathMLTableRowBox final : public MathMLBox {
    GC_CELL(MathMLTableRowBox, MathMLBox);

public:
    MathMLTableRowBox(DOM::Document&, MathML::MathMLTableRowElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLTableRowBox() override = default;

    MathML::MathMLTableRowElement& dom_node() { return static_cast<MathML::MathMLTableRowElement&>(MathMLBox::dom_node()); }
    MathML::MathMLTableRowElement const& dom_node() const { return static_cast<MathML::MathMLTableRowElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_table_row_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLTableRowBox>() const { return is_mathml_table_row_box(); }

}
