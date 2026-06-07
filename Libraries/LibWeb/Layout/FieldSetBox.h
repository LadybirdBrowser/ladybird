/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/LegendBox.h>
#include <LibWeb/Painting/FieldSetPaintable.h>
namespace Web::Layout {

class FieldSetBox final : public BlockContainer {
    LAYOUT_NODE(FieldSetBox, BlockContainer);

public:
    FieldSetBox(DOM::Document&, DOM::Element&, CSS::ComputedProperties const&);
    virtual ~FieldSetBox() override;

    DOM::Element& dom_node() { return static_cast<DOM::Element&>(*BlockContainer::dom_node()); }
    DOM::Element const& dom_node() const { return static_cast<DOM::Element const&>(*BlockContainer::dom_node()); }

    GC::Ptr<LegendBox const> rendered_legend() const;
    virtual RefPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_fieldset_box() const final
    {
        return true;
    }
};

template<>
inline bool Node::fast_is<FieldSetBox>() const { return is_fieldset_box(); }

}
