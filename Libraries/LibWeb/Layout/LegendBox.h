/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class LegendBox final : public BlockContainer {
    LAYOUT_NODE(LegendBox, BlockContainer);

public:
    LegendBox(DOM::Document&, DOM::Element&, CSS::ComputedProperties const&);
    virtual ~LegendBox() override;

    DOM::Element& dom_node() { return static_cast<DOM::Element&>(*Box::dom_node()); }
    DOM::Element const& dom_node() const { return static_cast<DOM::Element const&>(*Box::dom_node()); }

private:
    virtual bool is_legend_box() const final
    {
        return true;
    }
};

template<>
inline bool Node::fast_is<LegendBox>() const { return is_legend_box(); }

}
