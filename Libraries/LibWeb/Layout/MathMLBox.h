/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/MathML/MathMLElement.h>

namespace Web::Layout {

class MathMLBox : public BlockContainer {
    GC_CELL(MathMLBox, BlockContainer);

public:
    MathMLBox(DOM::Document&, MathML::MathMLElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLBox() override = default;

    MathML::MathMLElement& dom_node() { return as<MathML::MathMLElement>(*BlockContainer::dom_node()); }
    MathML::MathMLElement const& dom_node() const { return as<MathML::MathMLElement>(*BlockContainer::dom_node()); }

private:
    virtual bool is_mathml_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLBox>() const { return is_mathml_box(); }

}
