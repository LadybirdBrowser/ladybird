/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLBox.h>

namespace Web::Layout {

class MathMLRadicalBox final : public MathMLBox {
    GC_CELL(MathMLRadicalBox, MathMLBox);

public:
    MathMLRadicalBox(DOM::Document&, MathML::MathMLElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~MathMLRadicalBox() override = default;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_mathml_radical_box() const final override { return true; }
};

template<>
inline bool Node::fast_is<MathMLRadicalBox>() const { return is_mathml_radical_box(); }

}
