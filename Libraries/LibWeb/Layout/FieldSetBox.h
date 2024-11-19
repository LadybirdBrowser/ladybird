/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class FieldSetBox final : public BlockContainer {
    GC_CELL(FieldSetBox, BlockContainer);
    GC_DECLARE_ALLOCATOR(FieldSetBox);

public:
    FieldSetBox(DOM::Document&, DOM::Element&, CSS::StyleProperties);
    virtual ~FieldSetBox() override;

    DOM::Element& dom_node() { return static_cast<DOM::Element&>(*BlockContainer::dom_node()); }
    DOM::Element const& dom_node() const { return static_cast<DOM::Element const&>(*BlockContainer::dom_node()); }

    void layout_legend() const;

private:
    bool has_rendered_legend() const;
    virtual bool is_fieldset_box() const final
    {
        return true;
    }
};

template<>
inline bool Node::fast_is<FieldSetBox>() const { return is_fieldset_box(); }

}
