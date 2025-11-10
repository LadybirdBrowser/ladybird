/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLErrorElement.h>

namespace Web::Layout {

class MathMLErrorBox final : public MathMLBox {
    GC_CELL(MathMLErrorBox, MathMLBox);

public:
    MathMLErrorBox(DOM::Document&, MathML::MathMLErrorElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLErrorBox() override = default;

    MathML::MathMLErrorElement& dom_node() { return static_cast<MathML::MathMLErrorElement&>(MathMLBox::dom_node()); }
    MathML::MathMLErrorElement const& dom_node() const { return static_cast<MathML::MathMLErrorElement const&>(MathMLBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_error_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLErrorBox>() const { return is_mathml_error_box(); }

}
