/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/MathMLBox.h>

namespace Web::Layout {

class MathMLFractionBox final : public MathMLBox {
    GC_CELL(MathMLFractionBox, MathMLBox);

public:
    MathMLFractionBox(DOM::Document&, MathML::MathMLElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLFractionBox() override = default;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_fraction_box() const final override { return true; }
};

template<>
inline bool Node::fast_is<MathMLFractionBox>() const { return is_mathml_fraction_box(); }

}
