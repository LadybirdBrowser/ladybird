/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLMultiscriptsElement.h>

namespace Web::Layout {

class MathMLMultiscriptsBox final : public MathMLBox {
    GC_CELL(MathMLMultiscriptsBox, MathMLBox);

public:
    MathMLMultiscriptsBox(DOM::Document&, MathML::MathMLMultiscriptsElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLMultiscriptsBox() override = default;

    MathML::MathMLMultiscriptsElement& dom_node() { return static_cast<MathML::MathMLMultiscriptsElement&>(MathMLBox::dom_node()); }
    MathML::MathMLMultiscriptsElement const& dom_node() const { return static_cast<MathML::MathMLMultiscriptsElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_multiscripts_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLMultiscriptsBox>() const { return is_mathml_multiscripts_box(); }

}
