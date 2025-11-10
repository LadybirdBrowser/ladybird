/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>
#include <LibWeb/MathML/MathMLPrescriptsElement.h>

namespace Web::Layout {

class MathMLPrescriptsBox final : public MathMLBox {
    GC_CELL(MathMLPrescriptsBox, MathMLBox);

public:
    MathMLPrescriptsBox(DOM::Document&, MathML::MathMLPrescriptsElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLPrescriptsBox() override = default;

    MathML::MathMLPrescriptsElement& dom_node() { return static_cast<MathML::MathMLPrescriptsElement&>(MathMLBox::dom_node()); }
    MathML::MathMLPrescriptsElement const& dom_node() const { return static_cast<MathML::MathMLPrescriptsElement const&>(MathMLBox::dom_node()); }

private:
    virtual bool is_mathml_prescripts_box() const final { return true; }
};

template<>
inline bool Node::fast_is<MathMLPrescriptsBox>() const { return is_mathml_prescripts_box(); }

}
